// KCD2-Synced bridge SMOKE TEST — a throwaway diagnostic DLL (NOT the client).
//
// Purpose: prove the signature-resolved WHGame.dll engine surface actually works inside the running
// game, staged so risk ramps up and you can stop at the first failure. It is injected into KCD2,
// hooks Present, and writes everything to %LOCALAPPDATA%\KCD2-Synced\smoketest.log.
//
// Stages ("RESOLVED" section):
//   S0  inject + Present hook            — logs "alive", proves injection + hook.           [no
//   engine calls] S1  signature validation             — each sig's MATCH COUNT (uniqueness!) +
//   address;
//                                          resolve g_pEntitySystem, check non-null.         [no
//                                          engine calls]
//   S2  read path (safe)                 — walk the entity array, pick a live entity, call
//                                          GetEntity(id) and assert it returns the SAME pointer
//                                          (validates GetEntity + the EntityId layout + ABI),
//                                          then log its stored position. [read-only]
//   S3  one write, on F8 (the risky bit) — SetWorldTM() to nudge the target +2 m up; logs the
//                                          position before/after and whether it faulted.      [one
//                                          mutation]
//
// Threading: every engine call happens inside the Present detour — the realistic bridge thread. If
// S2 reads fine but S3 writes crash, that is the signal that mutations need the main/game thread
// (next step: find an OnPostUpdate-style main-thread hook instead of Present). SEH wraps the risky
// calls so a fault is logged, not silently lost.
//
// Windows x64 only. Build: -DKCD2S_BUILD_CLIENT=ON -DKCD2S_BUILD_SMOKETEST=ON (see CMakeLists.txt).

#if defined(_WIN32)

#define _CRT_SECURE_NO_WARNINGS 1  // getenv("LOCALAPPDATA") is fine here (diagnostic DLL)

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <share.h>  // _fsopen / _SH_DENYNO — share the log so it can be read while the game runs

#include "MinHook.h"
#include "kcd2s/bridge/resolver.hpp"
#include "kcd2s/bridge/sigscan.hpp"

using kcd2s::bridge::knpos;
using kcd2s::bridge::Resolver;
using kcd2s::bridge::scan;
using kcd2s::bridge::Signature;

