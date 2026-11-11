// Winsock before Windows.h (see client_internal.hpp).
#include "client_internal.hpp"

#include "engine_loop.hpp"
#include "../crash/crash_context.hpp"
#include "../characters/frame_metrics.hpp"
#include "../testing/stack_sampler.hpp"

#include "../carts/cart_horses.hpp"
#include "../combat/player_damage.hpp"
#include "../replicas/deferred_moves.hpp"
#include "../replicas/held_items.hpp"
#include "../noclip/free_camera.hpp"
#include "../replicas/replica_spawn.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include "MinHook.h"
#include "client_state.hpp"
#include "log.hpp"
#include "natives.hpp"

namespace kcds::game_client {

Update original_update;
MapCallback map_callback;

namespace {

// The class of the C++ object `object` points to, from its vtable's RTTI (MSVC x64: the complete
// object locator before the vtable, its type descriptor's name at + 16); empty when it is not an
// object of the game's. Read with ReadProcessMemory: a stale pointer does not fault again here.
bool class_of(std::uintptr_t object, char* out, std::size_t size) {
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  const HANDLE self = GetCurrentProcess();
  SIZE_T bytes = 0;
  std::uintptr_t table = 0;
  std::uintptr_t locator = 0;
  std::uint32_t descriptor = 0;
  if (!object ||
      !ReadProcessMemory(self, reinterpret_cast<void*>(object), &table, sizeof(table), &bytes) ||
      table < base || table >= base + image_size ||
      !ReadProcessMemory(self, reinterpret_cast<void*>(table - sizeof(void*)), &locator,
                         sizeof(locator), &bytes) ||
      locator < base || locator >= base + image_size ||
      !ReadProcessMemory(self, reinterpret_cast<void*>(locator + 12), &descriptor,
                         sizeof(descriptor), &bytes) ||
      descriptor == 0 || descriptor >= image_size) {
    return false;
  }
  char name[96] = {};
  if (!ReadProcessMemory(self, reinterpret_cast<void*>(base + descriptor + 16), name,
                         sizeof(name) - 1, &bytes)) {
    return false;
  }
  std::snprintf(out, size, "%s(vt 0x%llx)", name,
                static_cast<unsigned long long>(0x180000000ULL + table - base));
  return true;
}

// The game objects the faulting thread's registers point to, by class (a
// render node drawn with a stale material in World Architect's content pictures).
void log_fault_objects(const CONTEXT& context) {
  const std::pair<const char*, std::uintptr_t> registers[] = {
      {"rax", context.Rax}, {"rbx", context.Rbx}, {"rcx", context.Rcx}, {"rdx", context.Rdx},
      {"rsi", context.Rsi}, {"rdi", context.Rdi}, {"rbp", context.Rbp}, {"r8", context.R8},
      {"r9", context.R9},   {"r10", context.R10}, {"r11", context.R11}, {"r12", context.R12},
      {"r13", context.R13}, {"r14", context.R14}, {"r15", context.R15}};
  for (const auto& [name, value] : registers) {
    char found[160] = {};
    if (class_of(value, found, sizeof(found))) {
      log("Fault object %s=0x%llx %s", name, static_cast<unsigned long long>(value), found);
    }
  }
  // The objects the top of the stack holds (a caller's render node, its material).
  constexpr int kStackSlots = 48;
  for (int slot = 0; slot < kStackSlots; ++slot) {
    std::uintptr_t value = 0;
    SIZE_T bytes = 0;
    const auto at = context.Rsp + std::uintptr_t(slot) * sizeof(std::uintptr_t);
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), &value, sizeof(value),
                           &bytes)) {
      break;
    }
    char found[160] = {};
    if (class_of(value, found, sizeof(found))) {
      log("Fault object [rsp+0x%x]=0x%llx %s", unsigned(slot * sizeof(std::uintptr_t)),
          static_cast<unsigned long long>(value), found);
    }
  }
}

}  // namespace

