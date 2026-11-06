#include "resource_book.hpp"

#include "../props/held_props.hpp"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MinHook.h"
#include "../chat/chat_overlay.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::book {
namespace {

namespace sdk = kcd2s::sdk;
namespace props = kcds::runtime::props;

constexpr std::size_t kServicesItems = 224;
constexpr std::size_t kItemsInventories = 232;
constexpr std::size_t kItemHandle = 48;
constexpr std::size_t kItemInventory = 144;
constexpr std::size_t kReaderItem = 168;
// An inventory reading goes back to the inventory: the reading's exit (C_Reading slot 46)
// runs its slot 6, which ends the reading (the teardown hook below already ran when
// the request comes), and then asks the UI (services+432, vtable slot 1, request 2)
// to open the inventory on the read item. A resource's book was never read from the inventory,
// so that request is skipped for it (stopping the screen's fade instead kept the HUD hidden,
// and closing the screen still opened it, live). Everything else is the game's own inventory
// reading: the local player's hands, body animation, camera and exit are the game's alone (body
// clips started on top of its reading pose left the arms off the book, live); other players only
// learn the held document from here.
// The reading activity of the player module's activity table (asks for it).
constexpr int kReadingActivity = 2;
// C_Reading slot 46: the player's own exit (the minigame_exit
// hold's callback), which lowers the book with the game's animation before the teardown. Slot 21
// tears down at once and leaves the arms raised.
constexpr std::size_t kReaderExitSlot = 46;
// Inventory change reasons: 4 is the Lua CreateItem's, 6 a removal by script.
constexpr unsigned kAddReason = 4;
constexpr unsigned kRemoveReason = 6;
// A book whose reading has not shown a page by then never reached the hands.
constexpr ULONGLONG kOpenTimeoutMs = 10000;
constexpr std::uint32_t kTextAsWritten = 0x400;

// Vanilla documents without rewards that carry a resource's book, by style (Tables.pak,
// Libs/Tables/item/item.xml): book_red, book_01 and the folded parchment.
struct Carrier {
  const char* name;
  std::uint32_t data1;
  std::uint16_t data2;
  std::uint16_t data3;
  std::uint8_t data4[8];
};
constexpr Carrier kCarriers[] = {
    // lorebook_onCumans, 15756be8-18d6-41e0-9338-c5039d1d0548
    {"book", 0x15756be8, 0x18d6, 0x41e0, {0x93, 0x38, 0xc5, 0x03, 0x9d, 0x1d, 0x05, 0x48}},
    // lorebook_monasticRuleOfSaintBenedict2, 306e8746-088f-456a-9454-f43df58bb618
    {"plainBook", 0x306e8746, 0x088f, 0x456a, {0x94, 0x54, 0xf4, 0x3d, 0xf5, 0x8b, 0xb6, 0x18}},
    // zikmunduv_dopis, 08bf183d-9090-4b59-bd37-65ccd23e9485
    {"letter", 0x08bf183d, 0x9090, 0x4b59, {0xbd, 0x37, 0x65, 0xcc, 0xd2, 0x3e, 0x94, 0x85}},
};

// The descriptor copies it with an aligned 16-byte load.
struct alignas(16) ClassGuid {
  std::uint32_t data1;
  std::uint16_t data2;
  std::uint16_t data3;
  std::uint8_t data4[8];
};
static_assert(sizeof(ClassGuid) == 16);

// The GeneralBook listener's pages: std::vector<CryString> Texts and std::vector<Image> Images.
struct BookImage {
  std::int64_t page;
  void* name;  // CryString
};
struct BookTexts {
  void** texts_begin;
  void** texts_end;
  void** texts_capacity;
  BookImage* images_begin;
  BookImage* images_end;
  BookImage* images_capacity;
};

using Listener = void(__fastcall*)(void* self, BookTexts* texts, float dyslexia, int type,
                                   int visual);
using Teardown = void(__fastcall*)(void* reader);
using InventoryFind = void*(__fastcall*)(void* inventories, void* entity);
using ItemDescInit = void*(__fastcall*)(void* desc, const ClassGuid* guid, int amount,
                                        float health);
using ItemDescFree = void(__fastcall*)(void* desc);
using InventoryAdd = void*(__fastcall*)(void* inventory, void* desc, unsigned reason, char flag);
using InventoryRemove = void*(__fastcall*)(void* inventory, void* item, unsigned reason,
                                           unsigned amount);
using ReadItem = bool(__fastcall*)(void* human, void* item, void* error);
using StringCall = void*(__fastcall*)(void* string);
using Activity = void*(__fastcall*)(void* human, int type, char create, char check);
using CanRead = bool(__fastcall*)(void* human);
using ReaderCall = void(__fastcall*)(void* reader);
using InventoryReturn = void(__fastcall*)(void* reader);
using DocumentRead = void(__fastcall*)(void* documents, void* soul, void* item);

Logger log_line = nullptr;
Natives natives;
Listener original_listener = nullptr;
Teardown original_teardown = nullptr;
DocumentRead original_document_read = nullptr;
InventoryReturn original_inventory_return = nullptr;
// The item handle of the resource's book the last teardown ended; the exit asks for the inventory
// after its teardown.
std::atomic<std::uint64_t> ended_handle{0};
// The prop of a vanilla document the player reads (an inventory reading, not Book.open), so other
// players see it in the hands too; 0 when there is none (props::HeldProp: book 1, letter 3).
std::atomic<std::int64_t> vanilla_prop{0};
// Other players keep the document in the replica's hands while their put-away clip plays.
constexpr ULONGLONG kPutAwayMs = 1500;
std::atomic<std::int64_t> lingering_prop{0};
// The held prop's state for other players (props::HeldProp): the closing flag while the book is
// put away, and a count of page turns, so the replica's book closes and turns its pages.
constexpr std::int64_t kPropClosing = 16;
// A document taken out and put away with animations (a resource's book, not a letter); other
// players animate only those (props::HeldProp).
constexpr std::int64_t kPropAnimated = 32;
constexpr int kPropTurnShift = 8;
// The last page turn went back (list_prev on other players).
constexpr std::int64_t kPropTurnBack = 64;
std::atomic<std::int64_t> page_turns{0};
std::atomic<bool> last_turn_back{false};
// Which page turns the reading allows now: its page-turn actions are enabled only towards a page
// that exists. Pressing the disabled one turned no page here but the replica's
// book turned one.
using PageActions = std::intptr_t(__fastcall*)(void* reading, bool previous, bool next);
PageActions original_page_actions = nullptr;
std::atomic<bool> previous_page_allowed{true};
std::atomic<bool> next_page_allowed{true};

std::intptr_t __fastcall hooked_page_actions(void* reading, bool previous, bool next) {
  previous_page_allowed.store(previous);
  next_page_allowed.store(next);
  return original_page_actions(reading, previous, next);
}
std::atomic<ULONGLONG> lingering_until{0};
constexpr std::int64_t kBookProp = 1;
constexpr std::int64_t kLetterProp = 3;

// The game's document types (item table Document Type, the listener's type argument): lore
// books 1 and skill books 3 are bound books; recipes 2, maps 4 and letters 5 or 6 are a
// folded parchment.
std::int64_t document_prop(int type) {
  if (type == 1 || type == 3) {
    return kBookProp;
  }
  return kLetterProp;
}

struct Request {
  std::int64_t id = 0;
  int style = 0;
  int type = 1;
  int visual = 1;
  float legibility = 1;
  std::vector<std::string> pages;
  std::vector<std::pair<std::int64_t, std::string>> images;
};

struct Event {
  std::int64_t id = 0;
  std::string name;
  std::string reason;
};

std::mutex state_mutex;
bool open_pending = false;
Request pending;
bool close_pending = false;
bool legibility_pending = false;
// Book.setPages: new pages for the open book.
bool pages_pending = false;
std::vector<std::string> pending_pages;
// Book.write: start (page >= 0) or stop writing.
bool write_pending = false;
bool write_enabled = false;
std::int64_t write_page = -1;
float legibility_value = 1;
std::vector<Event> events;
bool in_world_now = false;
bool can_read_now = false;

// Engine thread only.
struct Active {
  bool present = false;
  Request request;
  void* inventory = nullptr;
  void* item = nullptr;
  std::uint64_t handle = 0;
  bool opened = false;
  bool closing_by_script = false;
  // The take-out plays before the game's reading starts at reading_at (0: it has started).
  ULONGLONG reading_at = 0;
  ULONGLONG started = 0;
  std::vector<void*> texts;  // CryString slots the listener was given
  std::vector<BookImage> images;
  void* listener = nullptr;
  // Writing (Book.write): the page written on and its text before the typed lines.
  bool writing = false;
  std::size_t writing_page = 0;
  std::string writing_base;
  int listener_type = 1;
  int listener_visual = 1;
};
Active active;

// When the local reader's own put-away document goes (0: none). Engine thread only.
ULONGLONG put_away_until = 0;
// The game's teardown snaps the body to its idle over the next frames (a put-away started with it
// did not play, live), so the put-away starts this much later; 0: none due.
constexpr ULONGLONG kPutAwayDelayMs = 120;
ULONGLONG put_away_at = 0;
// From the teardown of a resource's book to its put-away (holding_reading_pose).
std::atomic<bool> holding_pose{false};
std::int64_t put_away_prop = 0;
// The game's inventory reading shows the document in the hands at once and drops it at once; a
// resource's book plays the reading's own take-out before it (kcd_book_database.adb
// ReadingBookIn, reading_stand_inv_in) and its put-away after it (ReadingBookOut,
// reading_stand_inv_out), with a document of its own (props::local_take_out). Letters keep the
// game's (the book clips do not fit a parchment).
constexpr ULONGLONG kTakeOutMs = 1300;
constexpr int kLetterStyle = 2;

const void* reported_session = nullptr;
bool reported = false;
bool reported_in_world = false;
bool reported_can_read = false;

void* field(void* object, std::size_t offset) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(object) + offset);
}

