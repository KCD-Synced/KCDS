#include "replica_buffs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "custom_buffs.hpp"

namespace kcds::runtime::buffs::replica {
namespace {

namespace integration = kcd2s::integration;
using integration::Guid;
using integration::InjuryState;
using integration::ReplicaBuff;

using GetManager = void*(__fastcall*)();
using AddBuff = void*(__fastcall*)(void* manager, void* soul, const Guid* buff, void* source,
                                   const void* params);
using RemoveInstance = bool(__fastcall*)(void* manager, const std::uint64_t* instance);
// C_InjuredBuffInitParams: duration, injury type, the two flags.
using CreateInjuredParams = void*(__fastcall*)(void** out, const float* duration,
                                               const unsigned* type, const char* flag,
                                               const char* extra);
using DeleteParams = void(__fastcall*)(void* params, char free);

constexpr std::size_t kSoulBuffsBegin = 1352;
constexpr std::size_t kSoulBuffsEnd = 1360;
constexpr std::size_t kInstanceId = 8;
constexpr std::size_t kInstanceDefinition = 16;
constexpr std::size_t kDefinitionClass = 32;
// C_InjuredSoulBuffInstance (its init): severity, type and the two flags.
constexpr std::size_t kInjurySeverity = 216;
constexpr std::size_t kInjuryType = 220;
constexpr std::size_t kInjuryFlag = 232;
constexpr std::size_t kInjuryExtra = 244;
// C_DrunkennessBuff (buff "drunkenness", ff92671b-...), as read live: + 216 the alcohol still to
// be taken in (falls as it is), + 220 the drunkenness (rises with it, falls as the body sobers).
// The replica takes the owner's drunkenness and nothing to take in, so it follows the owner's
// reports instead of a course of its own.
constexpr std::size_t kAlcoholToAbsorb = 216;
constexpr std::size_t kDrunkennessAmount = 220;
constexpr char kDrunkennessBuff[] = "ff92671b-2a82-4def-8d59-51627e0ecfc7";
constexpr std::size_t kAddSlot = 0;
constexpr std::size_t kRemoveInstanceSlot = 3;
// More instances than a body carries; a soul with more is not read.
constexpr std::size_t kMaxInstances = 256;

Logger log_line = nullptr;
Natives natives;

template <class T>
T field(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

struct Instance {
  ReplicaBuff entry;
  std::uint64_t id = 0;
  std::uint8_t* address = nullptr;
};

// Whether the instance is the game's injury (its vtable's init is the injury's).
bool injury_instance(const void* instance) {
  return natives.injured_init && *static_cast<void* const* const*>(instance) &&
         (*static_cast<void* const* const*>(instance))[0] == natives.injured_init;
}

bool drunkenness_buff(const Guid& buff) {
  static Guid drunkenness{};
  static const bool parsed = integration::parse_guid(kDrunkennessBuff, drunkenness);
  return parsed && buff == drunkenness;
}

// The drunkenness amount in thousandths (integration::kDrunkennessStep steps).
std::int32_t drunkenness_of(const std::uint8_t* instance) {
  const float amount = field<float>(instance, kDrunkennessAmount);
  if (!(amount > 0.0f)) {
    return 0;
  }
  const auto thousandths =
      std::int32_t(std::min(double(amount) * 1000.0, double(integration::kDrunkennessMax)));
  return thousandths - thousandths % integration::kDrunkennessStep;
}

void set_drunkenness(std::uint8_t* instance, std::int32_t thousandths) {
  const float amount = float(thousandths) / 1000.0f;
  const float nothing = 0.0f;
  std::memcpy(instance + kDrunkennessAmount, &amount, sizeof(amount));
  std::memcpy(instance + kAlcoholToAbsorb, &nothing, sizeof(nothing));
}

InjuryState injury_of(const std::uint8_t* instance) {
  InjuryState injury;
  injury.type = field<std::uint32_t>(instance, kInjuryType);
  injury.flags = std::uint8_t((instance[kInjuryFlag] ? 1 : 0) | (instance[kInjuryExtra] ? 2 : 0));
  injury.severity = std::clamp(field<std::int32_t>(instance, kInjurySeverity), 0,
                               integration::kInjurySeverityOne);
  return injury;
}

// The soul's instances of mirrored classes, in the soul's order.
std::vector<Instance> mirrored(void* soul) {
  std::vector<Instance> instances;
  if (!soul) {
    return instances;
  }
  const auto* begin = field<void* const*>(soul, kSoulBuffsBegin);
  const auto* end = field<void* const*>(soul, kSoulBuffsEnd);
  if (!begin || end < begin || std::size_t(end - begin) > kMaxInstances) {
    return instances;
  }
  for (auto it = begin; it != end; ++it) {
    auto* instance = static_cast<std::uint8_t*>(*it);
    const void* definition = instance ? field<const void*>(instance, kInstanceDefinition) : nullptr;
    // A resource's own buff has no effects in the game: the server applies them.
    if (!definition || custom::owns(definition) ||
        !integration::replica_buff_class(field<std::uint32_t>(definition, kDefinitionClass))) {
      continue;
    }
    Instance mirrored_instance;
    std::memcpy(mirrored_instance.entry.buff.data(), definition,
                mirrored_instance.entry.buff.size());
    if (injury_instance(instance)) {
      mirrored_instance.entry.injury = injury_of(instance);
    } else if (drunkenness_buff(mirrored_instance.entry.buff)) {
      mirrored_instance.entry.amount = drunkenness_of(instance);
    }
    mirrored_instance.id = field<std::uint64_t>(instance, kInstanceId);
    mirrored_instance.address = instance;
    instances.push_back(mirrored_instance);
  }
  return instances;
}

bool known(const std::vector<Guid>& buffs, const Guid& buff) {
  return std::find(buffs.begin(), buffs.end(), buff) != buffs.end();
}

// Adds the buff to the soul as the game would: an injury through the game's own injury
// parameters with the owner's numbers, any other plainly. The new instance, null for none.
void* add(void* manager, void* soul, const ReplicaBuff& buff) {
  // The game loads GUIDs and parameters with aligned moves.
  alignas(16) Guid id = buff.buff;
  alignas(16) const std::uint8_t perk[16] = {};
  void* params = nullptr;
  if (buff.injury && natives.create_injured_params) {
    const float duration = float(buff.injury->severity) / float(integration::kInjurySeverityOne);
    const unsigned type = buff.injury->type;
    const char flag = char(buff.injury->flags & 1);
    const char extra = char((buff.injury->flags >> 1) & 1);
    reinterpret_cast<CreateInjuredParams>(natives.create_injured_params)(&params, &duration, &type,
                                                                         &flag, &extra);
  }
  void* instance = slot<AddBuff>(manager, kAddSlot)(manager, soul, &id, params, perk);
  if (params) {
    // The buff took its numbers in its init; the parameters are the caller's to delete.
    slot<DeleteParams>(params, 0)(params, 1);
  }
  return instance;
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  natives = resolved;
  log_line = logger;
  return natives.manager != nullptr;
}

std::vector<ReplicaBuff> read(void* soul) {
  std::vector<ReplicaBuff> buffs;
  for (const Instance& instance : mirrored(soul)) {
    if (buffs.size() == integration::kMaxReplicaBuffs) {
      break;
    }
    ReplicaBuff entry = instance.entry;
    if (entry.injury) {
      // Whole steps only: the game heals it a little every moment.
      entry.injury->severity -= entry.injury->severity % integration::kInjurySeverityStep;
    }
    buffs.push_back(entry);
  }
  std::sort(buffs.begin(), buffs.end());
  return buffs;
}

unsigned apply(void* replica_soul, const std::vector<ReplicaBuff>& owner, Mirror& mirror) {
  void* manager = natives.manager ? reinterpret_cast<GetManager>(natives.manager)() : nullptr;
  if (!replica_soul || !manager) {
    return 0;
  }
  // Each of the owner's instances keeps one of the replica's of the same buff (an injury of the
  // same type); the rest go.
  std::vector<Instance> current = mirrored(replica_soul);
  std::vector<ReplicaBuff> missing;
  unsigned changed = 0;
  for (const ReplicaBuff& buff : owner) {
    const auto kept = std::find_if(current.begin(), current.end(), [&](const Instance& instance) {
      return instance.entry.buff == buff.buff &&
             (!buff.injury ||
              (instance.entry.injury && instance.entry.injury->type == buff.injury->type));
    });
    if (kept == current.end()) {
      missing.push_back(buff);
      continue;
    }
    if (buff.injury && kept->entry.injury &&
        (kept->entry.injury->flags != buff.injury->flags ||
         std::abs(kept->entry.injury->severity - buff.injury->severity) >=
             integration::kInjurySeverityStep)) {
      // The owner's injury healed or worsened: the replica's takes its numbers.
      std::memcpy(kept->address + kInjurySeverity, &buff.injury->severity, 4);
      kept->address[kInjuryFlag] = std::uint8_t(buff.injury->flags & 1);
      kept->address[kInjuryExtra] = std::uint8_t((buff.injury->flags >> 1) & 1);
      ++changed;
    }
    if (buff.amount && kept->entry.amount && *kept->entry.amount != *buff.amount) {
      // The owner drank more, or sobered up: the replica's takes the amount.
      set_drunkenness(kept->address, *buff.amount);
      ++changed;
    }
    current.erase(kept);
  }
  // Missing again after the last apply() added it: it does not hold here.
  for (const Guid& buff : mirror.added) {
    const bool gone = std::any_of(missing.begin(), missing.end(),
                                  [&](const ReplicaBuff& entry) { return entry.buff == buff; });
    if (gone && !known(mirror.not_holding, buff)) {
      mirror.not_holding.push_back(buff);
      if (log_line) {
        log_line("REPLICA BUFFS %s does not hold on the replica",
                 integration::guid_text(buff).c_str());
      }
    }
  }
  mirror.added.clear();
  missing.erase(std::remove_if(missing.begin(), missing.end(),
                               [&](const ReplicaBuff& entry) {
                                 return known(mirror.not_holding, entry.buff);
                               }),
                missing.end());
  for (const Instance& extra : current) {
    const std::uint64_t id = extra.id;
    slot<RemoveInstance>(manager, kRemoveInstanceSlot)(manager, &id);
    ++changed;
  }
  for (const ReplicaBuff& buff : missing) {
    if (void* instance = add(manager, replica_soul, buff)) {
      if (buff.amount) {
        // Added without the owner's amount it ends at once; it starts with the owner's.
        set_drunkenness(static_cast<std::uint8_t*>(instance), *buff.amount);
      }
      mirror.added.push_back(buff.buff);
      ++changed;
      continue;
    }
    mirror.not_holding.push_back(buff.buff);
    if (log_line) {
      log_line("REPLICA BUFFS %s refused", integration::guid_text(buff.buff).c_str());
    }
  }
  return changed;
}

}  // namespace kcds::runtime::buffs::replica
