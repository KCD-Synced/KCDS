#include "human_overlay.hpp"
#include "kcd2s/testkit.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace kcds::runtime::human_overlay;

namespace {

struct Entry {
  std::uint64_t hash = 0;
  std::int32_t global_index = 0;
  std::uint16_t type = 0;
  std::uint16_t extra = 0;
};
static_assert(sizeof(Entry) == 16);

struct Entries {
  std::uint32_t unused = 0;
  std::uint32_t count = 3;
  std::array<Entry, 3> values{{{0xA02512BEC7816746ULL, 3, 0, 0},
                               {0x730B10DC850A40C5ULL, 4, 0, 0},
                               {0xFEDCBA9876543210ULL, 5, 0, 0}}};
};
static_assert(offsetof(Entries, values) - offsetof(Entries, count) == 4);

struct GlobalCafs {
  std::uint32_t unused = 0;
  std::uint32_t count = 10;
  std::array<std::byte, 10 * 168> values{};
};
static_assert(offsetof(GlobalCafs, values) - offsetof(GlobalCafs, count) == 4);

struct Manager {
  std::array<std::byte, 640> prefix{};
  void* global_cafs = nullptr;
  std::array<std::byte, 525728 - 640 - sizeof(void*)> before_facial{};
  std::uint32_t facial_begin = 3;
  std::uint32_t facial_end = 6;
};
static_assert(offsetof(Manager, global_cafs) == 640);
static_assert(offsetof(Manager, facial_begin) == 525728);
static_assert(offsetof(Manager, facial_end) == 525732);

struct Controllers {
  std::uint32_t unused = 0;
  std::uint32_t count = 2;
  std::array<std::uintptr_t, 2> values{1, 2};
};
static_assert(offsetof(Controllers, values) - offsetof(Controllers, count) == 4);

struct FakeSet {
  void** vtable = nullptr;
  Entry* entries = nullptr;
  bool null_entry = false;
  int entry_delta = 0;
  std::optional<std::int64_t> resolver_override;
  unsigned entry_calls = 0;
  unsigned resolver_calls = 0;
};
static_assert(offsetof(FakeSet, entries) == 8);

struct Model {
  std::array<std::byte, 536> prefix{};
  FakeSet* set = nullptr;
};
static_assert(offsetof(Model, set) == 536);

struct Character {
  void** vtable = nullptr;
  std::array<std::byte, 272 - sizeof(void*)> prefix{};
  alignas(void*) std::array<std::byte, 1264> skeleton{};
  std::array<std::byte, 2672 - 272 - 1264> before_model{};
  Model* model = nullptr;
  unsigned set_calls = 0;
  unsigned skeleton_calls = 0;
};
static_assert(offsetof(Character, skeleton) == 272);
static_assert(offsetof(Character, model) == 2672);

void* __fastcall get_set(void* pointer) {
  auto& character = *static_cast<Character*>(pointer);
  ++character.set_calls;
  return character.model->set;
}

void* __fastcall get_skeleton(void* pointer) {
  auto& character = *static_cast<Character*>(pointer);
  ++character.skeleton_calls;
  return character.skeleton.data();
}

void* __fastcall get_entry(void* pointer, int id) {
  auto& set = *static_cast<FakeSet*>(pointer);
  ++set.entry_calls;
  if (set.null_entry) {
    return nullptr;
  }
  return set.entries + id + set.entry_delta;
}

std::int64_t __fastcall resolve_hash(void* pointer, std::uint64_t hash) {
  auto& set = *static_cast<FakeSet*>(pointer);
  ++set.resolver_calls;
  if (set.resolver_override) {
    return *set.resolver_override;
  }
  std::uint32_t count;
  std::memcpy(&count, reinterpret_cast<const std::byte*>(set.entries) - 4, sizeof(count));
  for (unsigned i = 0; i < std::min(count, 3u); ++i) {
    if (set.entries[i].hash == hash) {
      return i;
    }
  }
  return -1;
}

struct Fixture {
  Character character;
  std::array<std::byte, 1264>& skeleton = character.skeleton;
  std::array<void*, 17> character_table{};
  std::array<void*, 9> set_table{};
  Entries entries;
  FakeSet set;
  Model model;
  GlobalCafs global_cafs;
  Controllers controllers;
  std::unique_ptr<Manager> manager = std::make_unique<Manager>();
  void* manager_pointer = manager.get();
  Natives natives{reinterpret_cast<void*>(get_set), reinterpret_cast<void*>(get_entry),
                  reinterpret_cast<void*>(resolve_hash), reinterpret_cast<void*>(get_skeleton),
                  &manager_pointer};

