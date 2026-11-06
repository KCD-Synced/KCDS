#include "vendor_haggle.hpp"

#include <Windows.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

#include "MinHook.h"

namespace kcds::runtime::vendors::haggle {
namespace {

namespace integration = kcd2s::integration;

using HaggleInput = std::int64_t(__fastcall*)(void* extension, int price, char finish);
using SetGlobal = std::int64_t(__fastcall*)(void* self, void* call, const char* name, float value);
using GetLocal = std::int64_t(__fastcall*)(void* self, void* call, const char* name,
                                           unsigned context);
using VariableIndex = unsigned(__fastcall*)(void* store, const char* name);
using EndWithNumber = std::int64_t(__fastcall*)(void* call, const float* value);

// The panel's input when the player cancels it.
constexpr int kInputCancelled = int(0x80000000u);
// How long an offer waits for the server, and a haggle without a word from its dialogue lasts.
constexpr ULONGLONG kAnswerWaitMs = 5000;
constexpr ULONGLONG kHaggleQuietMs = 120000;
// How long the last answer stays armed after its haggle ended: the dialogue reads it after the
// held input goes on. The game's other negotiations (bribes, lodging) use the same variables.
constexpr ULONGLONG kReactionKeepMs = 3000;
constexpr std::size_t kMaxOffers = 16;

// The variables the dialogue reads the trader's reaction from (Negotiation.lua's
// ReactToPlayerProposition and UpdateUiState), and the local one its accept trades at.
constexpr const char* kReactionKind = "negotiation_merchantReaction_kind";
constexpr const char* kPlayerAccepts = "negotiation_playerReaction_accept";
constexpr const char* kTip = "negotiation_merchantReaction_tip";
constexpr const char* kDivergence = "negotiation_merchantReaction_divergence";
constexpr const char* kOrientation = "negotiation_merchantReaction_orientation";
constexpr const char* kKeeperPrice = "shop_keeper_price";
constexpr const char* kYourPrice = "shop_your_price";
constexpr const char* kProgress = "shop_progress";
constexpr const char* kResultPrice = "negotiation_result_price";
// What haggle start wrote for the basket: the values of what the player buys and sells.
constexpr const char* kBuysValue = "haggle_playerBuysValue";
constexpr const char* kSellsValue = "haggle_playerSellsValue";
constexpr std::size_t kMaxReaction = 8;

struct Reaction {
  const char* name = nullptr;
  float value = 0;
};

Natives natives;
Logger log_line = nullptr;
bool hooked = false;
HaggleInput original_haggle_input = nullptr;
SetGlobal original_set_global = nullptr;
GetLocal original_get_local = nullptr;

// The haggle that runs.
struct Haggle {
  std::uint32_t session = 0;
  std::uint64_t revision = 0;
  std::vector<integration::VendorTradeLine> bought;
  std::vector<integration::VendorTradeLine> sold;
  std::uint32_t round = 0;
  ULONGLONG heard = 0;
  // The input held for the server's answer.
  void* extension = nullptr;
  int offered = 0;
  char finish = 0;
  ULONGLONG asked = 0;
};
bool running_haggle = false;
Haggle current;

// The server's answer the dialogue reads: the reaction's variables, and the agreed price.
Reaction reaction[kMaxReaction] = {};
std::size_t reaction_size = 0;
bool agreed = false;
float agreed_price = 0;
// When the last held input went on.
ULONGLONG released = 0;

std::mutex offers_mutex;
std::vector<integration::VendorHaggle> offers;

void* variable_store() {
  void* services = natives.services ? natives.services() : nullptr;
  return services ? *static_cast<void**>(services) : nullptr;
}

// A global variable's slot in the store: { float value, name hash } entries.
float* variable(const char* name) {
  void* store = variable_store();
  if (!store) {
    return nullptr;
  }
  const unsigned index = reinterpret_cast<VariableIndex>(natives.variable_index)(store, name);
  auto* const* range = static_cast<std::uint8_t* const*>(store);
  const std::size_t count = std::size_t(range[1] - range[0]) / 8;
  if (index >= count) {
    return nullptr;
  }
  return reinterpret_cast<float*>(range[0] + std::size_t(index) * 8);
}

float read_variable(const char* name) {
  const float* slot = variable(name);
  return slot ? *slot : 0.0f;
}

void disarm() {
  reaction_size = 0;
  agreed = false;
}

void react(const char* name, float value) {
  if (reaction_size < kMaxReaction) {
    reaction[reaction_size++] = Reaction{name, value};
  }
  if (float* slot = variable(name)) {
    *slot = value;
  }
}

// The game's divergence rank of a counter: how far the price is from the offer, by the volume.
float divergence_of(double price, double offered, double volume) {
  const double divergence = volume > 0 ? (price - offered) / volume : 1.0;
  if (divergence < 0.1) {
    return 1.0f;
  }
  if (divergence < 0.35) {
    return 2.0f;
  }
  return 3.0f;
}

// The server's answer as the dialogue's variables.
void arm(const integration::VendorHaggleReply& reply, int offered) {
  disarm();
  const double buys = read_variable(kBuysValue);
  const double sells = read_variable(kSellsValue);
  const double price = double(reply.price);
  const float progress = float(reply.mood) / float(integration::kMaxVendorHaggleMood);
  react(kReactionKind, float(std::uint8_t(reply.answer)));
  switch (reply.answer) {
    case integration::VendorHaggleAnswer::Accept:
      react(kPlayerAccepts, 0.0f);
      react(kTip, price > buys - sells ? 1.0f : 0.0f);
      agreed = true;
      agreed_price = float(price);
      break;
    case integration::VendorHaggleAnswer::Counter:
      react(kDivergence, divergence_of(price, double(offered), buys + sells));
      react(kOrientation, offered >= 0 ? 1.0f : -1.0f);
      react(kKeeperPrice, float(price));
      react(kYourPrice, float(offered));
      react(kProgress, progress);
      break;
    case integration::VendorHaggleAnswer::Mock:
      react(kKeeperPrice, float(price));
      react(kYourPrice, float(offered));
      react(kProgress, progress);
      break;
    case integration::VendorHaggleAnswer::Refuse:
      react(kProgress, progress);
      break;
  }
}

// Lets the held input go on: the dialogue reads the reaction now.
void release(int price) {
  void* extension = std::exchange(current.extension, nullptr);
  released = GetTickCount64();
  if (extension) {
    original_haggle_input(extension, price, current.finish);
  }
}

// ---- Hooks --------------------------------------------------------------------------------------

std::int64_t __fastcall haggle_input(void* extension, int price, char finish) {
  if (!running_haggle) {
    return original_haggle_input(extension, price, finish);
  }
  current.heard = GetTickCount64();
  if (price == kInputCancelled) {
    // The player left the panel: the dialogue ends the haggle.
    current.extension = nullptr;
    disarm();
    running_haggle = false;
    return original_haggle_input(extension, price, finish);
  }
  if (current.extension) {
    // One offer at a time: the panel waits for the answer.
    return 0;
  }
  if (current.round >= integration::kMaxVendorHaggleRounds) {
    running_haggle = false;
    return original_haggle_input(extension, kInputCancelled, finish);
  }
  current.extension = extension;
  current.offered = price;
  current.finish = finish;
  current.asked = current.heard;
  current.round += 1;
  integration::VendorHaggle offer;
  offer.session = current.session;
  offer.revision = current.revision;
  offer.round = current.round;
  offer.offered = price;
  offer.bought = current.bought;
  offer.sold = current.sold;
  {
    std::lock_guard<std::mutex> lock(offers_mutex);
    if (offers.size() < kMaxOffers) {
      offers.push_back(std::move(offer));
    }
  }
  if (log_line) {
    log_line("VENDOR %u haggle: offer %d, round %u", current.session, price, current.round);
  }
  return 0;
}

std::int64_t __fastcall set_global(void* self, void* call, const char* name, float value) {
  if (reaction_size > 0 && name) {
    for (std::size_t index = 0; index < reaction_size; ++index) {
      if (std::strcmp(reaction[index].name, name) == 0) {
        value = reaction[index].value;
        break;
      }
    }
  }
  return original_set_global(self, call, name, value);
}

std::int64_t __fastcall get_local(void* self, void* call, const char* name, unsigned context) {
  if (agreed && name && std::strcmp(name, kResultPrice) == 0) {
    const float price = agreed_price;
    return reinterpret_cast<EndWithNumber>(natives.end_with_number)(call, &price);
  }
  return original_get_local(self, call, name, context);
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.services || !natives.haggle_input || !natives.set_global || !natives.get_local ||
      !natives.variable_index || !natives.end_with_number) {
    return false;
  }
  hooked = MH_CreateHook(natives.haggle_input, reinterpret_cast<void*>(haggle_input),
                         reinterpret_cast<void**>(&original_haggle_input)) == MH_OK &&
           MH_EnableHook(natives.haggle_input) == MH_OK &&
           MH_CreateHook(natives.set_global, reinterpret_cast<void*>(set_global),
                         reinterpret_cast<void**>(&original_set_global)) == MH_OK &&
           MH_EnableHook(natives.set_global) == MH_OK &&
           MH_CreateHook(natives.get_local, reinterpret_cast<void*>(get_local),
                         reinterpret_cast<void**>(&original_get_local)) == MH_OK &&
           MH_EnableHook(natives.get_local) == MH_OK;
  return hooked;
}

bool installed() {
  return hooked;
}

void begin(std::uint32_t session, std::uint64_t revision,
           std::vector<integration::VendorTradeLine> bought,
           std::vector<integration::VendorTradeLine> sold) {
  disarm();
  current = Haggle{};
  current.session = session;
  current.revision = revision;
  current.bought = std::move(bought);
  current.sold = std::move(sold);
  current.heard = GetTickCount64();
  running_haggle = true;
}

bool running(std::uint32_t session) {
  return running_haggle && current.session == session;
}

void answer(const integration::VendorHaggleReply& reply) {
  if (!running_haggle || !current.extension || reply.session != current.session ||
      reply.round != current.round) {
    return;
  }
  current.heard = GetTickCount64();
  arm(reply, current.offered);
  if (log_line) {
    log_line("VENDOR %u haggle: answer %u, price %lld, mood %u", reply.session,
             unsigned(reply.answer), static_cast<long long>(reply.price), unsigned(reply.mood));
  }
  if (reply.answer == integration::VendorHaggleAnswer::Accept) {
    // The dialogue accepts at the agreed price; the server settles the basket at it.
    release(int(reply.price));
    return;
  }
  release(current.offered);
  if (reply.answer == integration::VendorHaggleAnswer::Refuse) {
    running_haggle = false;
  }
}

void frame() {
  const ULONGLONG now = GetTickCount64();
  if (!running_haggle) {
    if ((reaction_size > 0 || agreed) && now - released > kReactionKeepMs) {
      disarm();
    }
    return;
  }
  if (current.extension && now - current.asked > kAnswerWaitMs) {
    if (log_line) {
      log_line("VENDOR %u haggle: no answer, the offer is cancelled", current.session);
    }
    disarm();
    running_haggle = false;
    release(kInputCancelled);
    return;
  }
  if (!current.extension && now - current.heard > kHaggleQuietMs) {
    disarm();
    running_haggle = false;
  }
}

void end() {
  running_haggle = false;
  current.extension = nullptr;
  released = GetTickCount64();
}

void reset() {
  end();
  disarm();
}

std::vector<integration::VendorHaggle> take() {
  std::lock_guard<std::mutex> lock(offers_mutex);
  return std::exchange(offers, {});
}

}  // namespace kcds::runtime::vendors::haggle
