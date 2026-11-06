#include "perk_gate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::progression::perk_gate {
namespace {

namespace integration = kcd2s::integration;
using integration::PerkId;

using Learn = char(__fastcall*)(void* points, const PerkId* perk);
using AddPerk = bool(__fastcall*)(void* perk_set, const PerkId* perk, std::uint64_t source);
using PerkCheck = bool(__fastcall*)(void* perk_set, const PerkId* perk);
using Definition = const std::uint8_t*(__fastcall*)(void* definitions, const PerkId* perk);

constexpr std::size_t kSoulPerkSet = 1880;
constexpr std::size_t kSoulPerkPoints = 1968;
constexpr std::size_t kPointsPerkSet = 8;
// A perk definition: what pays for it (a stat id, or kSkillTree with the skill's id after it).
constexpr std::size_t kDefinitionTree = 72;
constexpr std::size_t kDefinitionSkill = 76;
constexpr std::uint32_t kSkillTree = 10;
constexpr std::uint32_t kMainLevelSkill = 35;
// The perk points' counters: stats at + 16 + 2 * stat, skills at + 36 + 2 * skill, the main
// level at + 106.
constexpr std::size_t kStatPoints = 16;
constexpr std::size_t kSkillPoints = 36;
constexpr std::size_t kMainPoints = 106;
// Requests not yet answered: far more than a player clicks; beyond it the oldest go.
constexpr std::size_t kMaxPending = 64;

Logger log_line = nullptr;
Natives natives;
Learn original_learn = nullptr;
AddPerk original_add_perk = nullptr;
void* local_points = nullptr;
void* local_perk_set = nullptr;
std::vector<PerkId> blocked;
std::vector<PerkId> pending;
std::vector<PerkId> requests;
std::vector<std::pair<PerkId, integration::PerkRefusal>> refusals;
int server_grants = 0;
bool server_learn = false;

template <class T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

bool contains(const std::vector<PerkId>& perks, const PerkId& perk) {
  return std::find(perks.begin(), perks.end(), perk) != perks.end();
}

// Whether the game's learn would learn the perk: offered, not refused by the add, a point left
// in the tree that pays for it.
bool learnable(void* points, const PerkId& perk) {
  void* perk_set = field<void*>(points, kPointsPerkSet);
  if (!perk_set || !reinterpret_cast<PerkCheck>(natives.offered)(perk_set, &perk) ||
      reinterpret_cast<PerkCheck>(natives.add_refused)(perk_set, &perk)) {
    return false;
  }
  const std::uint8_t* definition =
      reinterpret_cast<Definition>(natives.definition)(natives.definitions, &perk);
  if (!definition) {
    return false;
  }
  const auto tree = field<std::uint32_t>(definition, kDefinitionTree);
  std::size_t counter = kStatPoints + 2 * std::size_t(tree);
  if (tree == kSkillTree) {
    const auto skill = field<std::uint32_t>(definition, kDefinitionSkill);
    counter = skill == kMainLevelSkill ? kMainPoints : kSkillPoints + 2 * std::size_t(skill);
  }
  return field<std::uint16_t>(points, counter) > 0;
}

char __fastcall hooked_learn(void* points, const PerkId* perk) {
  if (server_learn || !local_points || points != local_points || !perk ||
      !learnable(points, *perk)) {
    return original_learn(points, perk);
  }
  // Asked once: a second confirmation while the server decides is dropped.
  if (!contains(pending, *perk)) {
    if (pending.size() >= kMaxPending) {
      pending.erase(pending.begin());
    }
    pending.push_back(*perk);
    requests.push_back(*perk);
  }
  return 0;
}

bool __fastcall hooked_add_perk(void* perk_set, const PerkId* perk, std::uint64_t source) {
  if (server_grants == 0 && perk_set && perk_set == local_perk_set && perk &&
      contains(blocked, *perk)) {
    if (log_line) {
      log_line("PROGRESSION blocked perk not added: %s", integration::perk_id_text(*perk).c_str());
    }
    return false;
  }
  return original_add_perk(perk_set, perk, source);
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  log_line = logger;
  natives = resolved;
  if (!natives.learn || !natives.add_perk || !natives.offered || !natives.add_refused ||
      !natives.definition || !natives.definitions) {
    return false;
  }
  return MH_CreateHook(natives.learn, reinterpret_cast<void*>(hooked_learn),
                       reinterpret_cast<void**>(&original_learn)) == MH_OK &&
         MH_EnableHook(natives.learn) == MH_OK &&
         MH_CreateHook(natives.add_perk, reinterpret_cast<void*>(hooked_add_perk),
                       reinterpret_cast<void**>(&original_add_perk)) == MH_OK &&
         MH_EnableHook(natives.add_perk) == MH_OK;
}

void tick(void* soul) {
  if (!soul) {
    local_points = nullptr;
    local_perk_set = nullptr;
    blocked.clear();
    pending.clear();
    requests.clear();
    refusals.clear();
    return;
  }
  local_points = field<void*>(soul, kSoulPerkPoints);
  local_perk_set = static_cast<std::uint8_t*>(soul) + kSoulPerkSet;
}

std::vector<PerkId> take_requests() {
  if (log_line && !requests.empty()) {
    log_line("PROGRESSION perks asked of the server: %zu (first %s)", requests.size(),
             integration::perk_id_text(requests.front()).c_str());
  }
  return std::exchange(requests, {});
}

bool learn(void* soul, const PerkId& perk) {
  pending.erase(std::remove(pending.begin(), pending.end(), perk), pending.end());
  void* points = field<void*>(soul, kSoulPerkPoints);
  if (!points || !original_learn) {
    return false;
  }
  // The learn loads the GUID with movaps: it has to be 16-byte aligned.
  alignas(16) const PerkId aligned = perk;
  ServerGrant grant;
  server_learn = true;
  const bool learnt = original_learn(points, &aligned) != 0;
  server_learn = false;
  return learnt;
}

void refused(const PerkId& perk, integration::PerkRefusal refusal) {
  pending.erase(std::remove(pending.begin(), pending.end(), perk), pending.end());
  if (refusals.size() >= kMaxPending) {
    refusals.erase(refusals.begin());
  }
  refusals.emplace_back(perk, refusal);
}

std::vector<std::pair<PerkId, integration::PerkRefusal>> take_refusals() {
  return std::exchange(refusals, {});
}

void set_blocked(std::vector<PerkId> perks) {
  blocked = std::move(perks);
}

ServerGrant::ServerGrant() {
  ++server_grants;
}

ServerGrant::~ServerGrant() {
  --server_grants;
}

}  // namespace kcds::runtime::progression::perk_gate
