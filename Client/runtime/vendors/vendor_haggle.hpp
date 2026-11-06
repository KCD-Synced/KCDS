// The game's own haggle with a server vendor that haggles (vendor_screen.hpp
// tells how the shop screen starts it, kcd2s/integration/vendor.hpp the messages).
//
// The game haggles in the dialogue SMLOUVANI with the trader: its script (Negotiation.lua) works
// out the trader's reaction to each price the player chooses on the dialogue's haggle panel
// (wh::dialogmodule::C_DialogueHaggleExtension), and the dialogue reads the reaction from global
// variables. For a server vendor the server answers instead:
//
//   - the panel's input (the extension's slot 6: (extension, price, finish), the price
//     0x80000000 when the player cancels) is held, and the offer goes to the server;
//   - the server's answer is written into the game's variable store (services + 0: a vector of
//     { float value, name hash }; its index by name, adding the name when missing) and
//     the held input goes on; while the dialogue's script then works out its own reaction, Lua's
//     Variables.SetGlobal ((self, call, name, value)) keeps the server's values of
//     the reaction's variables, and Lua's Variables.GetLocal ((self, call, name,
//     context)) answers negotiation_result_price, the price the dialogue's accept trades at,
//     with the agreed price (a script call's number result);
//   - an offer the server does not answer in time is cancelled on the panel.
//
// Prices are the game's and the server's money units: what the player pays, negative when the
// vendor pays. Engine thread.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/vendor.hpp"

namespace kcds::runtime::vendors::haggle {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;
  void* haggle_input = nullptr;  // hooked
  void* set_global = nullptr;    // hooked
  void* get_local = nullptr;     // hooked
  void* variable_index = nullptr;
  void* end_with_number = nullptr;
};

// Hooks the haggle; false when a native is missing (vendors that haggle then trade at their list
// prices).
bool install(const Natives& natives, Logger logger);
bool installed();

// The game's haggle started for the basket of `session`'s screen.
void begin(std::uint32_t session, std::uint64_t revision,
           std::vector<kcd2s::integration::VendorTradeLine> bought,
           std::vector<kcd2s::integration::VendorTradeLine> sold);

// A haggle runs for `session`: its dialogue may hide the shop screen meanwhile.
bool running(std::uint32_t session);

// The server's answer to the offer waiting for it.
void answer(const kcd2s::integration::VendorHaggleReply& reply);

// Cancels an offer the server did not answer in time and ends a haggle left without a dialogue.
void frame();

// The session's screen went away or its basket was traded: the haggle is over. Its last answer
// stays for the dialogue a few seconds more.
void end();

// Another haggle of the game's own starts: nothing of ours stays.
void reset();

// The offers made since the last call, oldest first.
std::vector<kcd2s::integration::VendorHaggle> take();

}  // namespace kcds::runtime::vendors::haggle
