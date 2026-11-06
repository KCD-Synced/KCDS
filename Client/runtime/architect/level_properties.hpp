// World Architect: a level entity's own properties in Details (a door's lock, a light's colour and
// radius, a spawn's settings ...). They are the
// fields of the entity's script table "Properties" as the level set them, read and set natively
// through the script table (IScriptTable, WHGame 1.5.5: GetValueAny slot 8, SetValueAny slot 7,
// Release slot 4, Dump slot 21, which hands every key to a sink: slot 1 a numbered key, slot 2 a
// named one, with its type 0 nil, 1 string, 2 number, 3 bool, 4 function, 5 table, 6 pointer, 7
// user data).
//
//   sync localEntityProperties { guid }
//     -> [{ key, type: 'bool' | 'number' | 'text', value }]   the bools, numbers and texts of
//        Properties and of its tables one level down ("Lock.bLocked"), at most kMostProperties;
//        null for an entity the level does not have here
//   { op: 'localEntityProperty', guid, key, value }
//     sets one of them in this game (a bool, a number, a text as it is now); what the entity does
//     with it is its own: many read their properties when they are reset or loaded again
//
// Local mode only. handle_runtime_message: any thread; frame: engine thread; pump: network thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::architect::level_properties {

// True when the message was one of the above.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers and applies what was asked since the last frame.
void frame(bool in_world);

// Sends the answers made.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::architect::level_properties