void push_event(std::int64_t id, const char* name, const char* reason) {
  std::lock_guard<std::mutex> lock(state_mutex);
  events.push_back(Event{id, name, reason});
}

void* new_string(const std::string& text) {
  void* slot = nullptr;
  reinterpret_cast<StringCall>(natives.string_init)(&slot);
  natives.string_assign(&slot, text.c_str());
  return slot;
}

void free_strings() {
  for (void*& slot : active.texts) {
    reinterpret_cast<StringCall>(natives.string_free)(&slot);
  }
  for (BookImage& image : active.images) {
    reinterpret_cast<StringCall>(natives.string_free)(&image.name);
  }
  active.texts.clear();
  active.images.clear();
}

// The pages as the movie's Texts: a page break after every page but the last.
void build_pages() {
  free_strings();
  const std::vector<std::string>& pages = active.request.pages;
  for (std::size_t index = 0; index < pages.size(); ++index) {
    std::string text = pages[index];
    if (index + 1 < pages.size()) {
      text += "<newpage/>";
    }
    active.texts.push_back(new_string(text));
  }
  for (const auto& [page, image] : active.request.images) {
    active.images.push_back(BookImage{page, new_string(image)});
  }
}

void show_pages(void* listener) {
  BookTexts texts{};
  texts.texts_begin = active.texts.data();
  texts.texts_end = active.texts.data() + active.texts.size();
  texts.texts_capacity = texts.texts_end;
  texts.images_begin = active.images.data();
  texts.images_end = active.images.data() + active.images.size();
  texts.images_capacity = texts.images_end;
  const float legibility = std::clamp(active.request.legibility, 0.0f, 1.0f);
  // The listener turns every word of a text into a localization key unless the UI is told to
  // show texts as written; that holds only for this call.
  const std::uint32_t flags = *natives.ui_text_flags;
  *natives.ui_text_flags = flags | kTextAsWritten;
  original_listener(listener, &texts, legibility, active.request.type, active.request.visual);
  *natives.ui_text_flags = flags;
}

