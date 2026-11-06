#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

// What a player holds in its hands: the item classes in its right and left hand,
// such as a drawn sword and a shield. The owner publishes them in the NetworkPlayer field "hands"
// (hands_field) and every other client puts the replica's items of those classes in its hands
// (Client/runtime/weapons/drawn_weapons.hpp).
//
// The field: the right hand's class id, '|', the left hand's; a class id is its 16 bytes in 32
// upper-case hex digits, an empty hand nothing. "" when both hands are empty.
// H2 adds owner-generated item lifetime tokens: H2:<class>:<8 hex token>|<class>:<8 hex token>.
// Tokens are scoped to the owner, never native item IDs; zero is an unknown identity.
namespace kcd2s::integration {

using ItemClassId = std::array<std::uint8_t, 16>;

struct Hands {
  std::optional<ItemClassId> right;
  std::optional<ItemClassId> left;
  std::uint32_t right_token = 0;
  std::uint32_t left_token = 0;

  friend bool operator==(const Hands& a, const Hands& b) {
    return a.right == b.right && a.left == b.left && a.right_token == b.right_token &&
           a.left_token == b.left_token;
  }
};

inline std::string item_class_text(const ItemClassId& id) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string text;
  for (std::uint8_t byte : id) {
    text += kDigits[byte >> 4];
    text += kDigits[byte & 15];
  }
  return text;
}

// A class as scripts see it (Player.equipment, rightHandItem, leftHandItem, GroundItem.itemClass):
// its 16 bytes read as two little-endian 64-bit numbers, each written as 16 lower-case hex digits
// (a dashed GUID's first three fields reversed as one number, then its last
// eight bytes reversed).
inline std::string item_class_script_text(const ItemClassId& id) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (std::size_t half = 0; half < 2; ++half) {
    for (std::size_t i = 8; i-- > 0;) {
      const std::uint8_t byte = id[half * 8 + i];
      text += kDigits[byte >> 4];
      text += kDigits[byte & 15];
    }
  }
  return text;
}

// The class of item_class_script_text's form (either case); false for anything else.
inline bool parse_item_class_script_text(const std::string& text, ItemClassId& id) {
  if (text.size() != 32) {
    return false;
  }
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  ItemClassId parsed{};
  for (std::size_t half = 0; half < 2; ++half) {
    for (std::size_t i = 0; i < 8; ++i) {
      const std::size_t at = half * 16 + i * 2;
      const int high = digit(text[at]);
      const int low = digit(text[at + 1]);
      if (high < 0 || low < 0) {
        return false;
      }
      parsed[half * 8 + 7 - i] = std::uint8_t(high * 16 + low);
    }
  }
  id = parsed;
  return true;
}

inline std::string hands_field(const Hands& hands) {
  if (!hands.right && !hands.left) {
    return {};
  }
  std::string field;
  const bool tokens = hands.right_token || hands.left_token;
  if (tokens) {
    if ((!hands.right && hands.right_token) || (!hands.left && hands.left_token) ||
        (hands.right_token && hands.right_token == hands.left_token && hands.right != hands.left)) {
      return {};
    }
    field = "H2:";
  }
  const auto append_token = [&](std::uint32_t token) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    field += ':';
    for (int shift = 28; shift >= 0; shift -= 4) {
      field += kDigits[(token >> shift) & 15];
    }
  };
  if (hands.right) {
    field += item_class_text(*hands.right);
    if (tokens) {
      append_token(hands.right_token);
    }
  }
  field += '|';
  if (hands.left) {
    field += item_class_text(*hands.left);
    if (tokens) {
      append_token(hands.left_token);
    }
  }
  return field;
}

namespace detail {

inline int hex_digit(char c) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  for (int digit = 0; digit < 16; ++digit) {
    if (kDigits[digit] == c) {
      return digit;
    }
  }
  return -1;
}

// One hand of the field: nothing, or 32 hex digits.
inline bool parse_hand(const std::string& text, std::optional<ItemClassId>& hand) {
  if (text.empty()) {
    hand.reset();
    return true;
  }
  if (text.size() != 32) {
    return false;
  }
  ItemClassId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    const int high = hex_digit(text[2 * i]);
    const int low = hex_digit(text[2 * i + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    id[i] = std::uint8_t(high * 16 + low);
  }
  hand = id;
  return true;
}

inline bool parse_token_hand(const std::string& text, std::optional<ItemClassId>& hand,
                             std::uint32_t& token) {
  token = 0;
  if (text.empty()) {
    hand.reset();
    return true;
  }
  if (text.size() != 41 || text[32] != ':' || !parse_hand(text.substr(0, 32), hand)) {
    return false;
  }
  for (std::size_t index = 33; index < text.size(); ++index) {
    const int digit = hex_digit(text[index]);
    if (digit < 0) {
      return false;
    }
    token = (token << 4) | std::uint32_t(digit);
  }
  return true;
}

}  // namespace detail

