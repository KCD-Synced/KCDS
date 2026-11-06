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
