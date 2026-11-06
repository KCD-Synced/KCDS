#include "vendor_screen.hpp"

#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "MinHook.h"

#include "../inventory/stock_projection.hpp"
#include "../props/prop_entity.hpp"
#include "vendor_haggle.hpp"

namespace kcds::runtime::vendors {
namespace {

namespace integration = kcd2s::integration;
namespace sdk = kcd2s::sdk;

// The container whose inventory holds the shelf: the game's corpse container, which has no model,
// out of sight below the player.
constexpr char kShelfName[] = "kcds_vendor_shelf";
constexpr char kShelfClass[] = "StashCorpse";
constexpr float kShelfDepth = 60.0f;
// C_Shop (WHGame 1.5.5): 440 bytes (its allocation in); the haggle switch (+ 120, its
// slot 10 sets it), the key the shop module finds it by (+ 208), its goods' inventory (+ 224), the
// keeper's WUID (+ 392, slot 4) and its shop database id (+ 432; 0: none, so no item filter and the
// game's price factors are 1).
constexpr std::size_t kShopSize = 448;
constexpr std::size_t kShopHaggle = 120;
constexpr std::size_t kShopKey = 208;
constexpr std::size_t kShopInventory = 224;
constexpr std::size_t kShopKeeper = 392;
constexpr std::size_t kShopDatabase = 432;
// Services + 440 the shop screen's I_UIShop, which is C_UIShop + 16: its + 24 the screen's state
// property (slot 1 reads it; 0 when the screen is down) and its + 4616 the shop it shows.
constexpr std::size_t kServicesShopScreen = 440;
constexpr std::size_t kScreenInterface = 16;
constexpr std::size_t kScreenState = 24;
constexpr std::size_t kScreenShop = 4616;
constexpr std::size_t kPropertyRead = 1;
// C_ShoppingTransaction: the shop it trades with (+ 24) and the basket (+ 152, a vector of the
// game's items).
constexpr std::size_t kTradeShop = 24;
constexpr std::size_t kTradeBasket = 152;
// An item: its handle, its class (the GUID at + 8), its units and the inventory it is in.
constexpr std::size_t kItemHandle = 0x30;
constexpr std::size_t kItemClass = 0x48;
constexpr std::size_t kClassGuid = 8;
constexpr std::size_t kItemAmount = 0x50;
constexpr std::size_t kItemInventory = 0x90;
// Our shop's keys: a shop WUID kind (4) with a range of our own, one key per session.
constexpr std::uint64_t kKeyBase = 0x04FFFFFF00000000ull;
// The money a purse that never runs out shows, and the purse's row on the shelf.
constexpr std::uint32_t kUnlimitedPurse = 1000000;
constexpr char kPurseRow[] = "$purse";
constexpr std::size_t kMaxReports = 64;
constexpr std::size_t kMaxEvents = 64;

using ShopConstruct = void*(__fastcall*)(void* shop);
using ShopOpen = void(__fastcall*)(void* shop, std::uint64_t keeper);
using ShopByKey = void*(__fastcall*)(void* registry, std::uint64_t key);
using ItemPrice = int*(__fastcall*)(void* calculator, int* price, void* item, void* buyer,
                                    void* seller, int selling, void* shop);
using TradeHaggle = void*(__fastcall*)(void* trade, char asked);
using TradeExecute = std::int64_t(__fastcall*)(void* trade, unsigned price, void* keeper);
using ScreenClose = std::int64_t(__fastcall*)(void* screen);
using PropertyRead = std::uint8_t(__fastcall*)(void* property);

Natives natives;
Logger log_line = nullptr;
bool installed = false;
ShopByKey original_shop_by_key = nullptr;
ItemPrice original_item_price = nullptr;
TradeHaggle original_trade_haggle = nullptr;
TradeExecute original_trade_execute = nullptr;

// Our shop, made once and never handed to the game to free.
alignas(16) std::uint8_t shop_memory[kShopSize] = {};
void* shop = nullptr;
std::uint64_t default_keeper = 0;
// The keys our shop had so far.
std::uint32_t keys_used = 0;

// The session the server showed, and the shelf the screen shows.
struct Session {
  std::uint32_t id = 0;
  std::uint64_t revision = 0;
  bool buys = true;
  // The vendor haggles: the screen's haggle runs the game's, answered by the server.
  bool haggles = false;
  std::uint32_t npc = 0;
  std::uint64_t key = 0;
  // The screen is up (vendorOpened was raised).
  bool up = false;
  void* inventory = nullptr;
  // The shelf: the items that show a row, by their handles; the rows' classes and prices.
  std::unordered_map<std::uint64_t, std::string> items;
  std::map<std::string, std::pair<integration::Guid, std::uint32_t>> rows;
  std::vector<integration::VendorBuyPrice> buy_prices;
};

std::optional<Session> current;
// A screen the server asked for, brought up on the next frame; with an NPC keeper, once this
// game runs the NPC (the server hands it over), for at most kKeeperWaitMs.
std::optional<integration::VendorShow> waiting;
ULONGLONG waiting_since = 0;
constexpr ULONGLONG kKeeperWaitMs = 5000;

std::mutex reports_mutex;
std::vector<integration::VendorTrade> trades;
std::vector<integration::VendorClosed> closes;

struct Event {
  std::uint32_t session = 0;
  bool opened = false;
  std::uint32_t npc = 0;
  std::string reason;
};
std::mutex events_mutex;
std::vector<Event> events;

template <class Function>
Function slot(void* object, std::size_t index) {
  return (*static_cast<Function**>(object))[index];
}

template <class Value>
Value at(const void* object, std::size_t offset) {
  Value value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Value>
void put(void* object, std::size_t offset, const Value& value) {
  std::memcpy(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(value));
}

void raise(std::uint32_t session, bool opened, std::uint32_t npc, const char* reason) {
  std::lock_guard<std::mutex> lock(events_mutex);
  if (events.size() < kMaxEvents) {
    events.push_back(Event{session, opened, npc, reason});
  }
}

void report_closed(std::uint32_t session, integration::VendorClosedReason reason) {
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (closes.size() < kMaxReports) {
    closes.push_back(integration::VendorClosed{session, reason});
  }
}

// The I_UIShop of the shop screen, or null.
void* screen_interface() {
  void* services = natives.services ? natives.services() : nullptr;
  return services ? at<void*>(services, kServicesShopScreen) : nullptr;
}

// Whether the shop screen is up with our shop on it.
bool our_screen_up() {
  void* screen = screen_interface();
  if (!screen || !shop) {
    return false;
  }
  void* state = static_cast<std::uint8_t*>(screen) + kScreenState;
  const std::uint8_t up = slot<PropertyRead>(state, kPropertyRead)(state);
  return up != 0 && at<void*>(screen, kScreenShop) == shop;
}

bool ours(void* trade) {
  return trade && shop && current && at<void*>(trade, kTradeShop) == shop;
}

integration::Guid class_of(void* item) {
  integration::Guid guid{};
  void* item_class = at<void*>(item, kItemClass);
  if (item_class) {
    std::memcpy(guid.data(), static_cast<const std::uint8_t*>(item_class) + kClassGuid,
                guid.size());
  }
  return guid;
}

// The shelf row an item of our inventory shows: by its handle, or (an item the screen split off a
// stack) by its class, which a shelf lists once.
std::string shelf_row_of(void* item) {
  const auto handle = at<std::uint64_t>(item, kItemHandle);
  if (const auto found = current->items.find(handle); found != current->items.end()) {
    return found->second;
  }
  const integration::Guid item_class = class_of(item);
  for (const auto& [row, listed] : current->rows) {
    if (listed.first == item_class) {
      return row;
    }
  }
  return {};
}

// ---- Hooks --------------------------------------------------------------------------------------

void* __fastcall shop_by_key(void* registry, std::uint64_t key) {
  if (shop && current && key == current->key) {
    return shop;
  }
  return original_shop_by_key(registry, key);
}

// One unit's price on our screen: a shelf row's from the server's shelf, a player's item what the
// vendor's buy list pays for its class.
int* __fastcall item_price(void* calculator, int* price, void* item, void* buyer, void* seller,
                           int selling, void* priced_shop) {
  if (!shop || priced_shop != shop || !current || !item || !price) {
    return original_item_price(calculator, price, item, buyer, seller, selling, priced_shop);
  }
  std::uint32_t value = 0;
  if (at<void*>(item, kItemInventory) == current->inventory) {
    const std::string row = shelf_row_of(item);
    if (const auto found = current->rows.find(row); found != current->rows.end()) {
      value = found->second.second;
    }
  } else if (current->buys) {
    const integration::Guid item_class = class_of(item);
    for (const integration::VendorBuyPrice& buy : current->buy_prices) {
      if (buy.item == item_class) {
        value = buy.price;
        break;
      }
    }
  }
  *price = int(value);
  return price;
}

void add_line(std::vector<integration::VendorTradeLine>& lines, const std::string& row,
              std::uint32_t units) {
  for (integration::VendorTradeLine& line : lines) {
    if (line.row == row) {
      line.units += units;
      return;
    }
  }
  lines.push_back(integration::VendorTradeLine{row, units});
}

// The transaction's basket as the server's lines: shelf rows bought, the player's rows sold.
void read_basket(void* trade, integration::VendorTrade& basket) {
  basket.session = current->id;
  basket.revision = current->revision;
  void* const* item = at<void* const*>(trade, kTradeBasket);
  void* const* end = at<void* const*>(trade, kTradeBasket + 8);
  for (; item && item != end; ++item) {
    if (!*item) {
      continue;
    }
    const auto units = at<std::uint32_t>(*item, kItemAmount);
    if (units == 0) {
      continue;
    }
    if (at<void*>(*item, kItemInventory) == current->inventory) {
      const std::string row = shelf_row_of(*item);
      if (!row.empty() && row != kPurseRow) {
        add_line(basket.bought, row, units);
      }
      continue;
    }
    std::string row;
    std::uint64_t revision = 0;
    if (inventory::row_of(at<std::uint64_t>(*item, kItemHandle), row, revision)) {
      add_line(basket.sold, row, units);
    } else if (log_line) {
      log_line("VENDOR %u: a sold item no row keeps stays out of the basket", current->id);
    }
  }
}

// A server vendor haggles only when it says so: then the game's own haggle runs, and the server
// answers its offers (vendor_haggle.hpp). Otherwise Trade goes to the transaction's accept.
void* __fastcall trade_haggle(void* trade, char asked) {
  if (!ours(trade)) {
    // The game's own haggle with one of its traders: nothing of ours stays armed.
    haggle::reset();
    return original_trade_haggle(trade, asked);
  }
  if (!current->haggles || !haggle::installed()) {
    return nullptr;
  }
  integration::VendorTrade basket;
  read_basket(trade, basket);
  if (basket.bought.empty() && basket.sold.empty()) {
    return nullptr;
  }
  void* dialogue = original_trade_haggle(trade, asked);
  if (dialogue) {
    haggle::begin(current->id, current->revision, std::move(basket.bought), std::move(basket.sold));
  }
  if (log_line) {
    log_line("VENDOR %u haggle %s", current->id, dialogue ? "started" : "did not start");
  }
  return dialogue;
}

// The player confirmed the basket: nothing moves here; the server settles it (at the agreed
// price after a haggle).
std::int64_t __fastcall trade_execute(void* trade, unsigned price, void* keeper) {
  if (!ours(trade)) {
    return original_trade_execute(trade, price, keeper);
  }
  integration::VendorTrade basket;
  read_basket(trade, basket);
  haggle::end();
  if (log_line) {
    log_line("VENDOR %u trade: %zu bought, %zu sold lines (game price %u) for the server",
             current->id, basket.bought.size(), basket.sold.size(), price);
  }
  if (basket.bought.empty() && basket.sold.empty()) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(reports_mutex);
  if (trades.size() < kMaxReports) {
    trades.push_back(std::move(basket));
  }
  return 0;
}

// ---- The shelf ----------------------------------------------------------------------------------

// Stands the shelf's container when it is not there; the screen comes up on the next frame, once
// the container has its inventory.
void stand_shelf() {
  if (props::entity::find(kShelfName)) {
    return;
  }
  void* player = natives.local_entity ? natives.local_entity() : nullptr;
  if (!player) {
    return;
  }
  float position[3] = {};
  props::entity::world_position(player, position);
  position[2] -= kShelfDepth;
  props::entity::spawn_class(kShelfName, kShelfClass, position);
}

void* shelf_inventory() {
  void* entity = props::entity::find(kShelfName);
  return entity ? inventory::inventory_of_entity(entity) : nullptr;
}

// The session's shelf in our inventory, with the purse as a money row.
void fill(Session& session, const integration::VendorShow& show) {
  session.revision = show.revision;
  session.buys = show.buys;
  session.haggles = show.haggles;
  session.buy_prices = show.buy_prices;
  session.rows.clear();
  std::vector<integration::InventoryRowState> rows;
  for (const integration::VendorShelfRow& shelf_row : show.shelf) {
    rows.push_back(shelf_row.row);
    session.rows[shelf_row.row.id] = {shelf_row.row.item, shelf_row.price};
  }
  const std::uint32_t purse = show.purse_limited ? show.purse : kUnlimitedPurse;
  if (purse > 0) {
    integration::InventoryRowState money;
    money.id = kPurseRow;
    money.item = show.money;
    money.amount = purse;
    rows.push_back(std::move(money));
  }
  if (session.inventory) {
    session.items = inventory::fill_container(session.inventory, rows);
  }
}

// The keeper's WUID: the NPC's, or the shop's own none.
std::uint64_t keeper_of(std::uint32_t npc, bool player) {
  const auto lookup = player ? natives.player_entity : natives.npc_entity;
  void* entity = npc && lookup ? lookup(npc) : nullptr;
  std::uint64_t wuid = 0;
  if (entity && natives.wuid_of && natives.wuid_of(entity, wuid)) {
    return wuid;
  }
  return default_keeper;
}

// Ends the session we hold: the server's close, a replacement, or the screen going away.
void forget(const char* reason) {
  if (!current) {
    return;
  }
  if (current->up) {
    raise(current->id, false, current->npc, reason);
  }
  haggle::end();
  // The container stays, empty, for the next session of this world.
  if (current->inventory) {
    inventory::fill_container(current->inventory, {});
  }
  current.reset();
}

// Takes our screen down with the screen's own close.
void close_screen() {
  void* screen = screen_interface();
  if (screen && our_screen_up()) {
    reinterpret_cast<ScreenClose>(natives.screen_close)(static_cast<std::uint8_t*>(screen) -
                                                        kScreenInterface);
  }
}

// Brings the screen up for the session the server asked for.
void open(const integration::VendorShow& show) {
  // The screen's SetShopInfo reads the trader: without a keeper of an NPC the game faulted in it
  // (0.2.2), so a session without a live NPC keeper is refused.
  const std::uint64_t keeper = keeper_of(show.npc, show.keeper_player);
  if (keeper == 0 || keeper == default_keeper) {
    report_closed(show.session, integration::VendorClosedReason::Failed);
    if (log_line) {
      log_line("VENDOR %u: no NPC keeper, the screen stays down", show.session);
    }
    return;
  }
  if (!shop) {
    reinterpret_cast<ShopConstruct>(natives.shop_construct)(shop_memory);
    shop = shop_memory;
    default_keeper = at<std::uint64_t>(shop, kShopKeeper);
  }
  Session session;
  session.id = show.session;
  session.npc = show.keeper_player ? 0 : show.npc;
  // A key never used before in this game: the shop module keeps a transaction per key, with the
  // player it was made for.
  session.key = kKeyBase | ++keys_used;
  session.inventory = shelf_inventory();
  if (!session.inventory) {
    report_closed(show.session, integration::VendorClosedReason::Failed);
    if (log_line) {
      log_line("VENDOR %u: no shelf inventory, the screen stays down", show.session);
    }
    return;
  }
  fill(session, show);
  put<std::uint8_t>(shop, kShopHaggle, session.haggles && haggle::installed() ? 1 : 0);
  put<std::uint64_t>(shop, kShopKey, session.key);
  put<void*>(shop, kShopInventory, session.inventory);
  put<std::uint32_t>(shop, kShopDatabase, 0);
  current = std::move(session);
  reinterpret_cast<ShopOpen>(natives.shop_open)(shop, keeper);
  if (!our_screen_up()) {
    report_closed(show.session, integration::VendorClosedReason::Failed);
    forget("failed");
    if (log_line) {
      log_line("VENDOR %u: the game's shop screen did not come up", show.session);
    }
    return;
  }
  current->up = true;
  raise(current->id, true, current->npc, "");
  if (log_line) {
    log_line("VENDOR %u open: %zu shelf rows, %zu bought classes", show.session, show.shelf.size(),
             show.buy_prices.size());
  }
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  installed = natives.services && natives.local_entity && natives.shop_construct &&
              natives.shop_open && natives.shop_by_key && natives.item_price &&
              natives.trade_haggle && natives.trade_execute && natives.screen_close;
  if (!installed) {
    return false;
  }
  haggle::Natives haggle_natives;
  haggle_natives.services = natives.services;
  haggle_natives.haggle_input = natives.haggle_input;
  haggle_natives.set_global = natives.set_global;
  haggle_natives.get_local = natives.get_local;
  haggle_natives.variable_index = natives.variable_index;
  haggle_natives.end_with_number = natives.end_with_number;
  const bool haggles = haggle::install(haggle_natives, logger);
  if (log_line) {
    log_line("VENDOR haggle %s",
             haggles ? "installed" : "unavailable: vendors trade at list prices");
  }
  installed = MH_CreateHook(natives.shop_by_key, reinterpret_cast<void*>(shop_by_key),
                            reinterpret_cast<void**>(&original_shop_by_key)) == MH_OK &&
              MH_EnableHook(natives.shop_by_key) == MH_OK &&
              MH_CreateHook(natives.item_price, reinterpret_cast<void*>(item_price),
                            reinterpret_cast<void**>(&original_item_price)) == MH_OK &&
              MH_EnableHook(natives.item_price) == MH_OK &&
              MH_CreateHook(natives.trade_haggle, reinterpret_cast<void*>(trade_haggle),
                            reinterpret_cast<void**>(&original_trade_haggle)) == MH_OK &&
              MH_EnableHook(natives.trade_haggle) == MH_OK &&
              MH_CreateHook(natives.trade_execute, reinterpret_cast<void*>(trade_execute),
                            reinterpret_cast<void**>(&original_trade_execute)) == MH_OK &&
              MH_EnableHook(natives.trade_execute) == MH_OK;
  return installed;
}

void apply(const integration::VendorOrder& order) {
  if (!installed) {
    if (const auto* show = std::get_if<integration::VendorShow>(&order); show && show->open) {
      report_closed(show->session, integration::VendorClosedReason::Failed);
    }
    return;
  }
  if (const auto* reply = std::get_if<integration::VendorHaggleReply>(&order)) {
    if (current && current->id == reply->session) {
      haggle::answer(*reply);
    }
    return;
  }
  if (const auto* close = std::get_if<integration::VendorClose>(&order)) {
    if (waiting && waiting->session == close->session) {
      waiting.reset();
    }
    if (current && current->id == close->session) {
      close_screen();
      forget("server");
    }
    return;
  }
  const auto& show = std::get<integration::VendorShow>(order);
  if (show.open) {
    // The game has one shop screen: the session before goes, replaced.
    if (current && current->id != show.session) {
      close_screen();
      forget("replaced");
    }
    waiting = show;
    waiting_since = GetTickCount64();
    stand_shelf();
    return;
  }
  if (waiting && waiting->session == show.session) {
    // A refresh before the screen came up: it comes up with this shelf.
    const bool open_it = waiting->open;
    waiting = show;
    waiting->open = open_it;
    return;
  }
  if (current && current->id == show.session) {
    fill(*current, show);
  }
}

void frame(bool in_world) {
  if (!installed) {
    return;
  }
  if (!in_world) {
    if (waiting) {
      report_closed(waiting->session, integration::VendorClosedReason::WorldChanged);
      waiting.reset();
    }
    return;
  }
  haggle::frame();
  if (current && current->up && !our_screen_up() && !haggle::running(current->id)) {
    // The player closed it, or Trade did (the basket went before this). A haggle's dialogue may
    // hide the screen while it runs.
    const std::uint32_t session = current->id;
    forget("player");
    report_closed(session, integration::VendorClosedReason::Player);
    if (log_line) {
      log_line("VENDOR %u closed", session);
    }
  }
  if (waiting) {
    const std::uint64_t keeper = keeper_of(waiting->npc, waiting->keeper_player);
    const bool keeper_ready = keeper != 0 && keeper != default_keeper;
    if (waiting->npc != 0 && !keeper_ready && GetTickCount64() - waiting_since < kKeeperWaitMs) {
      return;
    }
    const integration::VendorShow show = *waiting;
    waiting.reset();
    open(show);
  }
}

std::vector<integration::VendorTrade> take_trades() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(trades, {});
}

std::vector<integration::VendorClosed> take_closes() {
  std::lock_guard<std::mutex> lock(reports_mutex);
  return std::exchange(closes, {});
}

std::vector<integration::VendorHaggle> take_haggles() {
  return haggle::take();
}

void reset(bool connection_over) {
  if (waiting) {
    report_closed(waiting->session, integration::VendorClosedReason::WorldChanged);
    waiting.reset();
  }
  if (!current) {
    return;
  }
  const std::uint32_t session = current->id;
  if (current->up) {
    raise(session, false, current->npc, connection_over ? "sessionOver" : "levelChange");
  }
  // The level took the shelf and the screen with it.
  current.reset();
  report_closed(session, integration::VendorClosedReason::WorldChanged);
}

void pump(const RuntimeSender& send) {
  std::vector<Event> taken;
  {
    std::lock_guard<std::mutex> lock(events_mutex);
    taken.swap(events);
  }
  for (const Event& event : taken) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("vendorEvent"))},
                            {"session", sdk::Value(std::int64_t(event.session))},
                            {"name", sdk::Value(std::string(event.opened ? "opened" : "closed"))},
                            {"npc", event.npc ? sdk::Value(std::int64_t(event.npc)) : sdk::Value()},
                            {"reason", sdk::Value(event.reason)}};
    send(sdk::Value(std::move(message)));
  }
}

}  // namespace kcds::runtime::vendors