namespace {

// ---- logging -----------------------------------------------------------------------------------
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;

void log_open() {
  InitializeCriticalSection(&g_logLock);
  char path[MAX_PATH] = {0};
  const char* base = getenv("LOCALAPPDATA");
  if (base) {
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\KCD2-Synced", base);
    CreateDirectoryA(dir, nullptr);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\smoketest.log", dir);
  } else {
    _snprintf_s(path, sizeof(path), _TRUNCATE, "C:\\kcd2s_smoketest.log");
  }
  g_log = _fsopen(path, "w", _SH_DENYNO);  // shared: another process may read while we write
}

void logf(const char* fmt, ...) {
  if (!g_log)
    return;
  EnterCriticalSection(&g_logLock);
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fprintf(g_log, "\n");
  fflush(g_log);
  LeaveCriticalSection(&g_logLock);
}

// ---- module image
// --------------------------------------------------------------------------------
std::uint8_t* g_image = nullptr;
std::size_t g_imageSize = 0;

bool locate_module() {
  HMODULE mod = GetModuleHandleW(L"WHGame.dll");  // confirmed retail name (get_metadata)
  if (!mod)
    return false;
  MODULEINFO mi{};
  if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
    return false;
  g_image = reinterpret_cast<std::uint8_t*>(mod);
  g_imageSize = mi.SizeOfImage;
  return true;
}

// ---- resolved engine surface
// --------------------------------------------------------------------- WHGame.dll 1.5.5 (sha256
// bdf8f9e4…) — all resolved by signature, never baked addresses.
using GetEntity_t = void*(__fastcall*)(void* entitySystem, std::uint32_t id);  // vtable slot 14
using SetWorldTM_t = void(__fastcall*)(void* entity, const float* m34,
                                       std::uint32_t flags);  // slot 30

GetEntity_t g_GetEntity = nullptr;
SetWorldTM_t g_SetWorldTM = nullptr;
void* g_entitySystem = nullptr;           // *(CEntitySystem**) from the g_pEntitySystem global
std::uint32_t* g_localActorId = nullptr;  // best-effort; may be null if the (weak) sig misses

// Entity-system / entity layout recovered live (CEntitySystem::GetEntity body @):
//   array at this+0x200, stride 24; salt @ +0 (word), IEntity* @ +16; entity->id @ +12.
//   EntityId: index = id & 0x3FFFF, salt = id >> 18.
constexpr std::size_t kArrOff = 0x200, kStride = 24, kPtrOff = 16, kMaxIdx = 0x3FFFD;
constexpr std::size_t kEntIdOff = 12;
constexpr std::size_t kEntPosOff = 48;  // stored local translation x,y,z (from GetWorldTM body)

Resolver make_resolver() {
  Resolver r;
  // mov rcx, cs:g_pEntitySystem  (reader @) — disp wildcarded, so it tracks per patch.
  // The mov starts at match+6 and is 7 bytes, so RIP (end of insn) is at match+13 -> insn_len=13.
  r.add("g_pEntitySystem", "44 8A C2 48 8B D1 48 8B 0D ?? ?? ?? ??", /*disp_at=*/9,
        /*insn_len=*/13);
  // CEntity::SetWorldTM prologue + arg setup (vtable slot 30 @). Extended past the
  // prologue (which alone matched 2 sites) through the `mov rdx,rdi; mov r8d,esi; mov rcx,rbx` call
  // setup — now unique. The two E8 call targets are wildcarded so it survives per-patch shifts.
  r.add("CEntity::SetWorldTM",
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D 68 A1 48 81 EC 90 00 00 00 0F 29 70 "
        "E8 41 8B F0 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B D7 44 8B C6 48 8B CB");
  // CEntitySystem::GetEntity prologue (vtable slot 14 @) — the lock xadd [rcx+0x120] is
  // unique.
  r.add("CEntitySystem::GetEntity",
        "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 B8 01 00 00 00 F0 0F C1 81 20 01 00 00");
  // Best-effort: lea rdx, g_localActorId (@). lea patterns are weak, hence the tail.
  r.add("g_localActorId", "48 8D 15 ?? ?? ?? ?? 48 8B CB 85 FF 0F 84", /*disp_at=*/3,
        /*insn_len=*/7);
  return r;
}

// Count matches across the image (capped) — the uniqueness check S1 reports.
std::size_t count_matches(const Signature& s, std::size_t cap) {
  std::size_t n = 0, off = 0;
  while (off < g_imageSize) {
    const std::size_t m = scan(g_image + off, g_imageSize - off, s);
    if (m == knpos)
      break;
    if (++n >= cap)
      break;
    off += m + 1;
  }
  return n;
}

// ---- safe memory reads (SEH; return false on fault) ---------------------------------------------
bool read_ptr(const void* at, void** out) {
  __try {
    *out = *reinterpret_cast<void* const*>(at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool read_u32(const void* at, std::uint32_t* out) {
  __try {
    *out = *reinterpret_cast<const std::uint32_t*>(at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool read_vec3(const void* at, float out[3]) {
  __try {
    out[0] = reinterpret_cast<const float*>(at)[0];
    out[1] = reinterpret_cast<const float*>(at)[1];
    out[2] = reinterpret_cast<const float*>(at)[2];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- the stages
// ----------------------------------------------------------------------------------
void* g_target = nullptr;  // entity chosen for the S3 write
std::uint32_t g_targetId = 0;
std::size_t g_esSlotOff = knpos;  // image offset of the g_pEntitySystem slot (read each F8)

bool resolve_all() {
  Resolver r = make_resolver();
  auto es = r.resolve("g_pEntitySystem", g_image, g_imageSize);
  auto setTM = r.resolve("CEntity::SetWorldTM", g_image, g_imageSize);
  auto getEnt = r.resolve("CEntitySystem::GetEntity", g_image, g_imageSize);
  auto laid = r.resolve("g_localActorId", g_image, g_imageSize);

  // S1: report match counts (uniqueness) + resolved addresses.
  logf("S1: sig match counts (want exactly 1 each):");
  Resolver rr = make_resolver();  // re-parse to get the Signatures for counting
  // (Resolver doesn't expose its sigs; re-parse the three code sigs here for the count.)
  auto cnt = [&](const char* name, const char* pat, int da, int il) {
    auto sig = Signature::parse(pat);
    std::size_t c = sig ? count_matches(*sig, 4) : 0;
    logf("   %-26s matches=%zu", name, c);
  };
  cnt("g_pEntitySystem", "44 8A C2 48 8B D1 48 8B 0D ?? ?? ?? ??", 9, 13);
  cnt("CEntity::SetWorldTM",
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D 68 A1 48 81 EC 90 00 00 00 0F 29 70 "
      "E8 "
      "41 8B F0 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 8B D7 44 8B C6 48 8B CB",
      -1, 0);
  cnt("CEntitySystem::GetEntity",
      "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 B8 01 00 00 00 F0 0F C1 81 20 01 00 00", -1, 0);

  logf("S1: resolved: g_pEntitySystem=%s SetWorldTM=%s GetEntity=%s localActorId=%s",
       es ? "ok" : "MISS", setTM ? "ok" : "MISS", getEnt ? "ok" : "MISS",
       laid ? "ok" : "miss(opt)");
  if (!es || !setTM || !getEnt)
    return false;

  g_SetWorldTM = reinterpret_cast<SetWorldTM_t>(g_image + *setTM);
  g_GetEntity = reinterpret_cast<GetEntity_t>(g_image + *getEnt);
  g_esSlotOff = *es;
  if (laid)
    g_localActorId = reinterpret_cast<std::uint32_t*>(g_image + *laid);
  logf("S1: SetWorldTM @ image+0x%zx  GetEntity @ image+0x%zx  g_pEntitySystem slot @ image+0x%zx",
       *setTM, *getEnt, *es);

  void* esPtr = nullptr;
  if (read_ptr(g_image + *es, &esPtr) && esPtr)
    logf("S1: CEntitySystem* = %p (engine up)", esPtr);
  else
    logf("S1: CEntitySystem* null now (engine/level not up yet — will read on F8)");
  return true;  // signatures resolved; the live entity-system pointer is read on F8
}

// Walk the entity array; validate GetEntity; choose the S3 target (player if we can, else first
// live).
bool read_path() {
  std::uint32_t wantId = 0;
  if (g_localActorId)
    read_u32(g_localActorId, &wantId);
  logf("S2: local actor id (best-effort) = 0x%08x", wantId);

  auto* base = reinterpret_cast<std::uint8_t*>(g_entitySystem) + kArrOff;
  void* firstLive = nullptr;
  std::uint32_t firstLiveId = 0;
  void* player = nullptr;
  int liveSeen = 0;

  for (std::size_t i = 1; i <= kMaxIdx; ++i) {
    void* ent = nullptr;
    if (!read_ptr(base + i * kStride + kPtrOff, &ent) || !ent)
      continue;
    std::uint32_t id = 0;
    if (!read_u32(reinterpret_cast<std::uint8_t*>(ent) + kEntIdOff, &id))
      continue;
    if (!firstLive) {
      firstLive = ent;
      firstLiveId = id;
    }
    if (wantId && id == wantId) {
      player = ent;
      break;
    }
    if (++liveSeen >= 20000)
      break;  // enough; don't walk all 262k if no player match
  }

  if (!firstLive) {
    logf("S2: no live entities found in the array — aborting (is a level loaded?)");
    return false;
  }
  logf("S2: first live entity %p id=0x%08x ; liveSeen>=%d", firstLive, firstLiveId, liveSeen);

  // Cross-check GetEntity(id) == the pointer we found by walking (validates the call + id layout).
  void* viaCall = nullptr;
  __try {
    viaCall = g_GetEntity(g_entitySystem, firstLiveId);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    logf("S2: GetEntity() FAULTED — ABI or threading problem");
    return false;
  }
  logf("S2: GetEntity(0x%08x) = %p  (walk gave %p)  -> %s", firstLiveId, viaCall, firstLive,
       viaCall == firstLive ? "MATCH ✓ (GetEntity validated)" : "MISMATCH ✗");

  g_target = player ? player : firstLive;
  g_targetId = player ? wantId : firstLiveId;
  float pos[3];
  if (read_vec3(reinterpret_cast<std::uint8_t*>(g_target) + kEntPosOff, pos))
    logf("S2: S3 target = %s %p id=0x%08x storedPos=(%.2f, %.2f, %.2f)",
         player ? "LOCAL PLAYER" : "first live entity", g_target, g_targetId, pos[0], pos[1],
         pos[2]);
  logf("S2: read path OK -> running S3 write now.");
  return true;
}

void do_write_test() {
  if (!g_target || !g_SetWorldTM)
    return;
  float before[3] = {0, 0, 0}, after[3] = {0, 0, 0};
  read_vec3(reinterpret_cast<std::uint8_t*>(g_target) + kEntPosOff, before);

  // World Matrix34 (row-major float[12]): identity rotation, translation = before + (0,0,+2).
  float m[12] = {1, 0, 0, before[0], 0, 1, 0, before[1], 0, 0, 1, before[2] + 2.0f};

  logf("S3: calling SetWorldTM(%p, +2m up, flags=0)  targetId=0x%08x ...", g_target, g_targetId);
  bool faulted = false;
  __try {
    g_SetWorldTM(g_target, m, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    faulted = true;
  }
  read_vec3(reinterpret_cast<std::uint8_t*>(g_target) + kEntPosOff, after);
  if (faulted)
    logf("S3: SetWorldTM FAULTED — likely a wrong-thread write. before=(%.2f,%.2f,%.2f)", before[0],
         before[1], before[2]);
  else
    logf("S3: SetWorldTM returned OK. before=(%.2f,%.2f,%.2f) after=(%.2f,%.2f,%.2f) %s", before[0],
         before[1], before[2], after[0], after[1], after[2],
         (after[2] - before[2] > 1.0f) ? "-> MOVED ✓" : "(no stored-pos delta; check in-game)");
}

// ---- Present hook
// --------------------------------------------------------------------------------
using Present_t = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
Present_t g_origPresent = nullptr;
bool g_scanDone = false;  // S1 attempted
bool g_scanOk = false;    // S1 signatures resolved
int g_frame = 0;
bool g_f8WasDown = false;

HRESULT __stdcall hooked_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (++g_frame == 1)
    logf("S0: Present hook alive (frame 1, thread %lu)", GetCurrentThreadId());

  // S1 runs ONCE after a short warmup (signature scan is expensive — never per frame).
  if (!g_scanDone && g_frame >= 60) {
    g_scanDone = true;
    g_scanOk = resolve_all();
    logf(g_scanOk ? "S1: signatures resolved. Load a save, get in-world, then press F8 for S2+S3."
                  : "S1: FATAL — signature resolve failed (see counts above).");
  }

  // S2+S3 on rising-edge F8 — on demand, so the entity walk only runs when you're in-world.
  const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
  if (g_scanOk && down && !g_f8WasDown) {
    logf("F8: running S2 (read) + S3 (write)…");
    void* es = nullptr;
    if (read_ptr(g_image + g_esSlotOff, &es) && es) {
      g_entitySystem = es;
      if (read_path())
        do_write_test();
    } else {
      logf("F8: CEntitySystem* is null — is a save actually loaded?");
    }
  }
  g_f8WasDown = down;

  return g_origPresent(sc, sync, flags);
}

// Find IDXGISwapChain::Present (vtable slot 8) off a throwaway device — version-independent.
void* find_present() {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"kcd2s_smoke_dummy";
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

  IDXGISwapChain* swap = nullptr;
  ID3D11Device* dev = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  void* present = nullptr;
  if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                              D3D11_SDK_VERSION, &scd, &swap, &dev, nullptr,
                                              nullptr))) {
    void** vtbl = *reinterpret_cast<void***>(swap);
    present = vtbl[8];
    swap->Release();
    dev->Release();
  }
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return present;
}

DWORD WINAPI init_thread(LPVOID) {
  log_open();
  logf("=== KCD2-Synced bridge smoke test ===");
  if (!locate_module()) {
    logf("FATAL: WHGame.dll not found in process");
    return 1;
  }
  logf("S0: WHGame.dll @ %p size=0x%zx", g_image, g_imageSize);

  if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
    logf("FATAL: MH_Initialize failed");
    return 1;
  }
  void* present = find_present();
  if (!present) {
    logf("FATAL: could not obtain IDXGISwapChain::Present");
    return 1;
  }
  logf("S0: Present fn @ %p", present);
  if (MH_CreateHook(present, reinterpret_cast<void*>(&hooked_present),
                    reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
      MH_EnableHook(present) != MH_OK) {
    logf("FATAL: MinHook hook/enable failed");
    return 1;
  }
  logf("S0: Present hook installed — waiting for frames…");
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
  }
  return TRUE;
}

#endif  // _WIN32