void finish(const char* reason) {
  if (!active.present) {
    return;
  }
  if (active.writing) {
    kcd2s::native_chat::end_capture();
  }
  push_event(active.request.id, "closed", reason);
  free_strings();
  active = Active{};
}

// Takes the carrier item out of the inventory again, if it is still there.
void remove_carrier() {
  if (!active.item || !active.inventory) {
    return;
  }
  if (field(active.item, kItemInventory) == active.inventory) {
    reinterpret_cast<InventoryRemove>(natives.inventory_remove)(active.inventory, active.item,
                                                                kRemoveReason, 0);
  }
  active.item = nullptr;
}

void __fastcall hooked_listener(void* self, BookTexts* texts, float dyslexia, int type,
                                int visual) {
  if (!active.present) {
    vanilla_prop.store(document_prop(type));
    original_listener(self, texts, dyslexia, type, visual);
    return;
  }
  active.listener = self;
  show_pages(self);
  if (!active.opened) {
    active.opened = true;
    push_event(active.request.id, "opened", "");
  }
}

void __fastcall hooked_document_read(void* documents, void* soul, void* item) {
  if (active.present && item == active.item) {
    return;
  }
  original_document_read(documents, soul, item);
}

// The held prop of the resource's book: the style's model (book 1, plainBook 2, letter 3,
// props::HeldProp), animated unless it is a letter.
std::int64_t resource_prop() {
  const int style = std::clamp(active.request.style, 0, 2);
  return (style + 1) | (style == kLetterStyle ? 0 : kPropAnimated);
}

