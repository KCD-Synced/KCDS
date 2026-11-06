// Windows/CryEngine engine glue — REAL implementation, compiled only in a Windows client build
// (CMake gate: KCD2S_BUILD_CLIENT AND WIN32) and injected into WHGame.dll. It is never built on the
// headless host. Values marked TODO(env) need a live pass or a running game to finalize (the
// open questions in). Everything else is production-shaped.
//
// References:,
#include "kcd2s/bridge/engine_win.hpp"

#if defined(_WIN32)

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <unordered_map>

#include "MinHook.h"  // provided by the Windows client build (vendored/fetched)
#include "kcd2s/bridge/resolver.hpp"

namespace kcd2s::bridge {
namespace {

// ---- module image -----------------------------------------------------------------------------
std::uint8_t* g_image = nullptr;
std::size_t g_image_size = 0;

bool locate_module() {
  // Shipped engine module. TODO(env): confirm the exact name on the retail build (typically
  // "WHGame.dll").
  HMODULE mod = GetModuleHandleW(L"WHGame.dll");
  if (!mod)
    mod = GetModuleHandleW(L"WHGame-155.dll");
  if (!mod)
    return false;
  MODULEINFO mi{};
  if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
    return false;
  g_image = reinterpret_cast<std::uint8_t*>(mod);
  g_image_size = mi.SizeOfImage;
  return true;
}

// ---- resolved engine entry points -------------------------------------------------------------
// Resolved by signature so they survive per-patch address shifts.
// CEntity::SetWorldTM — WHGame 1.5.5 CEntity vtable slot 30 (@). Takes a 3x4 row-major
// Matrix34 (float[12]) AND a uint flags word (0 = normal apply; 0x80000 = skip parent transform).
// The live decompile shows the 3rd arg in r8d — the old 2-arg prototype was wrong.
using SetWorldTM_t = void(__fastcall*)(void* entity, const float* matrix34, unsigned int flags);
// CEntitySystem::GetEntity — WHGame 1.5.5 CEntitySystem vtable slot 14 (@). EntityId is
// 32-bit: index = id & 0x3FFFF, salt = id >> 18 (confirmed live, not stock CryEngine layout).
using GetEntity_t = void*(__fastcall*)(void* entitySystem, std::uint32_t id);

SetWorldTM_t g_SetWorldTM = nullptr;
GetEntity_t g_GetEntity = nullptr;
void* g_entitySystem = nullptr;  // the engine's live CEntitySystem* (== gEnv->pEntitySystem)

Resolver make_resolver() {
  Resolver r;
  // Named targets the glue calls directly. All three below are now RESOLVED from a live pass on
  // WHGame.dll 1.5.5 (sha256 bdf8f9e4…, image base) —
  // TARGETS.md §1–3. The byte signatures are the real function prologues / reader instruction.
  //
  // pEntitySystem: the engine keeps its CEntitySystem* in a direct global (named g_pEntitySystem
  // @), used by ~100 engine fns — cleaner than gEnv+offset (no struct layout guess). The module
  // init `*(gEnv+0xA0) = entitySystem` (CEngineModule_EntitySystem::Initialize @) also confirms
  // gEnv->pEntitySystem is at offset 0xA0 if the gEnv route is ever preferred. Here we resolve the
  // `mov rcx, cs:g_pEntitySystem` reader (from the tiny accessor @) and read the pointer out of the
  // global slot. The mov rcx,[rip] starts at match+6 and is 7 bytes, so RIP is at match+13 ->
  // insn_len=13 (verified live: insn_len=7 undershot the global by 6 bytes and read garbage).
  r.add("g_pEntitySystem", "44 8A C2 48 8B D1 48 8B 0D ?? ?? ?? ??", /*disp_at=*/9,
        /*insn_len=*/13);
  // CEntity::SetWorldTM — CEntity vtable slot 30 (@). Converts world->local via the
  // parent TM then calls the internal local setter (slot 31 @). The bare prologue
  // matched 2 sites live, so this is extended through the `mov rdx,rdi; mov r8d,esi; mov rcx,rbx`
  // call setup to be unique; the two E8 call targets are wildcarded.
  r.add("CEntity::SetWorldTM",
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D 68 A1 48 81 EC 90 00 00 00 0F 29 70 "
        "E8 41 8B F0 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B D7 44 8B C6 48 8B CB");
  // CEntitySystem::GetEntity — CEntitySystem vtable slot 14 (@). Confirmed: slot 16
  // FindEntityByName calls vtable[14] with the resolved id. The `lock xadd [rcx+0x120]` reader-lock
  // makes the prologue unique.
  r.add("CEntitySystem::GetEntity",
        "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 B8 01 00 00 00 F0 0F C1 81 20 01 00 00");
  return r;
}

// gEnv->pEntitySystem is at offset 0xA0 (confirmed live, WHGame 1.5.5), but we don't need the gEnv
// struct at all: the engine exposes a direct global g_pEntitySystem holding the live
// CEntitySystem*. We resolve that global's slot by signature and read the pointer out of it.
constexpr std::size_t kGEnv_pEntitySystem_off = 0xA0;  // for the gEnv route

bool resolve_engine() {
  Resolver r = make_resolver();
  auto esGlobal =
      r.resolve("g_pEntitySystem", g_image, g_image_size);  // slot of the CEntitySystem* global
  auto setTM = r.resolve("CEntity::SetWorldTM", g_image, g_image_size);
  auto getEnt = r.resolve("CEntitySystem::GetEntity", g_image, g_image_size);
  if (!esGlobal || !setTM || !getEnt)
    return false;

  g_entitySystem = *reinterpret_cast<void**>(g_image + *esGlobal);
  if (!g_entitySystem)
    return false;  // engine not up yet — retry on a later frame
  g_SetWorldTM = reinterpret_cast<SetWorldTM_t>(g_image + *setTM);
  g_GetEntity = reinterpret_cast<GetEntity_t>(g_image + *getEnt);
  return g_entitySystem && g_SetWorldTM && g_GetEntity;
}

// ---- Present hook -----------------------------------------------------------------------------
using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
Present_t g_origPresent = nullptr;
EngineBridge* g_activeBridge = nullptr;
const replication::SchemaRegistry* g_schema = nullptr;
const replication::Mirror* g_mirror = nullptr;

HRESULT __stdcall hooked_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (g_activeBridge && g_schema && g_mirror)
    g_activeBridge->apply_frame(*g_schema, *g_mirror);
  return g_origPresent(sc, sync, flags);
}

// Read IDXGISwapChain::Present from a throwaway device/swapchain vtable (the robust, version-
// independent way to find the function to hook).
void* find_present() {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"kcd2s_dummy";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8, nullptr,
                            nullptr, wc.hInstance, nullptr);

