#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Conversations the server drives on one player (Dialogue): a page of
// rows in the game's own dialogue list (the HUD's interactive dialogue, SetDialogSequences), an
// optional line above it (the HUD's subtitles), the row the player picks and the close. Every
// message is on the control channel (0).
//
//   DIALOGUE_PAGE   : server -> the player's client: show this page of `session`. Every page of
//                     a session carries a new generation, which the client echoes with a pick, so
//                     a pick made against a page that was replaced in the meantime is dropped.
//   DIALOGUE_CLOSE  : server -> the player's client: take `session` off the screen.
//   DIALOGUE_CHOICE : client -> server: the player picked row `index` of the page `generation`.
//   DIALOGUE_CANCEL : client -> server: the conversation ended on the player's side: 1 the player
//                     backed out, 3 something interrupted it (the game's own conversation began,
//                     the list could not be shown).
//
// The rows' ids stay on the server: the wire carries the row's index in the page it was shown in,
// and the server turns it back into the id. Client/runtime/dialogue/ implements the client side.
namespace kcd2s::integration {

inline constexpr std::uint8_t kDialoguePageTag = 119;
inline constexpr std::uint8_t kDialogueCloseTag = 120;
inline constexpr std::uint8_t kDialogueChoiceTag = 121;
inline constexpr std::uint8_t kDialogueCancelTag = 122;

// The game's list shows at most eight rows.
inline constexpr std::size_t kMaxDialogueOptions = 8;
inline constexpr std::size_t kMaxDialogueOptionText = 512;
inline constexpr std::size_t kMaxDialogueLine = 1024;
inline constexpr std::size_t kMaxDialogueSpeaker = 64;
// DIALOGUE_CHOICEs and DIALOGUE_CANCELs one client may send in a second; more are dropped.
inline constexpr unsigned kMaxDialogueReportsPerSecond = 16;

// Why a conversation ended, as dialogueClosed reports it.
enum class DialogueCloseReason : std::uint8_t {
  Completed = 0,    // the server closed it
  Cancelled = 1,    // the player backed out
  Replaced = 2,     // another conversation took its place
  Interrupted = 3,  // a disconnect, a death, the game's own conversation, a timeout
};

struct DialogueOption {
  std::string text;
  bool enabled = true;

  friend bool operator==(const DialogueOption& a, const DialogueOption& b) {
    return a.text == b.text && a.enabled == b.enabled;
  }
};

// DIALOGUE_PAGE.
struct DialoguePage {
  std::uint32_t session = 0;
  std::uint32_t generation = 0;
  bool on_right = false;
  std::string line;     // empty: no line above the rows
  std::string speaker;  // the name the line is said by; may be empty
  std::vector<DialogueOption> options;

  friend bool operator==(const DialoguePage& a, const DialoguePage& b) {
    return a.session == b.session && a.generation == b.generation && a.on_right == b.on_right &&
           a.line == b.line && a.speaker == b.speaker && a.options == b.options;
  }
};

// DIALOGUE_CLOSE.
struct DialogueClose {
  std::uint32_t session = 0;

  friend bool operator==(const DialogueClose& a, const DialogueClose& b) {
    return a.session == b.session;
  }
};

// What the server told the client, in the order it was told.
using DialogueOrder = std::variant<DialoguePage, DialogueClose>;

// DIALOGUE_CHOICE.
struct DialogueChoice {
  std::uint32_t session = 0;
  std::uint32_t generation = 0;
  std::uint8_t index = 0;

  friend bool operator==(const DialogueChoice& a, const DialogueChoice& b) {
    return a.session == b.session && a.generation == b.generation && a.index == b.index;
  }
};

// DIALOGUE_CANCEL.
struct DialogueCancel {
  std::uint32_t session = 0;
  DialogueCloseReason reason = DialogueCloseReason::Cancelled;

  friend bool operator==(const DialogueCancel& a, const DialogueCancel& b) {
    return a.session == b.session && a.reason == b.reason;
  }
};

namespace dialogue_detail {

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  w.write_byte_run(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_text(core::BitReader& r, std::string& text, std::size_t longest) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > longest) {
    return false;
  }
  text.assign(std::size_t(length), '\0');
  r.read_byte_run(reinterpret_cast<std::uint8_t*>(text.data()), text.size());
  return !r.overrun();
}

// A session id or generation read from a varuint: 1 .. 2^32 - 1.
inline bool read_id(core::BitReader& r, std::uint32_t& id) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value == 0 || value > 0xFFFFFFFFu) {
    return false;
  }
  id = std::uint32_t(value);
  return true;
}

inline bool read_whole(const core::BitReader& r) {
  return !r.overrun() && r.bits_remaining() == 0;
}

inline bool page_fits(const DialoguePage& page) {
  if (page.session == 0 || page.generation == 0 || page.options.empty() ||
      page.options.size() > kMaxDialogueOptions || page.line.size() > kMaxDialogueLine ||
      page.speaker.size() > kMaxDialogueSpeaker) {
    return false;
  }
  for (const DialogueOption& option : page.options) {
    if (option.text.empty() || option.text.size() > kMaxDialogueOptionText) {
      return false;
    }
  }
  return true;
}

}  // namespace dialogue_detail

