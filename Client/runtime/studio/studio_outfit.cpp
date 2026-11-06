#include "studio_outfit.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "../appearance/outfit.hpp"
#include "../core/log.hpp"
#include "kcd2s/integration/hands.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::studio::outfit {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The outfit code keeps its state per player: the studio's body is no player of a session.
constexpr std::uint32_t kStudioBody = 0xFFFF7777u;
// The quality and health the pieces are made with (an outfit field's per piece bytes).
constexpr std::uint8_t kQuality = 3;
constexpr std::uint16_t kHealth = 0xFFFF;
constexpr std::size_t kClassBytes = 16;
constexpr std::size_t kMostPieces = 32;

std::mutex outfit_mutex;
// The outfit field ("KO2", then per piece its class, quality and health); none until the editor
// gives one.
std::optional<std::string> wanted;

// A piece's 16 class bytes, as the game keeps them, from the catalogue's 32 hex digits (two
// little-endian 64-bit numbers, catalog.cjs classBytesHex); false when it is not that. Read in
// the digits' own order, the bytes were no class the game knows (the item registry
// found none, so nothing was worn).
bool class_bytes(const std::string& hex, std::string& out) {
  integration::ItemClassId id{};
  if (!integration::parse_item_class_script_text(hex, id)) {
    return false;
  }
  out.append(reinterpret_cast<const char*>(id.data()), kClassBytes);
  return true;
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioOutfit") {
    return false;
  }
  std::string field = "KO2";
  const sdk::Value* pieces = integration::script_field(message, "pieces");
  if (pieces && pieces->tag() == sdk::Tag::Array) {
    std::size_t count = 0;
    for (const sdk::Value& piece : pieces->as_array()) {
      if (piece.tag() != sdk::Tag::Str || count >= kMostPieces) {
        continue;
      }
      if (class_bytes(piece.as_str(), field)) {
        field.push_back(char(kQuality));
        field.push_back(char(kHealth & 0xFF));
        field.push_back(char(kHealth >> 8));
        ++count;
      }
    }
  }
  std::lock_guard<std::mutex> lock(outfit_mutex);
  wanted = field;
  return true;
}

void tick(void* local_actor, void* local_entity) {
  std::optional<std::string> field;
  {
    std::lock_guard<std::mutex> lock(outfit_mutex);
    field = wanted;
  }
  if (!field || !local_actor || !local_entity) {
    return;
  }
  // An outfit with no pieces ("KO2" alone) would leave the body as it is: apply takes an empty
  // outfit as not reported yet. Taking everything off is done piece by piece instead.
  if (field->size() == 3) {
    for (void* item : appearance::outfit::worn(local_actor)) {
      appearance::outfit::wear(local_actor, item, false);
    }
    return;
  }
  appearance::outfit::apply(kStudioBody, local_actor, local_entity, *field);
}

}  // namespace kcds::runtime::studio::outfit