bool reads_ours(void* reader) {
  return active.present && *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(reader) +
                                                             kReaderItem) == active.handle;
}

void __fastcall hooked_inventory_return(void* reader) {
  const std::uint64_t item =
      *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(reader) + kReaderItem);
  if (reads_ours(reader) || (item != 0 && item == ended_handle.load())) {
    if (log_line) {
      log_line("BOOK no inventory after a resource's book");
    }
    return;
  }
  original_inventory_return(reader);
}

void __fastcall hooked_teardown(void* reader) {
  const bool ours = reads_ours(reader);
  if (ours) {
    ended_handle.store(active.handle);
  }
  const std::int64_t prop = ours ? resource_prop() : vanilla_prop.load();
  // The teardown starts the idle; other players keep the reading pose until the put-away.
  if (ours && active.request.style != kLetterStyle) {
    holding_pose.store(true);
  }
  original_teardown(reader);
  lingering_prop.store(prop);
  lingering_until.store(GetTickCount64() + kPutAwayMs);
  if (!ours) {
    vanilla_prop.store(0);
  }
  if (ours) {
    if (active.request.style != kLetterStyle) {
      put_away_prop = prop;
      put_away_at = GetTickCount64() + kPutAwayDelayMs;
    }
    remove_carrier();
    finish(active.closing_by_script ? "script" : "player");
  }
}

void* player_inventory(void* services, void* entity) {
  void* items = field(services, kServicesItems);
  void* inventories = items ? field(items, kItemsInventories) : nullptr;
  if (!inventories) {
    return nullptr;
  }
  return reinterpret_cast<InventoryFind>(natives.inventory_find)(inventories, entity);
}

void* create_carrier(void* inventory, int style) {
  const Carrier& carrier = kCarriers[std::clamp(style, 0, 2)];
  ClassGuid guid{carrier.data1, carrier.data2, carrier.data3, {}};
  std::memcpy(guid.data4, carrier.data4, sizeof(guid.data4));
  alignas(16) std::uint8_t desc[512] = {};
  reinterpret_cast<ItemDescInit>(natives.item_desc_init)(desc, &guid, 1, -1.0f);
  void* item =
      reinterpret_cast<InventoryAdd>(natives.inventory_add)(inventory, desc, kAddReason, 0);
  reinterpret_cast<ItemDescFree>(natives.item_desc_free)(desc);
  return item;
}