  Fixture() {
    character_table[5] = natives.getSkeleton;
    character_table[15] = natives.getAnimationSet;
    character_table[16] = natives.getAnimationSet;
    set_table[8] = natives.resolveHash;
    set.vtable = set_table.data();
    set.entries = entries.values.data();
    character.vtable = character_table.data();
    model.set = &set;
    character.model = &model;
    manager->global_cafs = global_cafs.values.data();
    set_character(&character);
    install(natives);
  }

  ~Fixture() {
    install({});
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  void set_character(void* pointer) {
    std::memcpy(skeleton.data() + 1256, &pointer, sizeof(pointer));
  }

  void set_residency(int global_index = 3, std::uint32_t flags = 0x800, std::uint16_t resident = 2,
                     std::uint16_t expected = 2) {
    auto* header = global_cafs.values.data() + 168 * global_index;
    const void* pointer = controllers.values.data();
    std::memcpy(header + 24, &flags, sizeof(flags));
    std::memcpy(header + 56, &pointer, sizeof(pointer));
    std::memcpy(header + 156, &resident, sizeof(resident));
    std::memcpy(header + 158, &expected, sizeof(expected));
  }
};

}  // namespace

KCD2S_TEST(runtime_human_overlay, current_caf_identity_round_trips_without_pointer_cache) {
  Fixture fixture;
  for (int id = 0; id < 3; ++id) {
    const auto hash = alias(fixture.skeleton.data(), id);
    KCD2S_CHECK(hash && *hash == fixture.entries.values[id].hash);
    const auto local = resolve(fixture.skeleton.data(), fixture.entries.values[id].hash);
    KCD2S_CHECK(local && *local == id);
  }
  KCD2S_CHECK_EQ(fixture.character.set_calls, 6u);
  KCD2S_CHECK_EQ(fixture.character.skeleton_calls, 6u);
  KCD2S_CHECK_EQ(fixture.set.entry_calls, 6u);
  KCD2S_CHECK_EQ(fixture.set.resolver_calls, 6u);
}

KCD2S_TEST(runtime_human_overlay, missing_native_disables_previous_installation) {
  Fixture fixture;
  const std::array<void * Natives::*, 4> members = {&Natives::getAnimationSet, &Natives::entry,
                                                    &Natives::resolveHash, &Natives::getSkeleton};
  for (auto member : members) {
    KCD2S_CHECK(install(fixture.natives));
    auto partial = fixture.natives;
    partial.*member = nullptr;
    KCD2S_CHECK(!install(partial));
    KCD2S_CHECK(!scope(fixture.skeleton.data()));
    KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
  }
  auto partial = fixture.natives;
  partial.characterManager = nullptr;
  KCD2S_CHECK(!install(partial));
  KCD2S_CHECK(!scope(fixture.skeleton.data()));
  KCD2S_CHECK_EQ(fixture.character.set_calls, 0u);
}

KCD2S_TEST(runtime_human_overlay, character_vtable_and_skeleton_round_trip_are_required) {
  for (const auto slot : {5u, 15u, 16u}) {
    Fixture fixture;
    fixture.character_table[slot] = fixture.natives.entry;
    KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
    KCD2S_CHECK_EQ(fixture.character.skeleton_calls, 0u);
    KCD2S_CHECK_EQ(fixture.character.set_calls, 0u);
  }
  Fixture fixture;
  KCD2S_CHECK(!alias(nullptr, 0));
  fixture.set_character(nullptr);
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.set_character(&fixture.character);
  fixture.character.vtable = nullptr;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.character.vtable = fixture.character_table.data();
  Character other;
  other.vtable = fixture.character_table.data();
  other.model = &fixture.model;
  fixture.set_character(&other);
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  KCD2S_CHECK_EQ(fixture.character.set_calls, 0u);
}

KCD2S_TEST(runtime_human_overlay, current_set_resolver_and_bounded_vector_are_required) {
  Fixture fixture;
  fixture.model.set = nullptr;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.model.set = &fixture.set;
  fixture.set.vtable = nullptr;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.set.vtable = fixture.set_table.data();
  fixture.set_table[8] = fixture.natives.entry;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.set_table[8] = fixture.natives.resolveHash;
  fixture.set.entries = nullptr;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.set.entries = fixture.entries.values.data();
  for (const auto count : {0u, 1'000'001u, std::numeric_limits<std::uint32_t>::max()}) {
    fixture.entries.count = count;
    KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
  }
  KCD2S_CHECK_EQ(fixture.set.entry_calls, 0u);
  KCD2S_CHECK_EQ(fixture.set.resolver_calls, 0u);
}

KCD2S_TEST(runtime_human_overlay, entry_must_be_exact_current_caf_with_nonzero_identity) {
  Fixture fixture;
  auto& entry = fixture.entries.values[0];
  fixture.set.null_entry = true;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  fixture.set.null_entry = false;
  fixture.set.entry_delta = 1;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), entry.hash));
  fixture.set.entry_delta = 0;
  const auto original_hash = entry.hash;
  entry.hash = 0;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), 0));
  entry.hash = original_hash;
  entry.global_index = -1;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), entry.hash));
  entry.global_index = 3;
  for (const auto type : {1u, 2u, 65535u}) {
    entry.type = static_cast<std::uint16_t>(type);
    KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), entry.hash));
  }
}

