// Book for client resources: the game side of
// Client/resources/book.cjs.
//
// A resource's book is shown through the game's own reading of an inventory document
// (WHGame 1.5.5). A vanilla document without rewards carries it: one item of that class is
// created in the local player's inventory (the inventory manager, *(services + 224) + 232,
// finds the entity's inventory), and the game's inventory "read"
// (the can-read checks, the hands, the camera, C_Reading) is started on it. When the
// reading screen gets its pages (the GeneralBook listener: Texts, Images and
// SetFullBook(dyslexia level, document class, visual category)), the hook gives it the
// resource's pages instead. C_Reading's teardown (vtable slot 5) ends it: the carrier
// item is removed again and the book is reported closed. The reading's start hands the item to
// the soul's document handler (scholarship XP on the first read of a class, the read
// marker); for the carrier it is skipped, so a resource's book grants nothing.
//
//   runtime -> game:  { op: "bookOpen", id, style, type, visual, legibility, pages: [string],
//                       images: [{ page, image }] }
//                     { op: "bookClose" }, { op: "bookLegibility", value }
//   game -> runtime:  { op: "bookHost", inWorld, canRead } on change
//                     { op: "bookEvent", id, name: "opened" | "closed", reason }
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::book {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void*, const char*);

struct Natives {
  void* (*services)() = nullptr;      // the game services getter
  void* (*local_human)() = nullptr;   // the local C_Player, the human the reading runs on
  void* (*local_entity)() = nullptr;  // its engine entity
  StringAssign string_assign = nullptr;
  void* book_listener = nullptr;
  void* inventory_find = nullptr;
  void* item_desc_init = nullptr;
  void* item_desc_free = nullptr;
  void* inventory_add = nullptr;
  void* inventory_remove = nullptr;
  void* read_item = nullptr;
  void* string_init = nullptr;
  void* string_free = nullptr;
  void* reading_teardown = nullptr;
  void* activity = nullptr;
  void* can_read = nullptr;
  void* inventory_return = nullptr;  // the reading's request for the inventory
  void* document_read = nullptr;     // the soul's first-read XP and read marker
  // the reading enables its page-turn actions (reading, previous page, next page).
  void* page_actions = nullptr;
  // The UI's text flags (dword_1855E2B14): with 0x400 set, passes a text through as
  // written instead of making localization keys (@word) of its words.
  std::uint32_t* ui_text_flags = nullptr;
};

bool install(const Natives& natives, Logger logger);

// For local::Client::set_local_runtime_handler: true when the message was Book's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Test hook (KCDS_TEST_HOOKS only), engine thread: adds one item of the class (item table id,
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") to the local player's inventory with the same natives
// as a book's carrier; false when it could not. The game's script CreateItem adds nothing in a
// multiplayer world. `health` is the item's absolute health; negative is the class's
// default.
bool give_test_item(const std::string& class_id, float health = -1.0f);

// Engine thread, every frame: opens and closes books, samples whether the player can read.
void frame(bool in_world);

// Engine thread: the held prop (Client/runtime/props) of the book in the local player's hands,
// 0 when there is none; other players see it on this player.
std::int64_t held_prop();

// Any thread: true from the end of the local reading of a resource's book until its put-away
// starts. The body clip the game starts at the end (its idle) is not sent to other players then:
// their replica went from the reading to the idle and only then into the put-away (0.5 s).
bool holding_reading_pose();

// The player pressed read_next (forward) or read_prev while reading; engine thread. Counted as a
// page turn only when the reading has a page that way.
void page_turned(bool forward);

// Network thread: reports state changes and events to the runtime of session.
void pump(const void* session, const RuntimeSender& send);

}  // namespace kcds::runtime::book
