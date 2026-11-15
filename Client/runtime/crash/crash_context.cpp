#include "crash_context.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace kcds::game_client {
// The stage of the mod's frame (core/client_internal.hpp).
extern const char* engine_stage;
}  // namespace kcds::game_client

namespace kcds::runtime::crash {
namespace {

constexpr std::size_t kTextBytes = 64;
constexpr int kMostFrames = 32;
// GlitchTip keeps a tag's value up to 200 characters.
constexpr std::size_t kMostTagBytes = 200;

// Written by the frame's thread, read only while the process crashes: plain buffers, no allocation.
char last_op[kTextBytes] = "";
char last_resource[kTextBytes] = "";
std::atomic<std::uint64_t> last_op_ms{0};
std::atomic<std::uint32_t> frame_thread{0};
char last_overlay[kTextBytes] = "";
std::atomic<std::uint64_t> last_overlay_ms{0};

// The DLLs a graphics layer stands in for: loaded from outside Windows' own folder they are a
// proxy (OptiScaler as dxgi.dll, winmm.dll or version.dll; ReShade as dxgi.dll).
constexpr const wchar_t* kProxyNames[] = {L"dxgi.dll",   L"d3d12.dll",   L"d3d11.dll",
                                          L"winmm.dll",  L"version.dll", L"dinput8.dll",
                                          L"wininet.dll"};
// Modules of frame generation, upscalers and overlays that draw into or present the game's frames.
constexpr const wchar_t* kLayerModules[] = {
    L"libxess_fg.dll",          L"libxess.dll",
    L"nvngx_dlssg.dll",         L"nvngx_dlss.dll",
    L"sl.dlss_g.dll",           L"sl.interposer.dll",
    L"amd_fidelityfx_dx12.dll", L"ffx_frameinterpolation_x64.dll",
    L"OptiScaler.dll",          L"ReShade64.dll",
    L"RTSSHooks64.dll",         L"gameoverlayrenderer64.dll",
    L"DiscordHook64.dll",       L"EOSOverlayRenderer-Win64-Shipping.dll"};

void copy_text(char* to, const std::string& from) {
  const std::size_t count = from.size() < kTextBytes - 1 ? from.size() : kTextBytes - 1;
  std::memcpy(to, from.data(), count);
  to[count] = '\0';
}

// "module+0xoffset" of one address, or the bare address outside every module.
std::string where(DWORD64 address) {
  HMODULE module = nullptr;
  char text[MAX_PATH + 32];
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(address), &module) &&
      module) {
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    std::snprintf(text, sizeof(text), "%s+0x%llx", name ? name + 1 : path,
                  static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
  } else {
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
  }
  return text;
}

// The first frames of the stack, "a < b < c", as many as fit in one tag.
std::string top_frames(const std::string& stack) {
  std::string top;
  std::size_t start = 0;
  while (start < stack.size()) {
    std::size_t end = stack.find('\n', start);
    if (end == std::string::npos) {
      end = stack.size();
    }
    const std::string frame = stack.substr(start, end - start);
    const std::string longer = top.empty() ? frame : top + " < " + frame;
    if (longer.size() > kMostTagBytes) {
      break;
    }
    top = longer;
    start = end + 1;
  }
  return top;
}

}  // namespace

void note_script(const std::string& op, const std::string& resource) {
  copy_text(last_op, op);
  copy_text(last_resource, resource);
  last_op_ms.store(GetTickCount64());
}

void note_frame_thread(std::uint32_t thread_id) {
  frame_thread.store(thread_id);
}

void note_overlay(const std::string& what) {
  copy_text(last_overlay, what);
  last_overlay_ms.store(GetTickCount64());
}

std::string graphics_layers() {
  std::string layers;
  const auto add = [&layers](const std::string& text) {
    layers += layers.empty() ? text : ", " + text;
  };
  wchar_t system[MAX_PATH] = {};
  const UINT system_length = GetSystemDirectoryW(system, MAX_PATH);
  for (const wchar_t* name : kProxyNames) {
    HMODULE module = GetModuleHandleW(name);
    wchar_t path[MAX_PATH] = {};
    if (!module || GetModuleFileNameW(module, path, MAX_PATH) == 0) {
      continue;
    }
    // Windows' own copy lies in the system folder; anything else stands in for it.
    if (system_length > 0 && _wcsnicmp(path, system, system_length) == 0) {
      continue;
    }
    char narrow[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
    // The folder's last part only: the rest of the path names the player's own folders.
    std::string where_from = narrow;
    const std::size_t file = where_from.find_last_of("\\/");
    const std::string folder = file == std::string::npos ? "" : where_from.substr(0, file);
    const std::size_t parent = folder.find_last_of("\\/");
    char name_text[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, name_text, sizeof(name_text), nullptr, nullptr);
    add(std::string(name_text) + " proxy in " +
        (parent == std::string::npos ? folder : folder.substr(parent + 1)));
  }
  for (const wchar_t* name : kLayerModules) {
    if (GetModuleHandleW(name)) {
      char name_text[64] = {};
      WideCharToMultiByte(CP_UTF8, 0, name, -1, name_text, sizeof(name_text), nullptr, nullptr);
      add(name_text);
    }
  }
  return layers;
}

void annotate(const void* exception_pointers,
              std::vector<std::pair<std::string, std::string>>& tags, std::string& stack) {
  const bool on_frame_thread = GetCurrentThreadId() == frame_thread.load();
  tags.emplace_back("kcds.frame_thread", on_frame_thread ? "yes" : "no");
  tags.emplace_back("kcds.stage",
                    on_frame_thread ? kcds::game_client::engine_stage : "other thread");
  if (last_op[0] != '\0') {
    tags.emplace_back("kcds.script_op", last_op);
    tags.emplace_back("kcds.script_resource", last_resource[0] != '\0' ? last_resource : "?");
    tags.emplace_back("kcds.script_op_age_ms",
                      std::to_string(GetTickCount64() - last_op_ms.load()));
  }
  const std::string layers = graphics_layers();
  tags.emplace_back("kcds.graphics_layers",
                    layers.empty() ? "none" : layers.substr(0, kMostTagBytes));
  if (last_overlay[0] != '\0') {
    tags.emplace_back("kcds.overlay", last_overlay);
    tags.emplace_back("kcds.overlay_age_ms",
                      std::to_string(GetTickCount64() - last_overlay_ms.load()));
  }

  const auto* pointers = static_cast<const EXCEPTION_POINTERS*>(exception_pointers);
  if (!pointers || !pointers->ContextRecord) {
    return;
  }
  // The crashing thread's own stack, walked with the modules' unwind tables.
  CONTEXT context = *pointers->ContextRecord;
  for (int frame = 0; frame < kMostFrames && context.Rip != 0; ++frame) {
    stack += where(context.Rip);
    stack += '\n';
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!entry) {
      // A leaf function: its return address is at the stack pointer.
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data,
                     &establisher, nullptr);
  }
  // GlitchTip keeps only the tags of a minidump's event, not its extra: the top of the stack is
  // a tag too. The whole stack is in the attached client log (kcds.logs).
  if (!stack.empty()) {
    tags.emplace_back("kcds.stack_top", top_frames(stack));
  }
}

}  // namespace kcds::runtime::crash
