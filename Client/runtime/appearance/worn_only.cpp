#include "worn_only.hpp"

#include <algorithm>
#include <string>

#include "../inventory/stock_projection.hpp"
#include "outfit.hpp"

namespace kcds::runtime::appearance::worn_only {
namespace {

using kcd2s::integration::ItemClassId;

struct Piece {
  ItemClassId item_class{};
  void* item = nullptr;
};

Logger log_line = nullptr;
std::uint64_t applied_revision = 0;
std::vector<Piece> pieces;

// Whether the local inventory still holds the item (the player may have dropped or sold it).
bool held(void* item) {
  void* inventory = inventory::local_player_inventory();
  if (!inventory || !item) {
    return false;
  }
  const std::vector<void*> items = inventory::items_in(inventory);
  return std::find(items.begin(), items.end(), item) != items.end();
}

void take_away(void* actor, const Piece& piece) {
  if (!held(piece.item)) {
    return;
  }
  outfit::wear(actor, piece.item, false);
  inventory::set_worn_only(piece.item, false);
  inventory::take_unit(piece.item);
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void keep(void* actor, void* entity, const std::vector<ItemClassId>& wanted,
          std::uint64_t revision) {
  if (!actor || !entity || revision == applied_revision) {
    return;
  }
  applied_revision = revision;
  // Pieces no longer wanted go.
  std::vector<Piece> kept;
  for (const Piece& piece : pieces) {
    if (std::find(wanted.begin(), wanted.end(), piece.item_class) == wanted.end()) {
      take_away(actor, piece);
    } else {
      kept.push_back(piece);
    }
  }
  pieces = std::move(kept);
  // New ones are made and put on once.
  for (const ItemClassId& item_class : wanted) {
    const bool have = std::any_of(pieces.begin(), pieces.end(), [&](const Piece& piece) {
      return piece.item_class == item_class && held(piece.item);
    });
    if (have) {
      continue;
    }
    void* item = outfit::create_item_in_inventory(entity, item_class);
    if (!item) {
      if (log_line) {
        log_line("WORN %s could not be made",
                 kcd2s::integration::item_class_text(item_class).substr(0, 8).c_str());
      }
      continue;
    }
    inventory::set_worn_only(item, true);
    const bool on = outfit::wear(actor, item, true);
    pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                                [&](const Piece& piece) { return piece.item_class == item_class; }),
                 pieces.end());
    pieces.push_back(Piece{item_class, item});
    if (log_line) {
      log_line("WORN %s made and %s",
               kcd2s::integration::item_class_text(item_class).substr(0, 8).c_str(),
               on ? "put on" : "not put on (the game refused)");
    }
  }
}

void reset(void* actor) {
  for (const Piece& piece : pieces) {
    if (actor) {
      take_away(actor, piece);
    }
  }
  pieces.clear();
  applied_revision = 0;
}

}  // namespace kcds::runtime::appearance::worn_only
