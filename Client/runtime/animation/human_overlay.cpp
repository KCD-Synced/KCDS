#include "human_overlay.hpp"

#include <cstddef>
#include <cstring>
#include <limits>

namespace kcds::runtime::human_overlay {
namespace {

static_assert(sizeof(void*) == 8);
constexpr std::size_t kSkeletonCharacter = 1256;
constexpr std::size_t kCharacterModel = 2672;
constexpr std::size_t kModelAnimationSet = 536;
constexpr std::size_t kManagerGlobalCafs = 640;
constexpr std::size_t kManagerFacialBegin = 525728;
constexpr std::size_t kManagerFacialEnd = 525732;
constexpr std::size_t kEntryBytes = 16;
constexpr std::size_t kGlobalCafBytes = 168;
constexpr std::uint32_t kMaxEntries = 1'000'000;
Natives functions;

template <class T>
T read(const void* object, std::size_t offset = 0) {
  T value;
  std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(value));
  return value;
}

bool matches_slot(const void* object, unsigned slot, void* function) {
  const auto table = read<void*>(object);
  return table && read<void*>(table, sizeof(void*) * slot) == function;
}

std::optional<Scope> current_scope(void* skeleton) {
  if (!skeleton || !functions.getAnimationSet) {
    return std::nullopt;
  }
  const auto character = read<void*>(skeleton, kSkeletonCharacter);
  if (!character || !matches_slot(character, 5, functions.getSkeleton) ||
      !matches_slot(character, 15, functions.getAnimationSet) ||
      !matches_slot(character, 16, functions.getAnimationSet)) {
    return std::nullopt;
  }
  using Getter = void*(__fastcall*)(void*);
  if (reinterpret_cast<Getter>(functions.getSkeleton)(character) != skeleton) {
    return std::nullopt;
  }
  const auto model = read<void*>(character, kCharacterModel);
  if (!model) {
    return std::nullopt;
  }
  auto* set = reinterpret_cast<Getter>(functions.getAnimationSet)(character);
  if (!set || set != read<void*>(model, kModelAnimationSet) ||
      !matches_slot(set, 8, functions.resolveHash)) {
    return std::nullopt;
  }
  const auto entries = read<std::uintptr_t>(set, 8);
  if (entries < sizeof(std::uint32_t)) {
    return std::nullopt;
  }
  const auto count = read<std::uint32_t>(reinterpret_cast<const void*>(entries - 4));
  if (count == 0 || count > kMaxEntries ||
      entries > std::numeric_limits<std::uintptr_t>::max() - kEntryBytes * count) {
    return std::nullopt;
  }
  auto* manager = read<void*>(functions.characterManager);
  if (!manager) {
    return std::nullopt;
  }
  const auto global_cafs = read<std::uintptr_t>(manager, kManagerGlobalCafs);
  if (global_cafs < sizeof(std::uint32_t)) {
    return std::nullopt;
  }
  const auto global_count = read<std::uint32_t>(reinterpret_cast<const void*>(global_cafs - 4));
  const auto begin = read<std::uint32_t>(manager, kManagerFacialBegin);
  const auto end = read<std::uint32_t>(manager, kManagerFacialEnd);
  if (global_count == 0 || global_count > kMaxEntries || begin == 0 || begin >= end ||
      end > global_count ||
      global_cafs > std::numeric_limits<std::uintptr_t>::max() - kGlobalCafBytes * global_count) {
    return std::nullopt;
  }
  return Scope{reinterpret_cast<std::uintptr_t>(character),
               reinterpret_cast<std::uintptr_t>(model),
               reinterpret_cast<std::uintptr_t>(set),
               entries,
               count,
               reinterpret_cast<std::uintptr_t>(manager),
               global_cafs,
               global_count,
               begin,
               end};
}

bool facial_entry(const Scope& current, const void* entry) {
  const auto global_index = read<std::int32_t>(entry, 8);
  return read<std::uint64_t>(entry) != 0 && read<std::uint16_t>(entry, 12) == 0 &&
         global_index >= 0 && static_cast<std::uint32_t>(global_index) >= current.facial_begin &&
         static_cast<std::uint32_t>(global_index) < current.facial_end;
}

std::optional<std::uint64_t> entry_hash(const Scope& current, int id) {
  if (id < 0 || static_cast<std::uint32_t>(id) >= current.count) {
    return std::nullopt;
  }
  using Entry = void*(__fastcall*)(void*, int);
  const auto entry =
      reinterpret_cast<Entry>(functions.entry)(reinterpret_cast<void*>(current.animation_set), id);
  const auto expected = current.entries + kEntryBytes * static_cast<std::uint32_t>(id);
  if (!entry || reinterpret_cast<std::uintptr_t>(entry) != expected) {
    return std::nullopt;
  }
  if (!facial_entry(current, entry)) {
    return std::nullopt;
  }
  return read<std::uint64_t>(entry);
}

std::int64_t resolve_hash(void* set, std::uint64_t hash) {
  using Resolve = std::int64_t(__fastcall*)(void*, std::uint64_t);
  return reinterpret_cast<Resolve>(functions.resolveHash)(set, hash);
}

}  // namespace

