#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"
#include "kcd2s/integration/inventory.hpp"

// Item types a resource registered (Items.register).
//   ITEM_TYPES: server -> every client, the types: the whole set for a client that came in, and
//               each new one for the rest, before any item of it is shown. Each borrows the look
//               of an existing item (its visual) and has its own class GUID, id, weight, price,
//               stacking, and localization keys whose texts ITEM_NAMES carries.
// On the control channel (0).

namespace kcd2s::integration {

inline constexpr std::uint8_t kItemTypesTag = 29;
inline constexpr std::size_t kMaxItemTypes = 4096;
inline constexpr std::size_t kMaxItemTypeIdBytes = 128;
inline constexpr std::size_t kMaxItemIconFileBytes = 512;

struct ItemType {
  Guid id{};
  Guid visual{};
  std::string name;             // its registration id
  std::string name_key;         // localization key of its name
  std::string description_key;  // localization key of its description
  std::string use_label_key;    // localization key of its use action's label
  // Its own icon: the icon id its class shows (the game loads
  // Libs/UI/Textures/Icons/Items/<icon>_icon.dds) and "<resource>/<path>" of the .dds a resource
  // ships to clients for it; both "" for the visual's icon.
  std::string icon;
  std::string icon_file;
  // Its own mesh: a path under Objects/ as the prop catalogue writes it
  // ("objects/manmade/common_furniture/bucket/bucket_a_a.cgf"); "" for the visual's.
  std::string model;
  float weight = 0;
  std::uint32_t price = 0;
  bool stackable = true;
  bool usable = false;

  friend bool operator==(const ItemType& a, const ItemType& b) {
    return a.id == b.id && a.visual == b.visual && a.name == b.name && a.name_key == b.name_key &&
           a.description_key == b.description_key && a.use_label_key == b.use_label_key &&
           a.icon == b.icon && a.icon_file == b.icon_file && a.model == b.model &&
           a.weight == b.weight && a.price == b.price && a.stackable == b.stackable &&
           a.usable == b.usable;
  }
};

struct ItemTypes {
  bool full = false;
  std::vector<ItemType> types;

  friend bool operator==(const ItemTypes& a, const ItemTypes& b) {
    return a.full == b.full && a.types == b.types;
  }
};

// ITEM_TYPES: [tag][u8 full][varuint count][types: 16 bytes GUID, 16 bytes visual GUID, the id,
// name key, description key, use label key, icon, icon file and model (varuint length, bytes), u32
// weight bits, varuint price, u8 flags (1 stackable, 2 usable)]. Empty for types that cannot
// travel.
inline std::vector<std::uint8_t> encode_item_types(const ItemTypes& types) {
  if (types.types.size() > kMaxItemTypes) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kItemTypesTag, 8);
  w.write_bits(types.full ? 1 : 0, 8);
  w.write_varuint(types.types.size());
  for (const ItemType& type : types.types) {
    if (type.name.empty() || type.name.size() > kMaxItemTypeIdBytes || type.name_key.empty() ||
        type.name_key.size() > kMaxItemNameBytes || type.description_key.empty() ||
        type.description_key.size() > kMaxItemNameBytes || type.use_label_key.empty() ||
        type.use_label_key.size() > kMaxItemNameBytes || type.icon.size() > kMaxItemNameBytes ||
        type.icon_file.size() > kMaxItemIconFileBytes ||
        type.model.size() > kMaxItemIconFileBytes || type.icon.empty() != type.icon_file.empty()) {
      return {};
    }
    w.write_bytes(type.id.data(), type.id.size());
    w.write_bytes(type.visual.data(), type.visual.size());
    inventory_detail::write_text(w, type.name);
    inventory_detail::write_text(w, type.name_key);
    inventory_detail::write_text(w, type.description_key);
    inventory_detail::write_text(w, type.use_label_key);
    inventory_detail::write_text(w, type.icon);
    inventory_detail::write_text(w, type.icon_file);
    inventory_detail::write_text(w, type.model);
    std::uint32_t weight_bits = 0;
    std::memcpy(&weight_bits, &type.weight, sizeof(weight_bits));
    w.write_bits(weight_bits, 32);
    w.write_varuint(type.price);
    w.write_bits((type.stackable ? 1 : 0) | (type.usable ? 2 : 0), 8);
  }
  return w.bytes();
}

inline bool decode_item_types(const std::vector<std::uint8_t>& p, ItemTypes& types) {
  if (p.size() < 3 || p[0] != kItemTypesTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  ItemTypes decoded;
  const std::uint64_t full = r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || full > 1 || count > kMaxItemTypes) {
    return false;
  }
  decoded.full = full == 1;
  decoded.types.resize(count);
  for (ItemType& type : decoded.types) {
    for (std::uint8_t& byte : type.id) {
      byte = std::uint8_t(r.read_bits(8));
    }
    for (std::uint8_t& byte : type.visual) {
      byte = std::uint8_t(r.read_bits(8));
    }
    if (!inventory_detail::read_text(r, type.name, false, kMaxItemTypeIdBytes) ||
        !inventory_detail::read_text(r, type.name_key, false) ||
        !inventory_detail::read_text(r, type.description_key, false) ||
        !inventory_detail::read_text(r, type.use_label_key, false) ||
        !inventory_detail::read_text(r, type.icon, true) ||
        !inventory_detail::read_text(r, type.icon_file, true, kMaxItemIconFileBytes) ||
        !inventory_detail::read_text(r, type.model, true, kMaxItemIconFileBytes) ||
        type.icon.empty() != type.icon_file.empty()) {
      return false;
    }
    const auto weight_bits = std::uint32_t(r.read_bits(32));
    std::memcpy(&type.weight, &weight_bits, sizeof(weight_bits));
    const std::uint64_t price = r.read_varuint();
    const std::uint64_t flags = r.read_bits(8);
    if (r.overrun() || price > 0xFFFFFFFFu || flags > 3 || !(type.weight >= 0.0f)) {
      return false;
    }
    type.price = std::uint32_t(price);
    type.stackable = (flags & 1) != 0;
    type.usable = (flags & 2) != 0;
  }
  if (!inventory_detail::read_whole(r)) {
    return false;
  }
  types = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
