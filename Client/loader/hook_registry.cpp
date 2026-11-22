#include "hook_registry.hpp"

namespace kcds::loader {

void HookRegistry::run_game_code(Hook& hook) {
  hook.slot->store(hook.trampoline, std::memory_order_release);
  hook.enabled = false;
}

int HookRegistry::create_hook(void* target, void* detour, void** original) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = hooks_.find(target);
  if (found != hooks_.end()) {
    Hook& hook = found->second;
    if (hook.owned) {
      return kHookAlreadyCreated;
    }
    // Hooked by a payload before: the same thunk and the same original.
    hook.owned = true;
    hook.detour = detour;
    hook.queued = Queued::none;
    run_game_code(hook);
    if (original) {
      *original = hook.trampoline;
    }
    return kHookOk;
  }
  Hook hook;
  hook.thunk = backend_.make_thunk(&hook.slot);
  if (!hook.thunk || !hook.slot) {
    return kHookMemoryAlloc;
  }
  const int created = backend_.create(target, hook.thunk, &hook.trampoline);
  if (created != kHookOk) {
    return created;
  }
  hook.owned = true;
  hook.detour = detour;
  run_game_code(hook);
  if (original) {
    *original = hook.trampoline;
  }
  hooks_.emplace(target, hook);
  return kHookOk;
}

int HookRegistry::enable_one(Hook& hook, void* target) {
  if (hook.enabled) {
    return kHookEnabled;
  }
  if (!hook.patched) {
    const int enabled = backend_.enable(target);
    if (enabled != kHookOk) {
      return enabled;
    }
    hook.patched = true;
  }
  hook.slot->store(hook.detour, std::memory_order_release);
  hook.enabled = true;
  return kHookOk;
}

int HookRegistry::enable_hook(void* target) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target) {
    for (auto& [function, hook] : hooks_) {
      if (hook.owned && !hook.enabled) {
        const int enabled = enable_one(hook, function);
        if (enabled != kHookOk) {
          return enabled;
        }
      }
    }
    return kHookOk;
  }
  auto found = hooks_.find(target);
  if (found == hooks_.end() || !found->second.owned) {
    return kHookNotCreated;
  }
  return enable_one(found->second, target);
}

int HookRegistry::disable_hook(void* target) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target) {
    for (auto& [function, hook] : hooks_) {
      (void)function;
      if (hook.owned && hook.enabled) {
        run_game_code(hook);
      }
    }
    return kHookOk;
  }
  auto found = hooks_.find(target);
  if (found == hooks_.end() || !found->second.owned) {
    return kHookNotCreated;
  }
  if (!found->second.enabled) {
    return kHookDisabled;
  }
  run_game_code(found->second);
  return kHookOk;
}

int HookRegistry::remove_hook(void* target) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = hooks_.find(target);
  if (found == hooks_.end() || !found->second.owned) {
    return kHookNotCreated;
  }
  Hook& hook = found->second;
  // The game stays patched: the thunk runs the game's own code until a payload hooks it again.
  run_game_code(hook);
  hook.owned = false;
  hook.detour = nullptr;
  hook.queued = Queued::none;
  return kHookOk;
}

int HookRegistry::queue_enable_hook(void* target) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target) {
    for (auto& [function, hook] : hooks_) {
      (void)function;
      if (hook.owned) {
        hook.queued = Queued::enable;
      }
    }
    return kHookOk;
  }
  auto found = hooks_.find(target);
  if (found == hooks_.end() || !found->second.owned) {
    return kHookNotCreated;
  }
  found->second.queued = Queued::enable;
  return kHookOk;
}

int HookRegistry::queue_disable_hook(void* target) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target) {
    for (auto& [function, hook] : hooks_) {
      (void)function;
      if (hook.owned) {
        hook.queued = Queued::disable;
      }
    }
    return kHookOk;
  }
  auto found = hooks_.find(target);
  if (found == hooks_.end() || !found->second.owned) {
    return kHookNotCreated;
  }
  found->second.queued = Queued::disable;
  return kHookOk;
}

int HookRegistry::apply_queued() {
  std::lock_guard<std::mutex> lock(mutex_);
  // The game is patched for every queued hook at once (MinHook suspends its threads one time).
  bool patching = false;
  for (auto& [function, hook] : hooks_) {
    if (hook.queued == Queued::enable && !hook.patched) {
      const int queued = backend_.queue_enable(function);
      if (queued != kHookOk) {
        return queued;
      }
      patching = true;
    }
  }
  if (patching) {
    const int applied = backend_.apply_queued();
    if (applied != kHookOk) {
      return applied;
    }
  }
  for (auto& [function, hook] : hooks_) {
    (void)function;
    if (hook.queued == Queued::enable) {
      hook.patched = true;
      hook.slot->store(hook.detour, std::memory_order_release);
      hook.enabled = true;
    } else if (hook.queued == Queued::disable) {
      run_game_code(hook);
    }
    hook.queued = Queued::none;
  }
  return kHookOk;
}

void HookRegistry::release_payload() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [function, hook] : hooks_) {
    (void)function;
    run_game_code(hook);
    hook.owned = false;
    hook.detour = nullptr;
    hook.queued = Queued::none;
  }
}

void* HookRegistry::jumps_to(void* target) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = hooks_.find(target);
  return found == hooks_.end() ? nullptr : found->second.slot->load(std::memory_order_acquire);
}

}  // namespace kcds::loader