int engine_exception(EXCEPTION_POINTERS* details) {
  discard_held_hits("engine adapter exception");
  authoritative_replica_soul = nullptr;
  spawning_remote = false;
  replaying_animation = false;
  auto address = reinterpret_cast<std::uintptr_t>(details->ExceptionRecord->ExceptionAddress);
  auto base = reinterpret_cast<std::uintptr_t>(image);
  log("Engine exception code=0x%lx stage=%s address=%p IDA=0x%llx",
      details->ExceptionRecord->ExceptionCode, engine_stage,
      details->ExceptionRecord->ExceptionAddress,
      address >= base && address < base + image_size
          ? static_cast<unsigned long long>(0x180000000ULL + address - base)
          : 0ULL);
  auto context = *details->ContextRecord;
  log("Fault registers rcx=0x%llx rdx=0x%llx", context.Rcx, context.Rdx);
  log_fault_objects(context);
  // The game's code addresses on the faulting thread's stack, the likely callers first: where a
  // fault in a shared handler came from. The scan stays inside this thread's stack, and a stack
  // pointer outside it (a corrupted one) is not read at all (a fault near the top of
  // the stack faulted again here and hid the first one).
  ULONG_PTR stack_low = 0;
  ULONG_PTR stack_high = 0;
  GetCurrentThreadStackLimits(&stack_low, &stack_high);
  {
    const auto* stack = reinterpret_cast<const std::uintptr_t*>(context.Rsp);
    int slots = 0;
    if (context.Rsp >= stack_low && context.Rsp < stack_high) {
      slots = static_cast<int>((stack_high - context.Rsp) / sizeof(std::uintptr_t));
    }
    if (slots > 256) {
      slots = 256;
    }
    char callers[512] = {};
    int written = 0;
    for (int i = 0, found = 0; i < slots && found < 12; ++i) {
      if (stack[i] >= base && stack[i] < base + image_size) {
        written += std::snprintf(callers + written, sizeof(callers) - written, " 0x%llx",
                                 static_cast<unsigned long long>(0x180000000ULL + stack[i] - base));
        ++found;
      }
    }
    log("Fault stack game addresses:%s", callers);
  }
  // Read diagnostics without dereferencing suspect pointers or handling away a fault.
  if (details->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      address - reinterpret_cast<std::uintptr_t>(image) == 0x647e9c) {
    std::uintptr_t object = 0, table = 0, owner = 0;
    SIZE_T bytes = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rdx), &object,
                          sizeof(object), &bytes) &&
        object) {
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(object), &table, sizeof(table),
                        &bytes);
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(object + 24), &owner,
                        sizeof(owner), &bytes);
      log("SPATIAL fault object=%p tableIDA=0x%llx owner=%p", reinterpret_cast<void*>(object),
          table ? 0x180000000ULL + table - reinterpret_cast<std::uintptr_t>(image) : 0ULL,
          reinterpret_cast<void*>(owner));
    }
  }
  // A call through a null pointer faults at 0: the caller's return address is on the stack.
  if (!context.Rip) {
    DWORD64 caller = 0;
    SIZE_T bytes = 0;
    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), &caller,
                          sizeof(caller), &bytes)) {
      context.Rip = caller;
      context.Rsp += sizeof(caller);
    }
  }
  HMODULE own = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&engine_exception), &own);
  log("Fault modules game=%p client=%p", image, static_cast<void*>(own));
  for (unsigned depth = 0; depth < 18 && context.Rip; ++depth) {
    // The unwind reads the stack: a frame outside this thread's stack ends the walk.
    if (context.Rsp < stack_low || context.Rsp + sizeof(DWORD64) > stack_high) {
      break;
    }
    auto pc = context.Rip;
    log("STACK %u pc=0x%llx IDA=0x%llx", depth, pc,
        pc >= base && pc < base + image_size
            ? static_cast<unsigned long long>(0x180000000ULL + pc - base)
            : 0ULL);
    DWORD64 unwind_base = 0;
    auto entry = RtlLookupFunctionEntry(pc, &unwind_base, nullptr);
    if (!entry) {
      // Windows x64 leaf functions need no RUNTIME_FUNCTION: unwind their
      // caller by popping the saved return address, rather than losing the trace.
      DWORD64 caller = 0;
      SIZE_T bytes = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), &caller,
                             sizeof(caller), &bytes) ||
          bytes != sizeof(caller) || !caller)
        break;
      context.Rip = caller;
      context.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, unwind_base, pc, entry, &context, &handler_data,
                     &establisher, nullptr);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}