// A class id from its table text; false when the text is not one.
bool parse_class_id(const std::string& text, ClassGuid& guid) {
  unsigned values[11] = {};
  const int read = std::sscanf(text.c_str(), "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &values[0],
                               &values[1], &values[2], &values[3], &values[4], &values[5],
                               &values[6], &values[7], &values[8], &values[9], &values[10]);
  if (read != 11) {
    return false;
  }
  guid.data1 = values[0];
  guid.data2 = static_cast<std::uint16_t>(values[1]);
  guid.data3 = static_cast<std::uint16_t>(values[2]);
  for (int index = 0; index < 8; ++index) {
    guid.data4[index] = static_cast<std::uint8_t>(values[3 + index]);
  }
  return true;
}

}  // namespace

bool give_test_item(const std::string& class_id, float health) {
  ClassGuid guid{};
  void* services = natives.services ? natives.services() : nullptr;
  void* entity = natives.local_entity ? natives.local_entity() : nullptr;
  void* inventory = services && entity ? player_inventory(services, entity) : nullptr;
  if (!inventory || !parse_class_id(class_id, guid)) {
    return false;
  }
  alignas(16) std::uint8_t desc[512] = {};
  reinterpret_cast<ItemDescInit>(natives.item_desc_init)(desc, &guid, 1, health);
  void* item =
      reinterpret_cast<InventoryAdd>(natives.inventory_add)(inventory, desc, kAddReason, 0);
  reinterpret_cast<ItemDescFree>(natives.item_desc_free)(desc);
  return item != nullptr;
}

namespace {

// Starts the game's reading of the carrier; false (and the book closed) when it refused.
bool start_reading(void* human) {
  active.reading_at = 0;
  active.started = GetTickCount64();
  void* error = nullptr;
  reinterpret_cast<StringCall>(natives.string_init)(&error);
  const bool reading = reinterpret_cast<ReadItem>(natives.read_item)(human, active.item, &error);
  const char* reason = error ? static_cast<const char*>(error) : "";
  if (!reading) {
    log_line("BOOK %lld: the game refused reading (%s)", static_cast<long long>(active.request.id),
             reason);
  }
  reinterpret_cast<StringCall>(natives.string_free)(&error);
  if (!reading) {
    remove_carrier();
    finish("failed");
  }
  return reading;
}

void open_book(const Request& request, void* human, void* entity) {
  void* services = natives.services();
  void* inventory = services ? player_inventory(services, entity) : nullptr;
  if (!inventory) {
    push_event(request.id, "closed", "failed");
    log_line("BOOK %lld: no inventory", static_cast<long long>(request.id));
    return;
  }
  void* item = create_carrier(inventory, request.style);
  if (!item) {
    push_event(request.id, "closed", "failed");
    log_line("BOOK %lld: carrier item not created", static_cast<long long>(request.id));
    return;
  }
  active = Active{};
  active.present = true;
  active.request = request;
  active.inventory = inventory;
  active.item = item;
  active.handle = *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(item) + kItemHandle);
  active.started = GetTickCount64();
  build_pages();
  if (request.style == kLetterStyle) {
    start_reading(human);
    return;
  }
  put_away_until = 0;
  put_away_at = 0;
  holding_pose.store(false);
  props::local_take_out(std::clamp(request.style, 0, 2) + 1);
  active.reading_at = GetTickCount64() + kTakeOutMs;
}