// DIALOGUE_PAGE: [tag][varuint session][varuint generation][u8 on right][text line][text speaker]
// [u8 count][per row: text, u8 enabled]; a text is [varuint length][bytes]. Empty for a page that
// cannot travel: no session or generation, no rows or more than eight, an empty row, a text too
// long.
inline std::vector<std::uint8_t> encode_dialogue_page(const DialoguePage& page) {
  if (!dialogue_detail::page_fits(page)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDialoguePageTag, 8);
  w.write_varuint(page.session);
  w.write_varuint(page.generation);
  w.write_bits(page.on_right ? 1 : 0, 8);
  dialogue_detail::write_text(w, page.line);
  dialogue_detail::write_text(w, page.speaker);
  w.write_bits(page.options.size(), 8);
  for (const DialogueOption& option : page.options) {
    dialogue_detail::write_text(w, option.text);
    w.write_bits(option.enabled ? 1 : 0, 8);
  }
  return w.bytes();
}

inline bool decode_dialogue_page(const std::vector<std::uint8_t>& p, DialoguePage& page) {
  if (p.empty() || p[0] != kDialoguePageTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DialoguePage decoded;
  if (!dialogue_detail::read_id(r, decoded.session) ||
      !dialogue_detail::read_id(r, decoded.generation)) {
    return false;
  }
  const std::uint64_t on_right = r.read_bits(8);
  if (on_right > 1 || !dialogue_detail::read_text(r, decoded.line, kMaxDialogueLine) ||
      !dialogue_detail::read_text(r, decoded.speaker, kMaxDialogueSpeaker)) {
    return false;
  }
  decoded.on_right = on_right == 1;
  const std::uint64_t count = r.read_bits(8);
  if (r.overrun() || count == 0 || count > kMaxDialogueOptions) {
    return false;
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    DialogueOption option;
    if (!dialogue_detail::read_text(r, option.text, kMaxDialogueOptionText)) {
      return false;
    }
    const std::uint64_t enabled = r.read_bits(8);
    if (enabled > 1 || option.text.empty()) {
      return false;
    }
    option.enabled = enabled == 1;
    decoded.options.push_back(std::move(option));
  }
  if (!dialogue_detail::read_whole(r)) {
    return false;
  }
  page = std::move(decoded);
  return true;
}

// DIALOGUE_CLOSE: [tag][varuint session]; empty without a session.
inline std::vector<std::uint8_t> encode_dialogue_close(const DialogueClose& close) {
  if (close.session == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDialogueCloseTag, 8);
  w.write_varuint(close.session);
  return w.bytes();
}

inline bool decode_dialogue_close(const std::vector<std::uint8_t>& p, DialogueClose& close) {
  if (p.empty() || p[0] != kDialogueCloseTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DialogueClose decoded;
  if (!dialogue_detail::read_id(r, decoded.session) || !dialogue_detail::read_whole(r)) {
    return false;
  }
  close = decoded;
  return true;
}

// DIALOGUE_CHOICE: [tag][varuint session][varuint generation][u8 index]; empty for one that
// cannot travel.
inline std::vector<std::uint8_t> encode_dialogue_choice(const DialogueChoice& choice) {
  if (choice.session == 0 || choice.generation == 0 || choice.index >= kMaxDialogueOptions) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDialogueChoiceTag, 8);
  w.write_varuint(choice.session);
  w.write_varuint(choice.generation);
  w.write_bits(choice.index, 8);
  return w.bytes();
}

inline bool decode_dialogue_choice(const std::vector<std::uint8_t>& p, DialogueChoice& choice) {
  if (p.empty() || p[0] != kDialogueChoiceTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DialogueChoice decoded;
  if (!dialogue_detail::read_id(r, decoded.session) ||
      !dialogue_detail::read_id(r, decoded.generation)) {
    return false;
  }
  const std::uint64_t index = r.read_bits(8);
  if (!dialogue_detail::read_whole(r) || index >= kMaxDialogueOptions) {
    return false;
  }
  decoded.index = std::uint8_t(index);
  choice = decoded;
  return true;
}

// DIALOGUE_CANCEL: [tag][varuint session][u8 reason, 1 or 3]; empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_dialogue_cancel(const DialogueCancel& cancel) {
  if (cancel.session == 0 || (cancel.reason != DialogueCloseReason::Cancelled &&
                              cancel.reason != DialogueCloseReason::Interrupted)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kDialogueCancelTag, 8);
  w.write_varuint(cancel.session);
  w.write_bits(std::uint64_t(cancel.reason), 8);
  return w.bytes();
}

inline bool decode_dialogue_cancel(const std::vector<std::uint8_t>& p, DialogueCancel& cancel) {
  if (p.empty() || p[0] != kDialogueCancelTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DialogueCancel decoded;
  if (!dialogue_detail::read_id(r, decoded.session)) {
    return false;
  }
  const std::uint64_t reason = r.read_bits(8);
  if (!dialogue_detail::read_whole(r) ||
      (reason != std::uint64_t(DialogueCloseReason::Cancelled) &&
       reason != std::uint64_t(DialogueCloseReason::Interrupted))) {
    return false;
  }
  decoded.reason = DialogueCloseReason(reason);
  cancel = decoded;
  return true;
}

}  // namespace kcd2s::integration