KCD2S_TEST(runtime_human_overlay, local_id_and_resolver_results_are_bounded_and_round_tripped) {
  Fixture fixture;
  for (const auto id : {-1, 3, std::numeric_limits<int>::max()}) {
    KCD2S_CHECK(!alias(fixture.skeleton.data(), id));
  }
  KCD2S_CHECK_EQ(fixture.set.entry_calls, 0u);
  for (const std::int64_t id :
       {-1LL, 3LL, 0x1'0000'0000LL, std::numeric_limits<std::int64_t>::max()}) {
    fixture.set.resolver_override = id;
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
  }
  fixture.set.resolver_override = 1;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
  fixture.set.resolver_override.reset();
  fixture.entries.values[1].hash = fixture.entries.values[0].hash;
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 1));
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), 0x123456789ABCDEF0ULL));
}

KCD2S_TEST(runtime_human_overlay, renumbered_current_model_uses_new_id_and_rejects_stale_alias) {
  Fixture fixture;
  const auto first_hash = fixture.entries.values[0].hash;
  const auto old_alias = alias(fixture.skeleton.data(), 1);
  KCD2S_CHECK(old_alias);

  Entries replacement;
  std::swap(replacement.values[0], replacement.values[2]);
  replacement.values[1].hash = 0x8000000000000001ULL;
  FakeSet new_set;
  new_set.vtable = fixture.set_table.data();
  new_set.entries = replacement.values.data();
  fixture.model.set = &new_set;
  const auto changed = resolve(fixture.skeleton.data(), first_hash);
  KCD2S_CHECK(changed && *changed == 2);
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), *old_alias));
  const auto now = alias(fixture.skeleton.data(), 1);
  KCD2S_CHECK(now && *now == replacement.values[1].hash);
  KCD2S_CHECK_EQ(fixture.set.entry_calls, 1u);

  // Pak registration can replace the current vector while the set object stays the same.
  fixture.set.entries = replacement.values.data();
  fixture.model.set = &fixture.set;
  const auto same_set = resolve(fixture.skeleton.data(), first_hash);
  KCD2S_CHECK(same_set && *same_set == 2);
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), *old_alias));
}

