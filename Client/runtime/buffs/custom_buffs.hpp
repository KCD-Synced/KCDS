// The resources' own buffs on the local player's status list (Buffs.register;
// CUSTOM_BUFF in kcd2s/integration/custom_buffs.hpp). The server keeps them, applies their stat
// changes and ends them; this client shows each one as a buff of the game's own buff manager.
//
// The game's buff table takes no new rows, so a custom buff is an instance of a copy of the
// definition of the game buff whose icon it borrows (WHGame 1.5.5, rows of 152 bytes, the
// buff table's columns: + 0 GUID, + 16 buff_name, + 24 buff_desc, + 32 class, + 36 family,
// + 40 icon_id, + 48 duration (float seconds), + 52 exclusivity, + 56 implementation, + 64
// buff_params, + 72 buff_ui_name, + 84 AI tag, + 92 is_persistent, + 96 visual_effect, + 144
// the implementation's class name). The copy keeps the GUID, the class and the icon (icon_id
// becomes the buff's own picture when it has one), and gets:
//   - no stat modifiers (buff_params empty; an instance's init reads them from that text)
// and no visual effect: the server applies the stat changes itself;
//   - the label as its name, no description;
//   - its own run: the implementation BasicTimed (C_TimedSoulBuffInstance: + 216 duration and
//     + 224 elapsed, doubles, real seconds; a duration below zero never ends) and the duration;
//   - no family, exclusivity or AI tag, so it neither replaces nor is replaced by a game buff,
//     and it is not saved.
// player_buffs.hpp's add_defined() puts the instance on the soul through the game's add. The
// copies are never freed: an instance holds its definition.
//
// The local player's report (BUFFS) and the replicas' mirror leave these instances out
// (owns()). An instance the game took off before its end (a death, a removal of the icon's
// buff) is put back with the time it has left.
//
// Engine thread.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/custom_buffs.hpp"

namespace kcds::runtime::buffs::custom {

using Logger = void (*)(const char* format, ...);

void set_logger(Logger logger);

// The game's CryString constructor from a text; without it no custom buff is shown,
// because the copied definition row holds the game's own strings.
void set_string_from_text(void* native);

// Every frame: the local player's soul (nullptr outside a server world: every custom buff goes).
void tick(void* soul);

// The server's shows and hides. A show with its own picture waits until the picture is mounted
// (a few seconds at most, then it shows with the borrowed icon).
void apply(void* soul, const std::vector<kcd2s::integration::CustomBuff>& orders);

// The buffs' own pictures the resource runtime is to send and mount (resource, path), since the
// last call.
std::vector<std::pair<std::string, std::string>> take_wanted_images();

// Every few hundred ms: puts back the shown buffs the game took off before their end.
void keep(void* soul);

// Whether the definition is one of the copies.
bool owns(const void* definition);

}  // namespace kcds::runtime::buffs::custom
