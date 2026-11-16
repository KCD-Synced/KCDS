// A smithy workpiece without a character does not crash the game.
//
// When the tongs take a workpiece, the game updates the attachments of the workpiece's character
// every frame (a lambda of C_BlacksmithingWorkpieceAttachment, WHGame 1.5.5): it asks the
// workpiece's entity for its character (IEntity slot 101, GetCharacter(0)) and goes through that
// character's attachments without checking it has one. A workpiece whose model is not a character
// (a .cgf) or did not load has none, and the game crashed there. The update is hooked: such a
// workpiece is skipped (its attachments stay as they are) and logged once.
//
// Engine thread.
#pragma once

namespace kcds::runtime::crafting::workpiece_guard {

using Logger = void (*)(const char* format, ...);

// `update`: the lambda's operator() (its signature in crafting_install.cpp). False when it is
// missing or could not be hooked.
bool install(void* update, Logger logger);

}  // namespace kcds::runtime::crafting::workpiece_guard
