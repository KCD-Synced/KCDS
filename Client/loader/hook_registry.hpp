// The loader's hooks of the game, kept across payloads.
//
// Each hooked game function jumps to a thunk of the loader, and the thunk jumps through a slot: the
// payload's detour while the payload has the hook enabled, otherwise the function that runs the
// game's own code (MinHook's trampoline). A payload that goes (or removes its hook) only resets
// slots, so the game is never patched again and a later payload hooking the same function gets the
// same thunk and the same original. The results are MinHook's MH_STATUS values.
#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace kcds::loader {

// MinHook's MH_STATUS values that the registry returns itself.
enum HookStatus : int {
  kHookOk = 0,
  kHookAlreadyCreated = 3,
  kHookNotCreated = 4,
  kHookEnabled = 5,
  kHookDisabled = 6,
  kHookMemoryAlloc = 9,
};

// What patches the game: MinHook in the loader, a fake in the tests.
class HookBackend {
 public:
  virtual ~HookBackend() = default;
  // A new thunk that jumps through *slot; null when none could be made.
  virtual void* make_thunk(std::atomic<void*>** slot) = 0;
  // MinHook's create (the game function goes to the thunk) and its enable, queued or not.
  virtual int create(void* target, void* thunk, void** trampoline) = 0;
  virtual int enable(void* target) = 0;
  virtual int queue_enable(void* target) = 0;
  virtual int apply_queued() = 0;
};

class HookRegistry {
 public:
  explicit HookRegistry(HookBackend& backend) : backend_(backend) {
  }

  // MinHook's calls for the payload. A null target in enable/disable/queue: every hook the
  // payload has.
  int create_hook(void* target, void* detour, void** original);
  int enable_hook(void* target);
  int disable_hook(void* target);
  int remove_hook(void* target);
  int queue_enable_hook(void* target);
  int queue_disable_hook(void* target);
  int apply_queued();

  // The payload's hooks run the game's own code for a while (its unload steps run then); resume
  // gives them back their detours (the swap was called off).
  void pause_payload();
  void resume_payload();

  // The payload goes: every hook runs the game's own code again, and none is the payload's.
  void release_payload();

  // Where the hook of target jumps now (its slot), null when it has none. For the tests.
  void* jumps_to(void* target) const;

 private:
  enum class Queued { none, enable, disable };
  struct Hook {
    void* thunk = nullptr;
    std::atomic<void*>* slot = nullptr;
    void* trampoline = nullptr;
    void* detour = nullptr;
    bool patched = false;  // the game function jumps to the thunk
    bool owned = false;    // the payload created it (and has not removed it)
    bool enabled = false;  // the slot holds the payload's detour
    Queued queued = Queued::none;
  };

  int enable_one(Hook& hook, void* target);
  static void run_game_code(Hook& hook);

  HookBackend& backend_;
  mutable std::mutex mutex_;
  std::unordered_map<void*, Hook> hooks_;
};

}  // namespace kcds::loader