KCD2S_TEST(runtime_human_overlay, native_dynamic_clip_index_is_local_and_can_exceed_65535) {
  Fixture fixture;
  constexpr int local_id = 303479;
  constexpr std::uint32_t count = local_id + 1;
  // Native vector prefix: the count immediately precedes its aligned 16-byte entries.
  std::vector<std::uint64_t> storage(1 + 2 * count);
  std::memcpy(reinterpret_cast<std::byte*>(storage.data()) + 4, &count, sizeof(count));
  fixture.set.entries = reinterpret_cast<Entry*>(storage.data() + 1);
  Entry current{0xA02512BEC7816746ULL, 3, 0, 0};
  std::memcpy(reinterpret_cast<std::byte*>(fixture.set.entries) + 16 * local_id, &current,
              sizeof(current));
  fixture.set.resolver_override = local_id;
  const auto hash = alias(fixture.skeleton.data(), local_id);
  KCD2S_CHECK(hash && *hash == current.hash);
  const auto resolved = resolve(fixture.skeleton.data(), current.hash);
  KCD2S_CHECK(resolved && *resolved == local_id);
}

KCD2S_TEST(runtime_human_overlay, scope_changes_with_native_lifetime_even_when_alias_and_id_match) {
  Fixture fixture;
  const auto first = scope(fixture.skeleton.data());
  KCD2S_CHECK(first);
  KCD2S_CHECK_EQ(first->character, reinterpret_cast<std::uintptr_t>(&fixture.character));
  KCD2S_CHECK_EQ(first->model, reinterpret_cast<std::uintptr_t>(&fixture.model));
  KCD2S_CHECK_EQ(first->animation_set, reinterpret_cast<std::uintptr_t>(&fixture.set));
  KCD2S_CHECK_EQ(first->entries, reinterpret_cast<std::uintptr_t>(fixture.entries.values.data()));
  KCD2S_CHECK_EQ(first->count, 3u);
  KCD2S_CHECK(scope(fixture.skeleton.data()) == first);

  const auto hash = fixture.entries.values[0].hash;
  Model replacement_model;
  replacement_model.set = &fixture.set;
  fixture.character.model = &replacement_model;
  const auto model_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(model_changed && model_changed != first);
  KCD2S_CHECK_EQ(model_changed->animation_set, first->animation_set);
  KCD2S_CHECK(resolve(fixture.skeleton.data(), hash) == 0);

  FakeSet replacement_set;
  replacement_set.vtable = fixture.set_table.data();
  replacement_set.entries = fixture.entries.values.data();
  replacement_model.set = &replacement_set;
  const auto set_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(set_changed && set_changed != model_changed);
  KCD2S_CHECK_EQ(set_changed->model, model_changed->model);
  KCD2S_CHECK_EQ(set_changed->entries, first->entries);
  KCD2S_CHECK(alias(fixture.skeleton.data(), 0) == hash);

  Entries replacement_entries = fixture.entries;
  replacement_set.entries = replacement_entries.values.data();
  const auto vector_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(vector_changed && vector_changed != set_changed);
  KCD2S_CHECK_EQ(vector_changed->animation_set, set_changed->animation_set);
  KCD2S_CHECK_EQ(vector_changed->count, first->count);
  KCD2S_CHECK(resolve(fixture.skeleton.data(), hash) == 0);
  KCD2S_CHECK(alias(fixture.skeleton.data(), 0) == hash);

  replacement_entries.count = 2;
  const auto count_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(count_changed && count_changed != vector_changed);
  KCD2S_CHECK_EQ(count_changed->entries, vector_changed->entries);
  KCD2S_CHECK_EQ(count_changed->count, 2u);

  Character replacement_character;
  replacement_character.vtable = fixture.character_table.data();
  replacement_character.model = &replacement_model;
  void* character_pointer = &replacement_character;
  std::memcpy(replacement_character.skeleton.data() + 1256, &character_pointer,
              sizeof(character_pointer));
  const auto character_changed = scope(replacement_character.skeleton.data());
  KCD2S_CHECK(character_changed && character_changed != count_changed);
  KCD2S_CHECK_EQ(character_changed->model, count_changed->model);
  KCD2S_CHECK_EQ(character_changed->animation_set, count_changed->animation_set);
}

KCD2S_TEST(runtime_human_overlay, missing_model_is_rejected_before_native_getter_dereferences_it) {
  Fixture fixture;
  fixture.character.model = nullptr;
  KCD2S_CHECK(!scope(fixture.skeleton.data()));
  KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
  KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
  KCD2S_CHECK_EQ(fixture.character.set_calls, 0u);

  fixture.character.model = &fixture.model;
  fixture.model.set = nullptr;
  KCD2S_CHECK(!scope(fixture.skeleton.data()));
  KCD2S_CHECK_EQ(fixture.character.set_calls, 1u);
}