  DXGI_SWAP_CHAIN_DESC scd{};
  scd.BufferCount = 1;
  scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.OutputWindow = hwnd;
  scd.SampleDesc.Count = 1;
  scd.Windowed = TRUE;

  IDXGISwapChain* sc = nullptr;
  ID3D11Device* dev = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  void* present = nullptr;
  if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                              D3D11_SDK_VERSION, &scd, &sc, &dev, nullptr,
                                              nullptr))) {
    void** vtbl = *reinterpret_cast<void***>(sc);
    present = vtbl[8];  // IDXGISwapChain::Present is vtable slot 8
    sc->Release();
    dev->Release();
  }
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return present;
}

std::unordered_map<core::EntityId, std::uint64_t>& engine_ids() {
  static std::unordered_map<core::EntityId, std::uint64_t> m;
  return m;
}

// Build a CryEngine Matrix34 (row-major 3x4, float[12]) from position + unit quaternion.
void make_matrix34(const core::Vec3& p, const core::Quat& q, float m[12]) {
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  m[0] = 1 - 2 * (yy + zz);
  m[1] = 2 * (xy - wz);
  m[2] = 2 * (xz + wy);
  m[3] = p.x;
  m[4] = 2 * (xy + wz);
  m[5] = 1 - 2 * (xx + zz);
  m[6] = 2 * (yz - wx);
  m[7] = p.y;
  m[8] = 2 * (xz - wy);
  m[9] = 2 * (yz + wx);
  m[10] = 1 - 2 * (xx + yy);
  m[11] = p.z;
}

}  // namespace

bool EngineBridge::initialize() {
  if (ready_)
    return true;
  if (!locate_module())
    return false;
  if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
    return false;
  if (!resolve_engine())
    return false;

  void* present = find_present();
  if (!present)
    return false;
  if (MH_CreateHook(present, reinterpret_cast<void*>(&hooked_present),
                    reinterpret_cast<void**>(&g_origPresent)) != MH_OK)
    return false;
  if (MH_EnableHook(present) != MH_OK)
    return false;

  g_activeBridge = this;
  ready_ = true;
  return true;
}

void EngineBridge::shutdown() {
  if (!ready_)
    return;
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
  g_activeBridge = nullptr;
  g_schema = nullptr;
  g_mirror = nullptr;
  ready_ = false;
}

void EngineBridge::bind_entity(core::EntityId ours, std::uint64_t engine_id) {
  engine_ids()[ours] = engine_id;
}
void EngineBridge::unbind_entity(core::EntityId ours) {
  engine_ids().erase(ours);
}

void EngineBridge::apply_frame(const replication::SchemaRegistry& schema,
                               const replication::Mirror& mirror) {
  // Stash for the Present detour, and apply now (the detour calls this each frame).
  g_schema = &schema;
  g_mirror = &mirror;
  if (!g_SetWorldTM || !g_GetEntity || !g_entitySystem)
    return;

  for (const TransformApply& t : extract_transforms(schema, mirror)) {
    auto it = engine_ids().find(t.entity);
    if (it == engine_ids().end())
      continue;  // not bound to an engine entity yet
    void* entity = g_GetEntity(g_entitySystem, it->second);
    if (!entity)
      continue;
    float m[12];
    make_matrix34(t.has_position ? t.position : core::Vec3{},
                  t.has_rotation ? t.rotation : core::Quat{}, m);
    g_SetWorldTM(entity, m,
                 0);  // flags=0: normal world-TM apply (propagates to physics + children)
  }
}

}  // namespace kcd2s::bridge

#endif  // _WIN32