// The player's reading while it runs; null when there is none.
void* reading_activity(void* human) {
  return reinterpret_cast<Activity>(natives.activity)(human, kReadingActivity, 0, 1);
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  log_line = logger;
  natives = given;
  const void* required[] = {
      natives.book_listener,    natives.inventory_find,   natives.item_desc_init,
      natives.item_desc_free,   natives.inventory_add,    natives.inventory_remove,
      natives.read_item,        natives.string_init,      natives.string_free,
      natives.reading_teardown, natives.activity,         natives.can_read,
      natives.document_read,    natives.inventory_return, natives.page_actions};
  for (const void* native : required) {
    if (!native) {
      return false;
    }
  }
  if (!natives.services || !natives.local_human || !natives.local_entity ||
      !natives.string_assign || !natives.ui_text_flags) {
    return false;
  }
  return MH_CreateHook(natives.book_listener, reinterpret_cast<void*>(hooked_listener),
                       reinterpret_cast<void**>(&original_listener)) == MH_OK &&
         MH_EnableHook(natives.book_listener) == MH_OK &&
         MH_CreateHook(natives.reading_teardown, reinterpret_cast<void*>(hooked_teardown),
                       reinterpret_cast<void**>(&original_teardown)) == MH_OK &&
         MH_EnableHook(natives.reading_teardown) == MH_OK &&
         MH_CreateHook(natives.document_read, reinterpret_cast<void*>(hooked_document_read),
                       reinterpret_cast<void**>(&original_document_read)) == MH_OK &&
         MH_EnableHook(natives.document_read) == MH_OK &&
         MH_CreateHook(natives.inventory_return, reinterpret_cast<void*>(hooked_inventory_return),
                       reinterpret_cast<void**>(&original_inventory_return)) == MH_OK &&
         MH_EnableHook(natives.inventory_return) == MH_OK &&
         MH_CreateHook(natives.page_actions, reinterpret_cast<void*>(hooked_page_actions),
                       reinterpret_cast<void**>(&original_page_actions)) == MH_OK &&
         MH_EnableHook(natives.page_actions) == MH_OK;
}

// Typed text as page markup: one paragraph per line, the markup characters escaped.
std::string written_markup(const std::string& text) {
  std::string out;
  std::string line;
  auto flush = [&]() {
    out += "<paragraph>" + line + "</paragraph>";
    line.clear();
  };
  for (char character : text) {
    if (character == '\n') {
      flush();
    } else if (character == '<') {
      line += "&lt;";
    } else if (character == '>') {
      line += "&gt;";
    } else if (character == '&') {
      line += "&amp;";
    } else {
      line += character;
    }
  }
  flush();
  return out;
}

void relayout_pages() {
  build_pages();
  if (active.listener) {
    show_pages(active.listener);
  }
}

void stop_writing() {
  if (active.writing) {
    kcd2s::native_chat::end_capture();
    active.writing = false;
  }
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "bookPages") {
    const sdk::Value* pages = kcd2s::integration::script_field(message, "pages");
    if (pages && pages->tag() == sdk::Tag::Array && !pages->as_array().empty()) {
      std::vector<std::string> texts;
      for (const sdk::Value& page : pages->as_array()) {
        texts.push_back(page.tag() == sdk::Tag::Str ? page.as_str() : std::string());
      }
      std::lock_guard<std::mutex> lock(state_mutex);
      pending_pages = std::move(texts);
      pages_pending = true;
    }
    return true;
  }
  if (operation == "bookWrite") {
    const sdk::Value* enabled = kcd2s::integration::script_field(message, "enabled");
    const sdk::Value* page = kcd2s::integration::script_field(message, "page");
    std::lock_guard<std::mutex> lock(state_mutex);
    write_enabled = enabled && enabled->tag() == sdk::Tag::Bool && enabled->as_bool();
    write_page = page && page->tag() == sdk::Tag::Int ? page->as_int() : -1;
    write_pending = true;
    return true;
  }
  if (operation == "bookClose") {
    std::lock_guard<std::mutex> lock(state_mutex);
    close_pending = true;
    return true;
  }
  if (operation == "bookLegibility") {
    const sdk::Value* value = kcd2s::integration::script_field(message, "value");
    if (value && (value->tag() == sdk::Tag::Double || value->tag() == sdk::Tag::Int)) {
      std::lock_guard<std::mutex> lock(state_mutex);
      legibility_value = value->tag() == sdk::Tag::Double ? static_cast<float>(value->as_double())
                                                          : static_cast<float>(value->as_int());
      legibility_pending = true;
    }
    return true;
  }
  if (operation != "bookOpen") {
    return false;
  }
  Request request;
  auto integer = [&](const char* name, std::int64_t fallback) {
    const sdk::Value* value = kcd2s::integration::script_field(message, name);
    return value && value->tag() == sdk::Tag::Int ? value->as_int() : fallback;
  };
  request.id = integer("id", 0);
  request.style = static_cast<int>(integer("style", 0));
  request.type = static_cast<int>(integer("type", 1));
  request.visual = static_cast<int>(integer("visual", 1));
  const sdk::Value* legibility = kcd2s::integration::script_field(message, "legibility");
  if (legibility && legibility->tag() == sdk::Tag::Double) {
    request.legibility = static_cast<float>(legibility->as_double());
  } else if (legibility && legibility->tag() == sdk::Tag::Int) {
    request.legibility = static_cast<float>(legibility->as_int());
  }
  const sdk::Value* pages = kcd2s::integration::script_field(message, "pages");
  if (pages && pages->tag() == sdk::Tag::Array) {
    for (const sdk::Value& page : pages->as_array()) {
      if (page.tag() == sdk::Tag::Str) {
        request.pages.push_back(page.as_str());
      }
    }
  }
  const sdk::Value* images = kcd2s::integration::script_field(message, "images");
  if (images && images->tag() == sdk::Tag::Array) {
    for (const sdk::Value& image : images->as_array()) {
      const sdk::Value* page = kcd2s::integration::script_field(image, "page");
      const sdk::Value* name = kcd2s::integration::script_field(image, "image");
      if (page && page->tag() == sdk::Tag::Int && name && name->tag() == sdk::Tag::Str) {
        request.images.emplace_back(page->as_int(), name->as_str());
      }
    }
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  pending = std::move(request);
  open_pending = true;
  return true;
}

