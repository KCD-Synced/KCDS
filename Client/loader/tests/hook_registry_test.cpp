#include "../hook_registry.hpp"

#include <atomic>
#include <memory>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcds::loader::HookBackend;
using kcds::loader::HookRegistry;

namespace {

// Thunks are numbered slots; the game is "patched" when its function is in `patched`.
class FakeBackend : public HookBackend {
 public:
  void* make_thunk(std::atomic<void*>** slot) override {
    slots.push_back(std::make_unique<std::atomic<void*>>(nullptr));
    *slot = slots.back().get();
    return reinterpret_cast<void*>(0x1000 + slots.size());
  }
  int create(void* target, void* thunk, void** trampoline) override {
    (void)thunk;
    ++creates;
    *trampoline = static_cast<char*>(target) + 1;  // "the game's own code"
    return 0;
  }
  int enable(void* target) override {
    patched.push_back(target);
    return 0;
  }
  int queue_enable(void* target) override {
    queued.push_back(target);
    return 0;
  }
  int apply_queued() override {
    patched.insert(patched.end(), queued.begin(), queued.end());
    queued.clear();
    ++applies;
    return 0;
  }

  std::vector<std::unique_ptr<std::atomic<void*>>> slots;
  std::vector<void*> patched;
  std::vector<void*> queued;
  int creates = 0;
  int applies = 0;
};

char game_a[8];
char game_b[8];
char detour_1[8];
char detour_2[8];

}  // namespace

KCD2S_TEST(hook_registry, a_hook_jumps_to_the_detour_only_while_enabled) {
  FakeBackend backend;
  HookRegistry hooks(backend);
  void* original = nullptr;
  KCD2S_CHECK_EQ(hooks.create_hook(game_a, detour_1, &original), 0);
  KCD2S_CHECK(original == game_a + 1);
  // Created, not enabled: the game's own code, and the game is not patched yet.
  KCD2S_CHECK(hooks.jumps_to(game_a) == game_a + 1);
  KCD2S_CHECK(backend.patched.empty());
  KCD2S_CHECK_EQ(hooks.enable_hook(game_a), 0);
  KCD2S_CHECK(hooks.jumps_to(game_a) == detour_1);
  KCD2S_CHECK_EQ(backend.patched.size(), 1u);
  KCD2S_CHECK_EQ(hooks.enable_hook(game_a), 5);  // MH_ERROR_ENABLED
  KCD2S_CHECK_EQ(hooks.disable_hook(game_a), 0);
  KCD2S_CHECK(hooks.jumps_to(game_a) == game_a + 1);
  KCD2S_CHECK_EQ(hooks.disable_hook(game_a), 6);                      // MH_ERROR_DISABLED
  KCD2S_CHECK_EQ(hooks.create_hook(game_a, detour_2, &original), 3);  // already created
  KCD2S_CHECK_EQ(hooks.enable_hook(game_b), 4);                       // not created
}

KCD2S_TEST(hook_registry, the_next_payload_gets_the_same_thunk_and_original) {
  FakeBackend backend;
  HookRegistry hooks(backend);
  void* original = nullptr;
  hooks.create_hook(game_a, detour_1, &original);
  hooks.enable_hook(game_a);
  // The payload goes: the game's own code runs, the game stays patched.
  hooks.release_payload();
  KCD2S_CHECK(hooks.jumps_to(game_a) == game_a + 1);
  KCD2S_CHECK_EQ(hooks.enable_hook(game_a), 4);
  // The next payload hooks the same function: no new MinHook hook, no new patch.
  void* again = nullptr;
  KCD2S_CHECK_EQ(hooks.create_hook(game_a, detour_2, &again), 0);
  KCD2S_CHECK(again == original);
  KCD2S_CHECK_EQ(hooks.enable_hook(game_a), 0);
  KCD2S_CHECK(hooks.jumps_to(game_a) == detour_2);
  KCD2S_CHECK_EQ(backend.creates, 1);
  KCD2S_CHECK_EQ(backend.patched.size(), 1u);
}

KCD2S_TEST(hook_registry, a_paused_payload_runs_the_game_code_until_resumed) {
  FakeBackend backend;
  HookRegistry hooks(backend);
  void* original = nullptr;
  hooks.create_hook(game_a, detour_1, &original);
  hooks.create_hook(game_b, detour_2, &original);
  hooks.enable_hook(game_a);
  hooks.pause_payload();
  KCD2S_CHECK(hooks.jumps_to(game_a) == game_a + 1);
  KCD2S_CHECK(hooks.jumps_to(game_b) == game_b + 1);
  hooks.resume_payload();
  // Only the enabled one gets its detour back.
  KCD2S_CHECK(hooks.jumps_to(game_a) == detour_1);
  KCD2S_CHECK(hooks.jumps_to(game_b) == game_b + 1);
}

KCD2S_TEST(hook_registry, removed_hooks_run_the_game_code_and_can_be_made_again) {
  FakeBackend backend;
  HookRegistry hooks(backend);
  void* original = nullptr;
  hooks.create_hook(game_a, detour_1, &original);
  hooks.enable_hook(game_a);
  KCD2S_CHECK_EQ(hooks.remove_hook(game_a), 0);
  KCD2S_CHECK(hooks.jumps_to(game_a) == game_a + 1);
  KCD2S_CHECK_EQ(hooks.remove_hook(game_a), 4);
  KCD2S_CHECK_EQ(hooks.create_hook(game_a, detour_2, &original), 0);
  KCD2S_CHECK_EQ(backend.creates, 1);
}

KCD2S_TEST(hook_registry, all_hooks_and_queued_enables) {
  FakeBackend backend;
  HookRegistry hooks(backend);
  void* original = nullptr;
  hooks.create_hook(game_a, detour_1, &original);
  hooks.create_hook(game_b, detour_2, &original);
  // Queued: the game is patched for both at once.
  KCD2S_CHECK_EQ(hooks.queue_enable_hook(nullptr), 0);
  KCD2S_CHECK(hooks.jumps_to(game_a) == game_a + 1);
  KCD2S_CHECK_EQ(hooks.apply_queued(), 0);
  KCD2S_CHECK_EQ(backend.applies, 1);
  KCD2S_CHECK(hooks.jumps_to(game_a) == detour_1);
  KCD2S_CHECK(hooks.jumps_to(game_b) == detour_2);
  // All off, then all on again (the game is patched already: no new patch).
  KCD2S_CHECK_EQ(hooks.disable_hook(nullptr), 0);
  KCD2S_CHECK(hooks.jumps_to(game_b) == game_b + 1);
  KCD2S_CHECK_EQ(hooks.enable_hook(nullptr), 0);
  KCD2S_CHECK(hooks.jumps_to(game_b) == detour_2);
  KCD2S_CHECK_EQ(backend.patched.size(), 2u);
}