bool install(const Natives& natives) {
  if (!natives.getAnimationSet || !natives.entry || !natives.resolveHash || !natives.getSkeleton ||
      !natives.characterManager) {
    functions = {};
    return false;
  }
  functions = natives;
  return true;
}

std::optional<Scope> scope(void* skeleton) {
  return current_scope(skeleton);
}

std::optional<Membership> snapshot(void* skeleton) {
  const auto current = current_scope(skeleton);
  if (!current) {
    return std::nullopt;
  }
  Membership membership;
  membership.scope = *current;
  membership.clips.resize(current->count);
  for (std::uint32_t id = 0; id < current->count; ++id) {
    const auto entry = reinterpret_cast<const void*>(current->entries + kEntryBytes * id);
    membership.clips[id] = facial_entry(*current, entry) ? 1 : 0;
  }
  if (current_scope(skeleton) != current) {
    return std::nullopt;
  }
  return membership;
}

bool ready_for_start(void* skeleton, int id) {
  const auto current = current_scope(skeleton);
  if (!current) {
    return false;
  }
  const auto hash = entry_hash(*current, id);
  if (!hash || resolve_hash(reinterpret_cast<void*>(current->animation_set), *hash) != id) {
    return false;
  }
  const auto entry = reinterpret_cast<const void*>(current->entries + kEntryBytes * id);
  const auto global_index = read<std::int32_t>(entry, 8);
  const auto header = reinterpret_cast<const void*>(
      current->global_cafs + kGlobalCafBytes * static_cast<std::uint32_t>(global_index));
  const auto flags = read<std::uint32_t>(header, 24);
  const auto resident = read<std::uint16_t>(header, 156);
  const auto expected = read<std::uint16_t>(header, 158);
  const auto controllers = read<std::uintptr_t>(header, 56);
  if ((flags & 0x800) == 0 || expected == 0 || resident != expected ||
      controllers < sizeof(std::uint32_t)) {
    return false;
  }
  const auto count = read<std::uint32_t>(reinterpret_cast<const void*>(controllers - 4));
  return count == resident && current_scope(skeleton) == current;
}

std::optional<std::uint64_t> alias(void* skeleton, int id) {
  const auto current = current_scope(skeleton);
  if (!current) {
    return std::nullopt;
  }
  const auto hash = entry_hash(*current, id);
  if (!hash || resolve_hash(reinterpret_cast<void*>(current->animation_set), *hash) != id) {
    return std::nullopt;
  }
  return hash;
}

std::optional<int> resolve(void* skeleton, std::uint64_t hash) {
  if (hash == 0) {
    return std::nullopt;
  }
  const auto current = current_scope(skeleton);
  if (!current) {
    return std::nullopt;
  }
  const auto id = resolve_hash(reinterpret_cast<void*>(current->animation_set), hash);
  if (id < 0 || id >= current->count) {
    return std::nullopt;
  }
  const auto found = entry_hash(*current, static_cast<int>(id));
  if (!found || *found != hash) {
    return std::nullopt;
  }
  return static_cast<int>(id);
}

}  // namespace kcds::runtime::human_overlay