// The game's functions on the faulting thread's call stack, innermost first, as addresses
// (a fault on one of the game's worker threads named only the address, not who called
// it). The unwind stays inside this thread's stack and ends at the first frame it cannot unwind.
void log_thread_callers(CONTEXT context) {
  ULONG_PTR stack_low = 0;
  ULONG_PTR stack_high = 0;
  GetCurrentThreadStackLimits(&stack_low, &stack_high);
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  char callers[640] = {};
  int written = 0;
  for (unsigned depth = 0; depth < 24 && context.Rip; ++depth) {
    if (context.Rsp < stack_low || context.Rsp + sizeof(DWORD64) > stack_high) {
      break;
    }
    const DWORD64 pc = context.Rip;
    if (pc >= base && pc < base + image_size && written < int(sizeof(callers)) - 24) {
      written += std::snprintf(callers + written, sizeof(callers) - written, " 0x%llx",
                               static_cast<unsigned long long>(0x180000000ULL + pc - base));
    }
    DWORD64 unwind_base = 0;
    auto entry = RtlLookupFunctionEntry(pc, &unwind_base, nullptr);
    if (!entry) {
      // A leaf function: its caller's return address is at the stack pointer.
      DWORD64 caller = 0;
      SIZE_T bytes = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), &caller,
                             sizeof(caller), &bytes) ||
          bytes != sizeof(caller) || !caller) {
        break;
      }
      context.Rip = caller;
      context.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, unwind_base, pc, entry, &context, &handler_data,
                     &establisher, nullptr);
  }
  log("Exception thread %lu callers:%s", GetCurrentThreadId(), callers);
}
// The thread that runs the game's update and our frame (hooked_update).
std::atomic<DWORD> frame_thread{0};
LONG CALLBACK observe_engine_exception(EXCEPTION_POINTERS* details) {
  if (details->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // This handler sees every thread of the game. A fault on another thread (a web view's, the
  // network's) is not in our frame: its stage name would be the frame's last one and wrong,
  // and the frame's state is not this thread's to reset.
  if (GetCurrentThreadId() != frame_thread.load()) {
    // The module the fault is in, and the offset in it, for the build's symbols.
    void* address = details->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(address), &module);
    if (module) {
      GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* file = std::strrchr(name, '\\');
    log("Exception 0x%lx on thread %lu, not the frame's thread %lu: %s+0x%llx",
        details->ExceptionRecord->ExceptionCode, GetCurrentThreadId(), frame_thread.load(),
        file ? file + 1 : name,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) -
                                        reinterpret_cast<std::uintptr_t>(module)));
    if (module == reinterpret_cast<HMODULE>(image)) {
      log_thread_callers(*details->ContextRecord);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  engine_exception(details);
  return EXCEPTION_CONTINUE_SEARCH;
}
// Once the adapter stopped, a NoClip flight ends (once), so the player is not left with a frozen
// camera and held controls.
bool flight_ended_after_failure = false;
void end_flight_after_failure() {
  if (flight_ended_after_failure || !kcds::runtime::noclip::active()) {
    return;
  }
  flight_ended_after_failure = true;
  __try {
    kcds::runtime::noclip::abandon();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log("NOCLIP could not end after the adapter stopped (0x%lx)", GetExceptionCode());
  }
}
void guarded_frame() {
  __try {
    if (!failed)
      frame();
  } __except (engine_exception(GetExceptionInformation())) {
    log("Engine adapter exception 0x%lx; stopped", GetExceptionCode());
    failed = true;
  }
  if (failed) {
    end_flight_after_failure();
  }
}
double engine_frame_started = 0.0;

char __fastcall hooked_update(void* self, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
  engine_frame_started = local::clock_seconds();
  // Pose modifiers go on before the update that animates the bodies (carts/cart_horses.hpp).
  if (!failed) {
    push_cart_reins();
    // Replicas move before the update places what they carry (replicas/deferred_moves.hpp).
    engine_stage = "replica moves";
    apply_replica_moves();
    note_crowd_trace_before_update();
  }
  tick_stack_sampler();
  frame_thread.store(GetCurrentThreadId());
  kcds::runtime::crash::note_frame_thread(GetCurrentThreadId());
  // A fault in the game's own update is told as such, not as the last stage of our frame.
  engine_stage = "game update";
  const auto update_start = kcds::runtime::characters::frame_metrics::stamp();
  auto result = original_update(self, a, b, c);
  const auto native_end = kcds::runtime::characters::frame_metrics::stamp();
  try {
    guarded_frame();
  } catch (const std::exception& e) {
    log("Adapter stopped: %s", e.what());
    discard_held_hits("engine adapter exception");
    authoritative_replica_soul = nullptr;
    failed = true;
  }
  kcds::runtime::characters::frame_metrics::finish(update_start, native_end, failed);
  if (!failed) {
    // What replicas hold in their hands, drawn with their skeletons (replicas/held_items.hpp).
    engine_stage = "replica held items";
    tick_replica_held_items();
    engine_stage = "crowd trace";
    tick_crowd_trace();
  }
  return result;
}

DWORD install_engine_loop_hook() {
  if (MH_CreateHook(update_target, reinterpret_cast<void*>(hooked_update),
                    reinterpret_cast<void**>(&original_update)) != MH_OK)
    return 1;
  if (MH_EnableHook(update_target) != MH_OK)
    return 1;
  return 0;
}

}  // namespace kcds::game_client
