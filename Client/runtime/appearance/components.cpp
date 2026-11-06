#include "components.hpp"

#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

namespace kcds::runtime::appearance::components {
namespace {

using kcd2s::integration::Appearance;
using kcd2s::integration::AppearanceRequest;
using kcd2s::integration::BodyParts;

using SetComponent = void(__fastcall*)(void* soul, std::uint8_t part, void* name, int set);
using StringCall = void(__fastcall*)(void* text);

// The parts of a component set, in the game's order.
enum Part : std::uint8_t {
  kBody = 0,
  kHead = 1,
  kHair = 2,
  kBeard = 3,
  kUnderwear = 4,
};

constexpr std::size_t kActorSoul = 1640;
constexpr std::size_t kSoulBase = 1072;
constexpr std::size_t kSoulOverride = 2784;
// 's set argument: 1 is the override set at soul + 2784, 2 the set at soul + 3176, which
// the inventory paper-doll is built from (the game copies the player's look into it) and which
// comes before the override set.
struct ComponentSet {
  int index;
  std::size_t offset;
};
constexpr ComponentSet kOverrideSet{1, kSoulOverride};
constexpr ComponentSet kDummySet{2, 3176};
// Set 0 is the soul's base set, inline at soul + 1072: fills it with a copy
// of the override set with the one part changed. The game builds a body part by part, the override
// set's part over the base set's: a male base body (the player's henry_body) brings its own hands,
// feet, arms and legs, which a female body (one torso skin) does not replace, so they stayed on the
// female figure, bent by her skeleton.
constexpr std::size_t kBaseSetIndex = 0;
constexpr char kMale[] = "male";
constexpr char kFemale[] = "female";

// soul + 3256: the soul's archetype (S_SoulArchetype, Libs/Tables/rpg/soul_archetype.xml), the
// records kept in id order 88 bytes apart; its id at + 8 and its gender at + 28 (1 male, 2
// female). The gender decides which half of the component tree the soul is built from. The soul
// keeps the archetype's id at + 884 and resolves the record from it whenever it is
// copied, as for the inventory paper-doll's soul.
constexpr std::size_t kSoulArchetypeId = 884;
constexpr std::size_t kSoulArchetype = 3256;
constexpr std::size_t kArchetypeSize = 88;
constexpr std::size_t kArchetypeId = 8;
constexpr std::size_t kArchetypeGender = 28;
constexpr std::uint8_t kGenderMale = 1;
constexpr std::uint8_t kGenderFemale = 2;
// Archetypes and their counterpart of the other gender: the player's Hero / Hero_female and the
// NPC / NPC_Female a replica's soul has.
struct GenderPair {
  int male;
  int female;
};
constexpr GenderPair kGenderPairs[] = {{13, 15}, {0, 1}};

// What a female body wears where nothing was chosen: the base set is the male player's.
constexpr char kFemaleBody[] = "female_body";
constexpr char kFemaleHead[] = "f_head_000";
constexpr char kFemaleHair[] = "f_hair_katherine";
constexpr char kFemaleUnderwear[] = "f_underwear01_m01";

Natives natives;
Logger log_line = nullptr;

// The "bodyParts" field a player's replica actor was last built from.
struct Replica {
  void* actor = nullptr;
  std::string field;
};
std::unordered_map<std::uint32_t, Replica> replicas;
// The base set's parts of a soul whose base was made female, to give back when it is male again.
std::unordered_map<void*, BodyParts> male_bases;

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

bool installed() {
  return natives.set_component && natives.string_init && natives.string_free &&
         natives.string_assign;
}

void* soul_of(void* actor) {
  return actor ? read<void*>(actor, kActorSoul) : nullptr;
}

std::string read_name(const void* set, Part part) {
  const char* text = read<const char*>(set, std::size_t(part) * sizeof(void*));
  return text ? std::string(text) : std::string();
}

std::uint8_t* archetype_of(void* soul) {
  return read<std::uint8_t*>(soul, kSoulArchetype);
}

std::string gender_of(void* soul) {
  const std::uint8_t* archetype = archetype_of(soul);
  return archetype && archetype[kArchetypeGender] == kGenderFemale ? kFemale : kMale;
}

// The archetype record with that id and gender next to the given one, or null.
std::uint8_t* counterpart(std::uint8_t* archetype, int id, std::uint8_t gender) {
  std::uint8_t* record =
      archetype + (id - read<int>(archetype, kArchetypeId)) * std::ptrdiff_t(kArchetypeSize);
  if (read<int>(record, kArchetypeId) != id || record[kArchetypeGender] != gender) {
    return nullptr;
  }
  return record;
}

// Gives the soul the archetype of its kind and of that gender (Hero / Hero_female, NPC /
// NPC_Female). False for an archetype without a counterpart.
bool set_gender(void* soul, const std::string& gender) {
  std::uint8_t* archetype = archetype_of(soul);
  if (!archetype) {
    return false;
  }
  if (gender_of(soul) == gender) {
    return true;
  }
  const int id = read<int>(archetype, kArchetypeId);
  for (const GenderPair& pair : kGenderPairs) {
    std::uint8_t* target = nullptr;
    if (gender == kFemale && id == pair.male) {
      target = counterpart(archetype, pair.female, kGenderFemale);
    } else if (gender == kMale && id == pair.female) {
      target = counterpart(archetype, pair.male, kGenderMale);
    }
    if (target) {
      const int target_id = read<int>(target, kArchetypeId);
      auto* bytes = static_cast<std::uint8_t*>(soul);
      std::memcpy(bytes + kSoulArchetypeId, &target_id, sizeof(target_id));
      std::memcpy(bytes + kSoulArchetype, &target, sizeof(target));
      return true;
    }
  }
  return false;
}

void set_part(void* soul, ComponentSet set, Part part, const std::string& name) {
  void* text = nullptr;
  reinterpret_cast<StringCall>(natives.string_init)(&text);
  natives.string_assign(&text, name.c_str());
  reinterpret_cast<SetComponent>(natives.set_component)(soul, part, &text, set.index);
  reinterpret_cast<StringCall>(natives.string_free)(&text);
}

// The five names a body of that appearance is built from: a male body takes the base set's part
// where nothing was chosen (an empty name), a female one has no base set of its own and takes
// the female defaults.
BodyParts parts_of(const Appearance& appearance) {
  if (appearance.gender != kFemale) {
    return {appearance.body, appearance.head, appearance.hair, appearance.beard, ""};
  }
  return {appearance.body.empty() ? kFemaleBody : appearance.body,
          appearance.head.empty() ? kFemaleHead : appearance.head,
          appearance.hair.empty() ? kFemaleHair : appearance.hair, "", kFemaleUnderwear};
}

// The parts of the soul's base set.
BodyParts base_parts(void* soul) {
  const void* base = static_cast<const std::uint8_t*>(soul) + kSoulBase;
  BodyParts parts(std::size_t(kUnderwear) + 1);
  for (Part part : {kBody, kHead, kHair, kBeard, kUnderwear}) {
    parts[part] = read_name(base, part);
  }
  return parts;
}

// Fills the base set with the override set's names and `body` (with set 0).
void copy_to_base(void* soul, const std::string& body) {
  void* text = nullptr;
  reinterpret_cast<StringCall>(natives.string_init)(&text);
  natives.string_assign(&text, body.c_str());
  reinterpret_cast<SetComponent>(natives.set_component)(soul, kBody, &text, kBaseSetIndex);
  reinterpret_cast<StringCall>(natives.string_free)(&text);
}

// A female body is built on a female base set, so nothing of a male base body shows through
// a soul made male again gets its own base set back.
void keep_base(void* soul, const std::string& gender, const BodyParts& wanted) {
  const auto saved = male_bases.find(soul);
  if (gender == kFemale) {
    if (base_parts(soul)[kBody] == wanted[kBody]) {
      return;
    }
    if (saved == male_bases.end()) {
      male_bases[soul] = base_parts(soul);
    }
    copy_to_base(soul, wanted[kBody]);
    if (log_line) {
      log_line("APPEARANCE base set made female (body=%s)", wanted[kBody].c_str());
    }
    return;
  }
  if (saved == male_bases.end()) {
    return;
  }
  // The base set takes the override set's names: the saved ones go there first, the wanted ones
  // come back after (put_parts).
  const BodyParts original = saved->second;
  male_bases.erase(saved);
  for (Part part : {kBody, kHead, kHair, kBeard, kUnderwear}) {
    set_part(soul, kOverrideSet, part, original[part]);
  }
  copy_to_base(soul, original[kBody]);
  if (log_line) {
    log_line("APPEARANCE base set given back (body=%s)", original[kBody].c_str());
  }
}

// Gives the soul the gender, then puts each of the five names in the set where it differs; the
// face before the beard, which is modelled for it.
bool put_parts(void* soul, const std::string& gender, const BodyParts& wanted,
               ComponentSet set = kOverrideSet) {
  if (!set_gender(soul, gender)) {
    if (log_line) {
      log_line("APPEARANCE refused gender=%s: the soul has no archetype of it", gender.c_str());
    }
    return false;
  }
  const bool own_look = set.index == kOverrideSet.index;
  if (own_look && gender == kMale) {
    keep_base(soul, gender, wanted);
  }
  unsigned changed = 0;
  for (Part part : {kBody, kHead, kHair, kBeard, kUnderwear}) {
    const void* chosen = read<const void*>(soul, set.offset);
    const std::string current = chosen ? read_name(chosen, part) : std::string();
    if (current == wanted[part]) {
      continue;
    }
    set_part(soul, set, part, wanted[part]);
    ++changed;
  }
  if (own_look && gender == kFemale) {
    keep_base(soul, gender, wanted);
  }
  return changed != 0;
}

bool put_on(void* actor, const Appearance& wanted) {
  void* soul = soul_of(actor);
  if (!soul || !installed() || (wanted.gender != kMale && wanted.gender != kFemale)) {
    return false;
  }
  const BodyParts parts = parts_of(wanted);
  if (put_parts(soul, wanted.gender, parts) && log_line) {
    log_line("APPEARANCE gender=%s body=%s head=%s hair=%s beard=%s", wanted.gender.c_str(),
             parts[kBody].c_str(), parts[kHead].c_str(), parts[kHair].c_str(),
             parts[kBeard].c_str());
  }
  return true;
}

}  // namespace

void install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

Appearance worn(void* actor) {
  Appearance appearance;
  void* soul = soul_of(actor);
  if (!soul) {
    return appearance;
  }
  appearance.gender = gender_of(soul);
  const void* set = read<const void*>(soul, kSoulOverride);
  if (!set) {
    return appearance;
  }
  appearance.body = read_name(set, kBody);
  appearance.head = read_name(set, kHead);
  appearance.hair = read_name(set, kHair);
  appearance.beard = read_name(set, kBeard);
  return appearance;
}

bool put_on_request(void* actor, const AppearanceRequest& request) {
  Appearance appearance = worn(actor);
  if (request.gender && *request.gender != appearance.gender) {
    // The other tree's names mean nothing on this one: a gender change starts from its defaults.
    appearance = Appearance{};
    appearance.gender = *request.gender;
  }
  const std::pair<const std::optional<std::string>*, std::string*> parts[] = {
      {&request.body, &appearance.body},
      {&request.head, &appearance.head},
      {&request.hair, &appearance.hair},
      {&request.beard, &appearance.beard},
  };
  for (const auto& [named, part] : parts) {
    if (*named) {
      *part = **named;
    }
  }
  return put_on(actor, appearance);
}

bool dress_dummy(void* actor, const Appearance& appearance) {
  void* soul = soul_of(actor);
  if (!soul || !installed() || (appearance.gender != kMale && appearance.gender != kFemale)) {
    return false;
  }
  BodyParts parts = parts_of(appearance);
  if (appearance.gender == kMale) {
    // The paper-doll's set comes before the base set: a part nobody chose is the player's own.
    const void* base = static_cast<const std::uint8_t*>(soul) + kSoulBase;
    for (Part part : {kBody, kHead, kHair, kBeard, kUnderwear}) {
      if (parts[part].empty()) {
        parts[part] = read_name(base, part);
      }
    }
  }
  put_parts(soul, appearance.gender, parts, kDummySet);
  return true;
}

void keep_doll_base(void* actor) {
  void* soul = soul_of(actor);
  const void* doll = soul ? read<const void*>(soul, kDummySet.offset) : nullptr;
  if (!doll || !installed()) {
    return;
  }
  // The game gives the figure the player's archetype and copies the look into its own set.
  BodyParts wanted(std::size_t(kUnderwear) + 1);
  wanted[kBody] = read_name(doll, kBody);
  keep_base(soul, gender_of(soul), wanted);
}

BodyParts built_from(void* actor) {
  void* soul = soul_of(actor);
  if (!soul) {
    return {};
  }
  const void* base = static_cast<const std::uint8_t*>(soul) + kSoulBase;
  const void* chosen = read<const void*>(soul, kSoulOverride);
  BodyParts parts;
  // The base set is the male player's: a female body is built from its chosen parts alone.
  const std::string gender = gender_of(soul);
  for (Part part : {kBody, kHead, kHair, kBeard, kUnderwear}) {
    std::string name = chosen ? read_name(chosen, part) : std::string();
    parts.push_back(name.empty() && gender == kMale ? read_name(base, part) : name);
  }
  parts.push_back(gender);
  return parts;
}

void show(std::uint32_t player, void* actor, const std::string& field) {
  Replica& replica = replicas[player];
  if (replica.actor == actor && replica.field == field) {
    return;
  }
  BodyParts wanted;
  void* soul = soul_of(actor);
  if (!soul || !installed() || !kcd2s::integration::parse_body_parts_field(field, wanted)) {
    return;
  }
  const std::string gender = wanted.back();
  wanted.pop_back();
  const bool changed = put_parts(soul, gender, wanted);
  replica.actor = actor;
  replica.field = field;
  if (log_line && changed) {
    log_line("APPEARANCE replica player=%u built from %s", player, field.c_str());
  }
}

void forget(std::uint32_t player) {
  replicas.erase(player);
}

}  // namespace kcds::runtime::appearance::components