KCD2S_TEST(runtime_human_overlay, ordinary_body_caf_is_rejected_by_current_facial_interval) {
  Fixture fixture;
  auto& entry = fixture.entries.values[0];
  for (const auto global_index : {-1, 0, 2, 6, 9, 10}) {
    entry.global_index = global_index;
    KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), entry.hash));
  }
  entry.global_index = 3;
  KCD2S_CHECK(alias(fixture.skeleton.data(), 0) == entry.hash);
  KCD2S_CHECK(resolve(fixture.skeleton.data(), entry.hash) == 0);
  entry.global_index = 5;
  KCD2S_CHECK(alias(fixture.skeleton.data(), 0) == entry.hash);
  KCD2S_CHECK(resolve(fixture.skeleton.data(), entry.hash) == 0);
}

KCD2S_TEST(runtime_human_overlay, missing_or_invalid_current_manager_metadata_fails_closed) {
  Fixture fixture;
  fixture.manager_pointer = nullptr;
  KCD2S_CHECK(!scope(fixture.skeleton.data()));
  KCD2S_CHECK(!snapshot(fixture.skeleton.data()));
  fixture.manager_pointer = fixture.manager.get();
  fixture.manager->global_cafs = nullptr;
  KCD2S_CHECK(!scope(fixture.skeleton.data()));
  fixture.manager->global_cafs = fixture.global_cafs.values.data();
  for (const auto count : {0u, 1'000'001u, std::numeric_limits<std::uint32_t>::max()}) {
    fixture.global_cafs.count = count;
    KCD2S_CHECK(!scope(fixture.skeleton.data()));
  }
  fixture.global_cafs.count = 10;
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> invalid = {
      {{0, 6}, {6, 6}, {7, 6}, {3, 11}, {3, 0}, {3, 0xFFFFFFFFu}}};
  for (const auto& [begin, end] : invalid) {
    fixture.manager->facial_begin = begin;
    fixture.manager->facial_end = end;
    KCD2S_CHECK(!alias(fixture.skeleton.data(), 0));
    KCD2S_CHECK(!resolve(fixture.skeleton.data(), fixture.entries.values[0].hash));
    KCD2S_CHECK(!snapshot(fixture.skeleton.data()));
  }
  KCD2S_CHECK_EQ(fixture.set.entry_calls, 0u);
  KCD2S_CHECK_EQ(fixture.set.resolver_calls, 0u);
}

KCD2S_TEST(runtime_human_overlay, scalar_membership_snapshot_excludes_body_and_non_caf_entries) {
  Fixture fixture;
  fixture.entries.values[1].type = 1;
  fixture.entries.values[2].global_index = 6;
  const auto cached = snapshot(fixture.skeleton.data());
  KCD2S_CHECK(cached);
  KCD2S_CHECK_EQ(cached->clips.size(), 3u);
  KCD2S_CHECK(cached->contains(0));
  KCD2S_CHECK(!cached->contains(-1));
  KCD2S_CHECK(!cached->contains(1));
  KCD2S_CHECK(!cached->contains(2));
  KCD2S_CHECK(!cached->contains(3));
  KCD2S_CHECK(!cached->contains(std::numeric_limits<int>::max()));
  KCD2S_CHECK(scope(fixture.skeleton.data()) == cached->scope);
  KCD2S_CHECK_EQ(cached->scope.manager, reinterpret_cast<std::uintptr_t>(fixture.manager.get()));
  KCD2S_CHECK_EQ(cached->scope.global_cafs,
                 reinterpret_cast<std::uintptr_t>(fixture.global_cafs.values.data()));
  KCD2S_CHECK_EQ(cached->scope.global_caf_count, 10u);
  KCD2S_CHECK_EQ(cached->scope.facial_begin, 3u);
  KCD2S_CHECK_EQ(cached->scope.facial_end, 6u);

  fixture.manager->facial_begin = 4;
  fixture.entries.values[1].type = 0;
  const auto changed = snapshot(fixture.skeleton.data());
  KCD2S_CHECK(changed && changed->scope != cached->scope);
  KCD2S_CHECK(!changed->contains(0));
  KCD2S_CHECK(changed->contains(1));
  KCD2S_CHECK(cached->contains(0));
  KCD2S_CHECK(!cached->contains(1));

  fixture.entries.values[1].hash = 0;
  KCD2S_CHECK(!snapshot(fixture.skeleton.data())->contains(1));
}

