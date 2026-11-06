// The server's conversations (Dialogue, DIALOGUE_* in kcd2s/integration/dialogue.hpp)
// in the game's own dialogue list.
//
// The list is the HUD's interactive dialogue (Libs/UI/UIElements/HUD.xml, the movie clips
// DialogLeft and DialogRight), driven through the HUD element's own functions the way the game's
// C_UIHudInteractiveDialog does (its: AddSequence, ShowSequences
// which fills the array SequenceList and calls SetDialogSequences, SetActiveSequence
// SetEnableSequence):
//   - SequenceList gets five values per row, as ShowSequences writes them: the text, whether the
//     row was used (drawn darker), a skill check name, its difficulty and a value (none here);
//   - SetDialogSequences(on right) draws the rows, ClearDialogSequences takes them off;
//   - SetActiveSequenceChanged(index, 0) highlights a row, SetEnableSequenceChanged(index, false)
//     greys one out;
//   - the line above the rows is the HUD's subtitles, SetSubtitles(text, speaker) and
//     ClearSubtitles.
//
// The game's own dialogue controller moves the highlight on its "dialog" action map (up, down,
// dialog_confirm), which is exclusive and only on while the game's own conversation runs. While a
// server conversation is shown the local player's input does the same instead: the forward and
// back movement actions move the highlight, the use action picks the row (only a fresh press: not
// the key still held when a page showed, nor a press within 350 ms of it), and every other action
// of the player is refused except looking around and the menus, so the player stands still while
// they talk. The arrow keys and Enter do the same, the number keys 1 to 8 pick a row at once and
// Backspace backs out of the conversation (reason 1).
//
// A pick hides nothing: the row stays highlighted until the server sends the next page or closes
// the conversation, and a second pick of the same page is not sent. The client ends the
// conversation itself (reason 3) when the game's own conversation begins or the HUD cannot show
// the list, and forgets it without a word when it leaves the world.
//
// Engine thread only.
#pragma once

#include <vector>

#include "kcd2s/integration/dialogue.hpp"

namespace kcds::runtime::dialogue {

// The server's page or close.
void apply(const kcd2s::integration::DialogueOrder& order);

// The local player's input action (from the player input's OnAction; mode 1 pressed, 2 released,
// 4 held): true when it went to the conversation or must not reach the game while one is shown.
bool on_action(const char* action, unsigned mode);

// Whether a server conversation is shown.
bool showing();

// Every frame: polls the keys (when `keys` is true: the game window has the focus and no chat or
// screen takes the input), ends the conversation when the game's own one began, and draws what
// changed. Outside a world the conversation is forgotten.
void frame(bool in_world, bool game_in_dialogue, bool keys);

// The picks and the conversations ended on this side since the last call, for the server.
std::vector<kcd2s::integration::DialogueChoice> take_choices();
std::vector<kcd2s::integration::DialogueCancel> take_cancels();

// The client left the world: the list is taken off and the conversation forgotten.
void reset();

}  // namespace kcds::runtime::dialogue