// The hands of a field; false when it is not one.
inline bool parse_hands_field(const std::string& field, Hands& hands) {
  hands = {};
  if (field.empty()) {
    return true;
  }
  if (field.size() > 86) {
    return false;
  }
  const bool tokens = field.starts_with("H2:");
  const std::string body = tokens ? field.substr(3) : field;
  const std::size_t bar = body.find('|');
  if (bar == std::string::npos || body.find('|', bar + 1) != std::string::npos) {
    return false;
  }
  Hands parsed;
  if (tokens) {
    if (!detail::parse_token_hand(body.substr(0, bar), parsed.right, parsed.right_token) ||
        !detail::parse_token_hand(body.substr(bar + 1), parsed.left, parsed.left_token) ||
        (!parsed.right_token && !parsed.left_token) ||
        (parsed.right_token && parsed.right_token == parsed.left_token &&
         parsed.right != parsed.left)) {
      return false;
    }
  } else if (!detail::parse_hand(body.substr(0, bar), parsed.right) ||
             !detail::parse_hand(body.substr(bar + 1), parsed.left)) {
    return false;
  }
  // Two empty hands are written as "".
  if (!parsed.right && !parsed.left) {
    return false;
  }
  hands = parsed;
  return true;
}

// Local-only observations used to fence native item lifetime. These values are never encoded.
// The instance distinguishes a replacement at the same native ID; the class must also match.
struct LocalHandItem {
  std::uint64_t uid = 0;
  std::uintptr_t instance = 0;
  ItemClassId item_class{};

  friend bool operator==(const LocalHandItem&, const LocalHandItem&) = default;
};

using LocalHandItems = std::array<std::optional<LocalHandItem>, 2>;

// A bounded owner cache. Old, non-current observations are evicted in LRU order. Tokens are
// never reused, including after clear(); exhausting uint32 fails closed for new identities.
class ItemLifetimeTokens {
 public:
  static constexpr std::size_t kCapacity = 32;

  explicit ItemLifetimeTokens(std::uint64_t next_token = 1) : next_token_(next_token) {
  }

  std::optional<std::uint32_t> token_for(const LocalHandItem& item, const LocalHandItems& current) {
    if (!item.uid || !item.instance) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < count_; ++index) {
      if (entries_[index].item.uid != item.uid) {
        continue;
      }
      const auto entry = entries_[index];
      erase(index);
      if (entry.item == item) {
        entries_[count_++] = entry;
        return entry.token;
      }
      break;
    }
    if (!next_token_ || next_token_ > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    if (count_ == entries_.size()) {
      std::size_t victim = 0;
      for (; victim < count_; ++victim) {
        bool in_hand = false;
        for (const auto& held : current) {
          in_hand = in_hand || (held && *held == entries_[victim].item);
        }
        if (!in_hand) {
          break;
        }
      }
      if (victim == count_) {
        return std::nullopt;
      }
      erase(victim);
    }
    const auto token = std::uint32_t(next_token_++);
    entries_[count_++] = Entry{item, token};
    return token;
  }

  template <class Lookup>
  void retain_valid(const Lookup& lookup) {
    for (std::size_t index = 0; index < count_;) {
      const auto live = lookup(entries_[index].item.uid);
      if (!live || *live != entries_[index].item) {
        erase(index);
      } else {
        ++index;
      }
    }
  }

  void clear() {
    entries_ = {};
    count_ = 0;
  }

  std::size_t size() const {
    return count_;
  }

 private:
  struct Entry {
    LocalHandItem item;
    std::uint32_t token = 0;
  };

  void erase(std::size_t index) {
    for (std::size_t next = index + 1; next < count_; ++next) {
      entries_[next - 1] = entries_[next];
    }
    entries_[--count_] = {};
  }

  std::array<Entry, kCapacity> entries_{};
  std::size_t count_ = 0;
  std::uint64_t next_token_;
};

// Only the latest owner field can bind current replica hands. Every lookup revalidates the
// full local observation, so a drop, new native instance or changed class invalidates it at once.
class HandItemBindings {
 public:
  void bind(const Hands& hands, const LocalHandItems& current) {
    clear();
    const std::array<std::optional<ItemClassId>, 2> classes{hands.right, hands.left};
    const std::array<std::uint32_t, 2> tokens{hands.right_token, hands.left_token};
    if (tokens[0] && tokens[0] == tokens[1] &&
        (classes[0] != classes[1] || current[0] != current[1])) {
      return;
    }
    for (std::size_t hand = 0; hand < current.size(); ++hand) {
      if (tokens[hand] && classes[hand] && current[hand] && current[hand]->uid &&
          current[hand]->instance && current[hand]->item_class == *classes[hand]) {
        entries_[hand] = Entry{*current[hand], tokens[hand]};
      }
    }
  }

  std::optional<std::uint32_t> token_for(std::uint64_t uid, const LocalHandItems& current) const {
    if (!uid) {
      return 0;
    }
    for (std::size_t hand = 0; hand < entries_.size(); ++hand) {
      const auto& entry = entries_[hand];
      if (entry.token && entry.item.uid == uid && current[hand] && *current[hand] == entry.item) {
        return entry.token;
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> item_from(std::uint32_t token, const LocalHandItems& current) const {
    if (!token) {
      return 0;
    }
    for (std::size_t hand = 0; hand < entries_.size(); ++hand) {
      const auto& entry = entries_[hand];
      if (entry.token == token && current[hand] && *current[hand] == entry.item) {
        return entry.item.uid;
      }
    }
    return std::nullopt;
  }

  void clear() {
    entries_ = {};
  }

 private:
  struct Entry {
    LocalHandItem item;
    std::uint32_t token = 0;
  };
  std::array<Entry, 2> entries_{};
};

}  // namespace kcd2s::integration