void frame(bool in_world) {
  void* human = in_world ? natives.local_human() : nullptr;
  void* entity = in_world ? natives.local_entity() : nullptr;
  bool open = false;
  bool close = false;
  bool relayout = false;
  bool new_pages = false;
  std::vector<std::string> pages;
  bool write = false;
  bool write_on = false;
  std::int64_t write_at = -1;
  Request request;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    new_pages = pages_pending;
    pages_pending = false;
    if (new_pages) {
      pages = std::move(pending_pages);
    }
    write = write_pending;
    write_pending = false;
    write_on = write_enabled;
    write_at = write_page;
    open = open_pending;
    close = close_pending;
    relayout = legibility_pending;
    open_pending = false;
    close_pending = false;
    legibility_pending = false;
    if (open) {
      request = std::move(pending);
    }
    if (relayout) {
      active.request.legibility = legibility_value;
    }
  }
  if (!human || !entity) {
    // The world went away with the hands and the carrier.
    finish("sessionOver");
    if (open) {
      push_event(request.id, "closed", "failed");
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    in_world_now = false;
    can_read_now = false;
    return;
  }
  if (!active.present && vanilla_prop.load() && !reading_activity(human)) {
    // A vanilla reading ended without the teardown hook (the world changed, for example).
    vanilla_prop.store(0);
  }
  if (close && active.present && active.reading_at != 0) {
    // Closed during the take-out: the game's reading never started.
    props::local_remove();
    remove_carrier();
    finish("script");
  }
  if (active.present && active.reading_at != 0 && GetTickCount64() >= active.reading_at) {
    props::local_remove();
    start_reading(human);
  }
  if (put_away_at != 0 && GetTickCount64() >= put_away_at) {
    put_away_at = 0;
    holding_pose.store(false);
    props::local_put_away(put_away_prop);
    put_away_until = GetTickCount64() + kPutAwayMs;
  }
  if (put_away_until != 0 && GetTickCount64() >= put_away_until) {
    put_away_until = 0;
    props::local_remove();
  }
  if (close && active.present) {
    if (void* reader = reading_activity(human)) {
      active.closing_by_script = true;
      (*reinterpret_cast<ReaderCall**>(reader))[kReaderExitSlot](reader);
    }
  }
  if (relayout && active.present && active.opened && active.listener) {
    show_pages(active.listener);
  }
  if (new_pages && active.present && active.opened) {
    stop_writing();
    active.request.pages = std::move(pages);
    relayout_pages();
  }
  if (write && active.present && active.opened) {
    stop_writing();
    if (write_on) {
      const std::size_t count = active.request.pages.size();
      const std::size_t page = write_at >= 0 && static_cast<std::size_t>(write_at) < count
                                   ? static_cast<std::size_t>(write_at)
                                   : count - 1;
      if (kcd2s::native_chat::begin_capture("")) {
        active.writing = true;
        active.writing_page = page;
        active.writing_base = active.request.pages[page];
      } else {
        push_event(active.request.id, "writingEnded", "");
      }
    }
  }
  if (active.writing) {
    const kcd2s::native_chat::CaptureUpdate update = kcd2s::native_chat::take_capture();
    if (update.changed) {
      active.request.pages[active.writing_page] = active.writing_base + written_markup(update.text);
      relayout_pages();
      push_event(active.request.id, "edited", update.text.c_str());
    }
    if (update.ended) {
      active.writing = false;
      push_event(active.request.id, "writingEnded", "");
    }
  }
  if (open) {
    if (active.present) {
      push_event(request.id, "closed", "failed");
    } else {
      open_book(request, human, entity);
    }
  }
  if (active.present && !active.opened && GetTickCount64() - active.started > kOpenTimeoutMs &&
      !reading_activity(human)) {
    remove_carrier();
    finish("failed");
  }
  const bool can_read = reinterpret_cast<CanRead>(natives.can_read)(human);
  std::lock_guard<std::mutex> lock(state_mutex);
  in_world_now = true;
  can_read_now = can_read;
}