KCD2S_TEST(runtime_human_overlay, manager_and_global_vector_changes_invalidate_scope) {
  Fixture fixture;
  const auto first = scope(fixture.skeleton.data());
  KCD2S_CHECK(first);
  auto replacement_manager = std::make_unique<Manager>(*fixture.manager);
  fixture.manager_pointer = replacement_manager.get();
  const auto manager_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(manager_changed && manager_changed != first);
  KCD2S_CHECK_EQ(manager_changed->entries, first->entries);

  GlobalCafs replacement_global;
  replacement_manager->global_cafs = replacement_global.values.data();
  const auto vector_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(vector_changed && vector_changed != manager_changed);
  KCD2S_CHECK_EQ(vector_changed->facial_begin, first->facial_begin);
  KCD2S_CHECK_EQ(vector_changed->facial_end, first->facial_end);

  replacement_global.count = 9;
  const auto count_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(count_changed && count_changed != vector_changed);
  KCD2S_CHECK_EQ(count_changed->global_caf_count, 9u);
  replacement_manager->facial_end = 7;
  const auto end_changed = scope(fixture.skeleton.data());
  KCD2S_CHECK(end_changed && end_changed != count_changed);
  KCD2S_CHECK_EQ(end_changed->facial_end, 7u);
}

KCD2S_TEST(runtime_human_overlay, complete_resident_facial_caf_selects_native_start_fast_path) {
  Fixture fixture;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  fixture.set_residency();
  KCD2S_CHECK(ready_for_start(fixture.skeleton.data(), 0));
  fixture.set_residency(3, 0x802);
  KCD2S_CHECK(ready_for_start(fixture.skeleton.data(), 0));
  fixture.set_residency(4);
  KCD2S_CHECK(ready_for_start(fixture.skeleton.data(), 1));
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 2));
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), -1));
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 3));
}

KCD2S_TEST(runtime_human_overlay,
           missing_readiness_bit_or_incomplete_controller_counts_defer_start) {
  Fixture fixture;
  for (const auto flags : {0u, 2u, 0x1000u}) {
    fixture.set_residency(3, flags);
    KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  }
  const std::array<std::array<std::uint16_t, 2>, 5> counts = {
      {{0, 0}, {0, 2}, {2, 0}, {1, 2}, {3, 2}}};
  for (const auto& [resident, expected] : counts) {
    fixture.set_residency(3, 0x800, resident, expected);
    KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  }
  fixture.set_residency();
  for (const auto count : {0u, 1u, 3u}) {
    fixture.controllers.count = count;
    KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  }
  fixture.controllers.count = 2;
  void* missing = nullptr;
  std::memcpy(fixture.global_cafs.values.data() + 168 * 3 + 56, &missing, sizeof(missing));
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
}

KCD2S_TEST(runtime_human_overlay, readiness_revalidates_current_identity_and_native_metadata) {
  Fixture fixture;
  fixture.set_residency();
  fixture.set_residency(2);
  KCD2S_CHECK(ready_for_start(fixture.skeleton.data(), 0));
  auto& entry = fixture.entries.values[0];
  entry.global_index = 2;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  entry.global_index = 3;
  entry.type = 1;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  entry.type = 0;
  fixture.set.entry_delta = 1;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  fixture.set.entry_delta = 0;
  fixture.set.resolver_override = 1;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  fixture.set.resolver_override.reset();
  GlobalCafs replacement;
  fixture.manager->global_cafs = replacement.values.data();
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  fixture.manager->global_cafs = fixture.global_cafs.values.data();
  KCD2S_CHECK(ready_for_start(fixture.skeleton.data(), 0));
  fixture.manager->facial_begin = 4;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
  fixture.manager->facial_begin = 3;
  fixture.manager_pointer = nullptr;
  KCD2S_CHECK(!ready_for_start(fixture.skeleton.data(), 0));
}
