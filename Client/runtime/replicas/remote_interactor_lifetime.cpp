#include "remote_interactor_lifetime.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "remote_interactor_scope.hpp"
#include "remote_ui_role.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace kcds::game_client {
namespace {

// Live, WHGame 1.5.5. These full methods and their relative operands are resolved
// uniquely. Absolute addresses in comments are evidence, never resolver inputs.
constexpr char kInteractorCtor[] =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8D 68 A1 48 81 EC 90 "
    "00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 41 83 CE FF";
constexpr char kInteractorDelete[] =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 9C 75 F1 FE F6 C3 01 74 1A 48 85 FF "
    "74 15 48 8B 05 ?? ?? ?? ?? 33 D2 48 8B CF FF D0";
constexpr char kBarrierCtor[] =
    "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 C7 41 08 00 00 00 00 48 89 01 48 8B D9 "
    "C6 41 10 00 E8 ?? ?? ?? ?? 48 8B 50 40 48 8B C3 48 89 53 18 C7 43 20 00 00 00 00";
constexpr char kBarrierDelete[] =
    "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 F6 C2 01 74 0A BA 28 00 00 00 "
    "E8 C2 BC 0E 00 48 8B C3 48 83 C4 20 5B C3";
constexpr char kHideCursors[] =
    "48 89 5C 24 10 48 89 4C 24 08 57 48 83 EC 20 48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? "
    "48 8D 54 24 30 48 8B 01 FF 50 70 48 8D 4C 24 30 48 8B 38";
constexpr char kInputDtor[] =
    "48 89 5C 24 18 56 57 41 56 48 83 EC 20 4C 8D 71 08 48 8B F1 48 8D 05 ?? ?? ?? ?? "
    "48 8B D6 48 89 01 48 8D 05 ?? ?? ?? ?? 49 89 06 48 8B 41 10";
constexpr char kClearAction[] =
    "48 81 EC 88 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 70 C6 44 24 28 00 "
    "48 83 C1 F8 44 88 4C 24 20 4C 8D 4C 24 30 48 C7 44 24 68 00 00 00 00";
constexpr char kActorRelease[] =
    "40 53 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 A0 00 00 00 "
    "4C 8B 05 ?? ?? ?? ??";

using Constructor = void*(__fastcall*)(void*);
using SignalRegistration = std::int64_t(__fastcall*)(void*, const void*);
using DeletingDestructor = void*(__fastcall*)(void*, unsigned char);
using Unary = std::int64_t(__fastcall*)(void*);
using ClearAction = std::int64_t(__fastcall*)(void*, const void*, const void*, unsigned char);
using Services = void*(__fastcall*)();

Constructor original_interactor_ctor = nullptr;
SignalRegistration original_register_signal = nullptr;
DeletingDestructor delete_interactor = nullptr;
DeletingDestructor delete_barrier = nullptr;
Unary original_hide_cursors = nullptr;
Unary original_input_dtor = nullptr;
ClearAction original_clear_action = nullptr;
Unary actor_release = nullptr;
Services services = nullptr;
void* interactor_vtable = nullptr;
void* barrier_vtable = nullptr;
void* interaction_vtables[3]{};
void* signal_callbacks[2]{};
void* signal_returns[2]{};
void* input_returns[2]{};
void* hide_cursor_return = nullptr;
bool installed = false;
bool attempted = false;

struct SignalCapture {
  void* owner;
  void* callback;
};

thread_local void* constructing_interactor = nullptr;
thread_local void* releasing_player = nullptr;
thread_local void* expected_remote_input = nullptr;
thread_local void* releasing_input = nullptr;
thread_local void* deleting_remote_interactor = nullptr;
std::atomic<std::uint64_t> omitted_callbacks{0};
std::atomic<std::uint64_t> released_interactors{0};
std::atomic<std::uint64_t> released_barriers{0};
std::atomic<std::uint64_t> preserved_cursors{0};
std::atomic<std::uint64_t> preserved_torch_actions{0};
std::atomic<std::uint64_t> refused_cleanup{0};
std::atomic<std::uint64_t> next_report{0};

template <class Value>
bool read(const void* object, std::size_t offset, Value& value) {
  if (!object || offset > UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(object)) {
    return false;
  }
  __try {
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool zero_word(const void* object, std::size_t offset) {
  std::uintptr_t value = 1;
  return read(object, offset, value) && value == 0;
}

bool same_vtable(const void* object, const void* expected) {
  void* actual = nullptr;
  return expected && read(object, 0, actual) && actual == expected;
}

void* service_member(std::size_t offset) {
  // The native getter is outside diagnostic SEH; native faults propagate normally.
  void* registry = nullptr;
  void* member = nullptr;
  return read(services(), 296, registry) && read(registry, offset, member) ? member : nullptr;
}

bool cry_string_equals(const void* reference, const char* expected, std::size_t size) {
  const char* text = nullptr;
  if (!read(reference, 0, text) || !text) {
    return false;
  }
  // The two CryString temporaries belong to Input::~Input and are destroyed by that caller.
  // Read only the bounded known string including its terminator; never consume either one.
  for (std::size_t index = 0; index < size; ++index) {
    char actual = 0;
    if (!read(text, index, actual) || actual != expected[index]) {
      return false;
    }
  }
  return true;
}

bool bounded_signal(const void* signal) {
  void* data = nullptr;
  std::int32_t count = -1;
  if (!read(signal, 0, data) || reinterpret_cast<std::uintptr_t>(data) < 4) {
    return false;
  }
  const auto header = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(data) - 4);
  // Native removal1804CD6C8 narrows the found/absent index to signed16. An absent
  // callback is a no-op only while count fits that index; do not enter the overflow case.
  return read(header, 0, count) && count >= 0 && count <= 32767;
}

bool bounded_interactor_signals() {
  auto* keybindings = static_cast<std::uint8_t*>(service_member(88));
  auto* initialized_signal = service_member(80);
  return keybindings && initialized_signal && bounded_signal(keybindings + 16) &&
         bounded_signal(keybindings + 32) && bounded_signal(initialized_signal);
}

void report() {
  auto due = next_report.load(std::memory_order_relaxed);
  const auto now = GetTickCount64();
  if (now < due || !next_report.compare_exchange_strong(due, now + 5000)) {
    return;
  }
  log("REMOTE interactor lifetime callbacks_omitted=%llu interactors_released=%llu "
      "barriers_released=%llu cursor_resets_omitted=%llu torch_clears_omitted=%llu "
      "cleanup_refused=%llu",
      omitted_callbacks.load(), released_interactors.load(), released_barriers.load(),
      preserved_cursors.load(), preserved_torch_actions.load(), refused_cleanup.load());
}

bool pristine_interactor(const void* object) {
  if (!same_vtable(object, interactor_vtable) || !zero_word(object, 8) ||
      !zero_word(object, 1040) || !zero_word(object, 1048) || !zero_word(object, 1056) ||
      !zero_word(object, 288) || !zero_word(object, 1328) || !zero_word(object, 1344)) {
    return false;
  }
  void* entries = nullptr;
  std::uint64_t count = 0;
  if (!read(object, 216, entries) || !read(object, 224, count) || !entries || count != 3) {
    return false;
  }
  // Constructor-owned sorted entries are {kind, interaction*}; Init binds the children
  // to the actor and script handles. Reject any initialized/replaced child before deletion.
  for (unsigned index = 0; index < 3; ++index) {
    std::uint32_t kind = 0;
    void* child = nullptr;
    if (!read(entries, index * 16, kind) || kind != (1u << index) ||
        !read(entries, index * 16 + 8, child)) {
      return false;
    }
    if (child && (!same_vtable(child, interaction_vtables[index]) || !zero_word(child, 8) ||
                  !zero_word(child, 16) || !zero_word(child, 24))) {
      return false;
    }
  }
  return bounded_interactor_signals();
}

bool pristine_barrier(const void* object) {
  std::uint8_t active = 1;
  std::uint32_t registration = 1;
  return same_vtable(object, barrier_vtable) && zero_word(object, 8) && read(object, 16, active) &&
         active == 0 && read(object, 32, registration) && registration == 0;
}

void* __fastcall construct_interactor(void* self) {
  const auto previous = constructing_interactor;
  constructing_interactor = kcds::runtime::replicas::constructing_remote_player ? self : nullptr;
  void* result = nullptr;
  __try {
    result = original_interactor_ctor(self);
  } __finally {
    constructing_interactor = previous;
  }
  return result;
}

std::int64_t __fastcall register_signal(void* signal, const void* capture_pointer) {
  const auto caller = _ReturnAddress();
  SignalCapture capture{};
  if (constructing_interactor && read(capture_pointer, 0, capture) &&
      capture.owner == constructing_interactor) {
    for (unsigned index = 0; index < 2; ++index) {
      if (caller == signal_returns[index] && capture.callback == signal_callbacks[index]) {
        const auto keybindings = static_cast<std::uint8_t*>(service_member(88));
        if (keybindings && signal == keybindings + 16 + index * 16) {
          ++omitted_callbacks;
          // This constructor ignores the insertion result. The capture is two borrowed
          // pointers with no destructor/ownership to consume.
          return 0;
        }
      }
    }
  }
  return original_register_signal(signal, capture_pointer);
}

std::int64_t __fastcall hide_cursors(void* self) {
  if (deleting_remote_interactor && self == deleting_remote_interactor &&
      _ReturnAddress() == hide_cursor_return) {
    ++preserved_cursors;
    // Only the interactor destructor's unconditional global HUD reset is local-only.
    // Its return value is ignored. All other destructor operations still execute.
    return 0;
  }
  return original_hide_cursors(self);
}

std::int64_t __fastcall destroy_input(void* self) {
  void* owner = nullptr;
  const auto previous = releasing_input;
  const bool remote =
      read(self, 16, owner) && kcds::runtime::replicas::remote_input_without_torch_ownership(
                                   self, owner, kcds::runtime::replicas::constructing_remote_player,
                                   releasing_player, expected_remote_input);
  releasing_input = remote ? self : nullptr;
  std::int64_t result = 0;
  __try {
    result = original_input_dtor(self);
  } __finally {
    releasing_input = previous;
  }
  return result;
}

std::int64_t __fastcall clear_action(void* registry, const void* map, const void* action,
                                     unsigned char all_maps) {
  const auto caller = _ReturnAddress();
  if (releasing_input && all_maps == 1 &&
      cry_string_equals(map, "draw_holster_torch", sizeof("draw_holster_torch"))) {
    const bool keyboard = caller == input_returns[0] &&
                          cry_string_equals(action, "toggle_torch", sizeof("toggle_torch"));
    const bool controller = caller == input_returns[1] &&
                            cry_string_equals(action, "xi_toggle_torch", sizeof("xi_toggle_torch"));
    if ((keyboard || controller) && registry == service_member(96)) {
      ++preserved_torch_actions;
      return 0;
    }
  }
  return original_clear_action(registry, map, action, all_maps);
}

void cleanup_interactor(void* object) {
  const auto previous = deleting_remote_interactor;
  deleting_remote_interactor = object;
  __try {
    delete_interactor(object, 1);
  } __finally {
    deleting_remote_interactor = previous;
  }
  ++released_interactors;
}

std::int64_t __fastcall release_player(void* player) {
  void* interactor = nullptr;
  void* barrier = nullptr;
  void* input = nullptr;
  read(player, 2952, input);
  const bool interactor_ok =
      read(player, 2824, interactor) && (!interactor || pristine_interactor(interactor));
  const bool barrier_ok = read(player, 2832, barrier) && (!barrier || pristine_barrier(barrier));
  const auto previous = releasing_player;
  const auto previous_input = expected_remote_input;
  releasing_player = player;
  expected_remote_input = input;
  std::int64_t result = 0;
  __try {
    // C_Actor::Release -> deleting C_Player destructor. That destructor never owns these
    // two pointers. Movement/update/destruction may still read +2824 until this returns.
    result = actor_release(player);
  } __finally {
    releasing_player = previous;
    expected_remote_input = previous_input;
  }
  // Never read player after native release. Native exceptions skip cleanup and propagate;
  // a partially released actor must not acquire dangling interactor pointers.
  if (interactor && interactor_ok && pristine_interactor(interactor)) {
    cleanup_interactor(interactor);
  } else if (!interactor_ok || interactor) {
    ++refused_cleanup;
  }
  if (barrier && barrier_ok && pristine_barrier(barrier)) {
    // Shutdown is a no-op for owner=null/active=0/id=0; the deleting destructor frees 40 B.
    delete_barrier(barrier, 1);
    ++released_barriers;
  } else if (!barrier_ok || barrier) {
    ++refused_cleanup;
  }
  report();
  return result;
}

bool in_image(const void* pointer, std::size_t bytes) {
  const auto base = reinterpret_cast<std::uintptr_t>(image);
  const auto value = reinterpret_cast<std::uintptr_t>(pointer);
  return value >= base && value - base <= image_size && bytes <= image_size - (value - base);
}

void* relative(const void* base, std::size_t offset, std::size_t operand, std::size_t length,
               std::uint8_t first, std::uint8_t second = 0) {
  const auto* instruction = static_cast<const std::uint8_t*>(base) + offset;
  if (!in_image(instruction, length) || instruction[0] != first ||
      (second && instruction[1] != second)) {
    return nullptr;
  }
  std::int32_t displacement = 0;
  std::memcpy(&displacement, instruction + operand, sizeof(displacement));
  const auto target = reinterpret_cast<std::intptr_t>(instruction) + length + displacement;
  auto* result = reinterpret_cast<void*>(target);
  return in_image(result, 8) ? result : nullptr;
}

struct Hook {
  void* target;
  void* replacement;
  void** original;
};

bool install_hooks(Hook* hooks, std::size_t count) {
  std::size_t created = 0;
  std::size_t enabled = 0;
  for (; created < count; ++created) {
    if (MH_CreateHook(hooks[created].target, hooks[created].replacement, hooks[created].original) !=
        MH_OK) {
      break;
    }
  }
  if (created == count) {
    for (; enabled < count; ++enabled) {
      if (MH_EnableHook(hooks[enabled].target) != MH_OK) {
        break;
      }
    }
  }
  if (created == count && enabled == count) {
    return true;
  }
  while (enabled) {
    MH_DisableHook(hooks[--enabled].target);
  }
  while (created) {
    MH_RemoveHook(hooks[--created].target);
  }
  return false;
}

}  // namespace

bool install_remote_interactor_lifetime() {
  if (attempted) {
    return installed;
  }
  attempted = true;
  if (!image || !image_size) {
    return false;
  }
  auto* ctor = resolve_unique("RemoteInteractorCtor", kInteractorCtor);
  // The signal template has several byte-identical instantiations. Resolve the full
  // routine through this constructor's unique direct call, then verify both call sites.
  auto* registration =
      resolve_unique("RemoteInteractorSignalRegister", kInteractorCtor, 0x514, 0x518);
  delete_interactor = reinterpret_cast<DeletingDestructor>(
      resolve_unique("RemoteInteractorDelete", kInteractorDelete));
  auto* barrier_ctor = resolve_unique("RemoteBarrierCtor", kBarrierCtor);
  delete_barrier =
      reinterpret_cast<DeletingDestructor>(resolve_unique("RemoteBarrierDelete", kBarrierDelete));
  auto* hud = resolve_unique("RemoteInteractorHideCursors", kHideCursors);
  auto* input_dtor = resolve_unique("RemoteInputDestructor", kInputDtor);
  auto* action_clear = resolve_unique("RemoteInputClearAction", kClearAction);
  actor_release =
      reinterpret_cast<Unary>(resolve_unique("RemoteInteractorActorRelease", kActorRelease));
  if (!ctor || !registration || !delete_interactor || !barrier_ctor || !delete_barrier || !hud ||
      !input_dtor || !action_clear || !actor_release) {
    log("REMOTE interactor lifetime signatures unavailable; adapter not started");
    return false;
  }
  interactor_vtable = relative(ctor, 0x31, 3, 7, 0x48, 0x8D);
  barrier_vtable = relative(barrier_ctor, 6, 3, 7, 0x48, 0x8D);
  constexpr std::size_t child_vtable_offsets[] = {0x395, 0x40A, 0x479};
  for (unsigned index = 0; index < 3; ++index) {
    interaction_vtables[index] = relative(ctor, child_vtable_offsets[index], 3, 7, 0x48, 0x8D);
  }
  services = reinterpret_cast<Services>(relative(ctor, 0x4E4, 1, 5, 0xE8));
  signal_callbacks[0] = relative(ctor, 0x4F0, 3, 7, 0x48, 0x8D);
  signal_callbacks[1] = relative(ctor, 0x518, 3, 7, 0x48, 0x8D);
  signal_returns[0] = static_cast<std::uint8_t*>(ctor) + 0x518;
  signal_returns[1] = static_cast<std::uint8_t*>(ctor) + 0x53C;
  auto* interactor_dtor = relative(reinterpret_cast<void*>(delete_interactor), 15, 1, 5, 0xE8);
  hide_cursor_return =
      interactor_dtor ? static_cast<std::uint8_t*>(interactor_dtor) + 0x4C : nullptr;
  input_returns[0] = static_cast<std::uint8_t*>(input_dtor) + 0xB5;
  input_returns[1] = static_cast<std::uint8_t*>(input_dtor) + 0x104;
  void* interactor_delete_slot = nullptr;
  void* barrier_delete_slot = nullptr;
  std::uint16_t first_input_call = 0;
  std::uint16_t second_input_call = 0;
  const bool layout_matches =
      interactor_vtable && barrier_vtable && interaction_vtables[0] && interaction_vtables[1] &&
      interaction_vtables[2] && services && signal_callbacks[0] && signal_callbacks[1] &&
      interactor_dtor && relative(ctor, 0x513, 1, 5, 0xE8) == registration &&
      relative(ctor, 0x537, 1, 5, 0xE8) == registration &&
      relative(interactor_dtor, 0x47, 1, 5, 0xE8) == hud &&
      read(interactor_vtable, 0, interactor_delete_slot) &&
      interactor_delete_slot == reinterpret_cast<void*>(delete_interactor) &&
      read(barrier_vtable, 0, barrier_delete_slot) &&
      barrier_delete_slot == reinterpret_cast<void*>(delete_barrier) &&
      read(input_dtor, 0xB3, first_input_call) && first_input_call == 0xD3FF &&
      read(input_dtor, 0x102, second_input_call) && second_input_call == 0xD3FF;
  if (!layout_matches) {
    log("REMOTE interactor lifetime native layout changed; adapter not started");
    return false;
  }
  Hook hooks[] = {
      {ctor, reinterpret_cast<void*>(construct_interactor),
       reinterpret_cast<void**>(&original_interactor_ctor)},
      {registration, reinterpret_cast<void*>(register_signal),
       reinterpret_cast<void**>(&original_register_signal)},
      {hud, reinterpret_cast<void*>(hide_cursors),
       reinterpret_cast<void**>(&original_hide_cursors)},
      {input_dtor, reinterpret_cast<void*>(destroy_input),
       reinterpret_cast<void**>(&original_input_dtor)},
      {action_clear, reinterpret_cast<void*>(clear_action),
       reinterpret_cast<void**>(&original_clear_action)},
  };
  installed = install_hooks(hooks, sizeof(hooks) / sizeof(hooks[0]));
  log(installed ? "REMOTE interactor lifetime installed; native ownership and local UI preserved"
                : "REMOTE interactor lifetime hooks unavailable; adapter not started");
  return installed;
}

void* remote_interactor_release_target() {
  return installed ? reinterpret_cast<void*>(release_player) : nullptr;
}

}  // namespace kcds::game_client