// A page turn reaches other players as a count in the held prop's state; their replica's
// document plays its page-turn clip. Engine thread.
constexpr ULONGLONG kPageTurnGapMs = 600;

void page_turned(bool forward) {
  static ULONGLONG last = 0;
  const bool reading = (active.present && active.opened) || vanilla_prop.load() != 0;
  const bool allowed = forward ? next_page_allowed.load() : previous_page_allowed.load();
  const ULONGLONG now = GetTickCount64();
  if (!reading || !allowed || now - last < kPageTurnGapMs) {
    return;
  }
  last = now;
  last_turn_back.store(!forward);
  page_turns.store((page_turns.load() + 1) & 0xFF);
}

bool holding_reading_pose() {
  return holding_pose.load();
}

std::int64_t held_prop() {
  const std::int64_t turns =
      (page_turns.load() << kPropTurnShift) | (last_turn_back.load() ? kPropTurnBack : 0);
  if (!active.present) {
    if (const std::int64_t vanilla = vanilla_prop.load()) {
      return vanilla | turns;
    }
    if (GetTickCount64() < lingering_until.load() && lingering_prop.load()) {
      return lingering_prop.load() | kPropClosing | turns;
    }
    return 0;
  }
  return resource_prop() | turns;
}

void pump(const void* session, const RuntimeSender& send) {
  bool in_world = false;
  bool can_read = false;
  std::vector<Event> taken;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    in_world = in_world_now;
    can_read = can_read_now;
    taken.swap(events);
  }
  if (session != reported_session) {
    reported_session = session;
    reported = false;
  }
  if (!reported || in_world != reported_in_world || can_read != reported_can_read) {
    sdk::Value::Map state{{"op", sdk::Value(std::string("bookHost"))},
                          {"inWorld", sdk::Value(in_world)},
                          {"canRead", sdk::Value(can_read)}};
    if (send(sdk::Value(std::move(state)))) {
      reported = true;
      reported_in_world = in_world;
      reported_can_read = can_read;
    }
  }
  for (const Event& event : taken) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("bookEvent"))},
                            {"id", sdk::Value(event.id)},
                            {"name", sdk::Value(event.name)},
                            {"reason", sdk::Value(event.reason)}};
    send(sdk::Value(std::move(message)));
  }
}

}  // namespace kcds::runtime::book
