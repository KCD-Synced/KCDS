#include "grime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "../core/game_layouts.hpp"
#include "outfit.hpp"

namespace kcds::runtime::appearance::grime {
namespace {

using ItemState = void*(__fastcall*)(void* item);
using Changed = void(__fastcall*)(void* data, void* key);

constexpr char kMagic[] = "KG1";
constexpr std::size_t kMagicSize = 3;
// C_ArmorRuntimeData: the dirt and the six zones' blood, its change flags.
constexpr std::size_t kDirt = 168;
constexpr std::size_t kStateBytes = 7;
constexpr std::size_t kChangeFlags = 176;
constexpr std::size_t kChangePending = 180;
// The actor's dirt helper and its body data.
constexpr std::size_t kActorDirtHelper = 608;
constexpr std::size_t kHelperBody = 24;
// An item's class (its id at + 8) and health.
constexpr std::size_t kItemClass = kcds::layouts::ItemInstance::kClass;
constexpr std::size_t kClassId = 8;
constexpr std::size_t kItemHealth = 0x54;
constexpr std::size_t kPieceBytes = 16 + kStateBytes + 1;
constexpr std::size_t kMaxPieces = 64;

using State = std::array<std::uint8_t, kStateBytes>;
using ClassId = std::array<std::uint8_t, 16>;

Natives natives;
Logger log_line = nullptr;
bool installed = false;

struct Applied {
  std::string grime;
  std::vector<void*> pieces;
};
std::unordered_map<std::uint32_t, Applied> applied;

template <class T>
T read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

void* body_data(void* actor) {
  void* helper = read<void*>(actor, kActorDirtHelper);
  return helper ? read<void*>(helper, kHelperBody) : nullptr;
}

void* item_data(void* item) {
  return reinterpret_cast<ItemState>(natives.item_state)(item);
}

bool class_of(void* item, ClassId& id) {
  void* klass = read<void*>(item, kItemClass);
  if (!klass) {
    return false;
  }
  std::memcpy(id.data(), static_cast<std::uint8_t*>(klass) + kClassId, id.size());
  return true;
}

State state_of(void* data) {
  State state{};
  if (data) {
    std::memcpy(state.data(), static_cast<std::uint8_t*>(data) + kDirt, state.size());
  }
  return state;
}

// Puts a state on a runtime data as a load from a save does, when it differs.
void put_state(void* data, const State& state) {
  auto* bytes = static_cast<std::uint8_t*>(data);
  if (std::memcmp(bytes + kDirt, state.data(), state.size()) == 0) {
    return;
  }
  std::memcpy(bytes + kDirt, state.data(), state.size());
  *reinterpret_cast<std::uint32_t*>(bytes + kChangeFlags) |= 3u;
  *reinterpret_cast<std::uint32_t*>(bytes + kChangePending) = 1;
  reinterpret_cast<Changed>(natives.changed)(data, natives.full_state);
}

std::uint8_t wear_byte(float health) {
  if (!std::isfinite(health)) {
    return 255;
  }
  const float clamped = health < 0 ? 0 : (health > 1 ? 1 : health);
  return std::uint8_t(std::lround(clamped * 255.0f));
}

struct Piece {
  ClassId item{};
  State state{};
  std::uint8_t wear = 255;
  bool used = false;
};

bool parse(const std::string& grime, State& body, std::vector<Piece>& pieces) {
  if (grime.size() < kMagicSize + kStateBytes || grime.compare(0, kMagicSize, kMagic) != 0 ||
      (grime.size() - kMagicSize - kStateBytes) % kPieceBytes != 0) {
    return false;
  }
  std::size_t at = kMagicSize;
  std::memcpy(body.data(), grime.data() + at, body.size());
  at += body.size();
  while (at < grime.size() && pieces.size() < kMaxPieces) {
    Piece piece;
    std::memcpy(piece.item.data(), grime.data() + at, piece.item.size());
    std::memcpy(piece.state.data(), grime.data() + at + 16, piece.state.size());
    piece.wear = std::uint8_t(grime[at + 16 + kStateBytes]);
    pieces.push_back(piece);
    at += kPieceBytes;
  }
  return true;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.item_state && natives.changed && natives.full_state;
  return installed;
}

std::string capture(void* actor) {
  if (!installed || !actor) {
    return {};
  }
  std::string grime(kMagic, kMagicSize);
  const State body = state_of(body_data(actor));
  grime.append(reinterpret_cast<const char*>(body.data()), body.size());
  std::size_t pieces = 0;
  for (void* item : outfit::worn(actor)) {
    void* data = item_data(item);
    ClassId id{};
    if (!data || !class_of(item, id) || pieces >= kMaxPieces) {
      continue;
    }
    const State state = state_of(data);
    grime.append(reinterpret_cast<const char*>(id.data()), id.size());
    grime.append(reinterpret_cast<const char*>(state.data()), state.size());
    grime.push_back(char(wear_byte(read<float>(item, kItemHealth))));
    ++pieces;
  }
  return grime;
}

void apply(std::uint32_t player, void* actor, const std::string& grime) {
  if (!installed || !actor || grime.empty()) {
    return;
  }
  std::vector<void*> worn = outfit::worn(actor);
  Applied& last = applied[player];
  if (last.grime == grime && last.pieces == worn) {
    return;
  }
  State body{};
  std::vector<Piece> pieces;
  if (!parse(grime, body, pieces)) {
    return;
  }
  last.grime = grime;
  last.pieces = worn;
  if (void* data = body_data(actor)) {
    put_state(data, body);
  }
  std::size_t matched = 0;
  for (void* item : worn) {
    void* data = item_data(item);
    ClassId id{};
    if (!data || !class_of(item, id)) {
      continue;
    }
    for (Piece& piece : pieces) {
      if (piece.used || piece.item != id) {
        continue;
      }
      piece.used = true;
      put_state(data, piece.state);
      // The piece's health comes with the outfit (quality and health to 1/65535, weapons
      // included); the wear byte here stays the scripts' Player.grime wear.
      ++matched;
      break;
    }
  }
  if (log_line) {
    log_line("APPEARANCE grime player=%u body dirt=%u pieces=%zu matched=%zu", player,
             unsigned(body[0]), pieces.size(), matched);
  }
}

bool set_dirt(void* actor, float dirt) {
  if (!installed || !actor) {
    return false;
  }
  const float clamped = dirt < 0 ? 0 : (dirt > 1 ? 1 : dirt);
  const auto value = std::uint8_t(std::lround(clamped * 255.0f));
  auto change = [&](void* data) {
    State state = state_of(data);
    state[0] = value;
    put_state(data, state);
  };
  void* body = body_data(actor);
  if (body) {
    change(body);
  }
  for (void* item : outfit::worn(actor)) {
    if (void* data = item_data(item)) {
      change(data);
    }
  }
  return body != nullptr;
}

void order(void* actor, const kcd2s::integration::GrimeOrder& order) {
  if (!installed || !actor) {
    return;
  }
  const bool clean = order.kind == kcd2s::integration::GrimeOrder::Kind::Clean;
  auto change = [&](void* data) {
    State state = state_of(data);
    if (clean) {
      state.fill(0);
    } else {
      state[0] = std::uint8_t(std::min(255, state[0] + order.dirt));
      for (std::size_t zone = 1; zone < state.size(); ++zone) {
        state[zone] = std::uint8_t(std::min(255, state[zone] + order.blood));
      }
    }
    put_state(data, state);
  };
  std::size_t changed = 0;
  if (order.body) {
    if (void* data = body_data(actor)) {
      change(data);
      ++changed;
    }
  }
  if (order.clothes) {
    for (void* item : outfit::worn(actor)) {
      if (void* data = item_data(item)) {
        change(data);
        ++changed;
      }
    }
  }
  if (log_line) {
    log_line("APPEARANCE grime %s by the server: %zu states (dirt +%u, blood +%u)",
             clean ? "cleaned" : "soiled", changed, unsigned(order.dirt), unsigned(order.blood));
  }
}

void forget(std::uint32_t player) {
  applied.erase(player);
}

}  // namespace kcds::runtime::appearance::grime
