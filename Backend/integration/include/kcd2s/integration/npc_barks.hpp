#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// What a server NPC says on its own (its barks, its lines in a fight), seen by every player
// (only the player its simulator's game showed it to saw the bubble). On the control
// channel (0).
//
//   NPC_BARK : the NPC's simulator -> server, once per line the game says by that NPC (its
//              bubble over the head, its voice, or both):
//              [tag][varuint npc][u8 localize][varuint n][n text][varuint v][v voice]
//              [varuint purpose]
//              `localize`: the text is the game's localization key (each player's game shows it
//              in their language). `voice`: the spoken file the game played on that NPC with the
//              line (empty: none), and `purpose` the state of the game's standalone_file_purpose
//              switch it played it with (how far the line carries). The server takes it only
//              from the NPC's simulator; the resource host puts it in the NPC's speech field (as
//              Npc.say), which every client shows, and every client but the simulator plays the
//              voice on its copy of the NPC.
namespace kcd2s::integration {

inline constexpr std::uint8_t kNpcBarkTag = 146;
inline constexpr std::size_t kMaxNpcBarkBytes = 240;
// The longest voice file path (game-root relative, without extension).
inline constexpr std::size_t kMaxNpcVoiceBytes = 160;
// Barks a simulator may report in a second.
inline constexpr unsigned kMaxNpcBarksPerSecond = 8;

struct NpcBark {
  std::uint32_t npc = 0;
  bool localize = false;
  std::string text;
  std::string voice;
  std::uint32_t purpose = 0;
  bool operator==(const NpcBark&) const = default;
};

// No control characters.
inline bool npc_bark_printable(const std::string& text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < 0x20) {
      return false;
    }
  }
  return true;
}

// A voice file: a game-root relative path of letters, digits, '_', '-', '.' and '/', not
// climbing out of the game folder; empty for none.
inline bool npc_voice_valid(const std::string& voice) {
  if (voice.size() > kMaxNpcVoiceBytes || voice.find("..") != std::string::npos ||
      (!voice.empty() && voice.front() == '/')) {
    return false;
  }
  for (const char c : voice) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_' && c != '-' && c != '.' && c != '/') {
      return false;
    }
  }
  return true;
}

// Some text of at most kMaxNpcBarkBytes without control characters, and a valid voice; one of
// them at least (a line the game says without a bubble has only its voice).
inline bool npc_bark_valid(const NpcBark& bark) {
  if (bark.npc == 0 || (bark.text.empty() && bark.voice.empty()) ||
      bark.text.size() > kMaxNpcBarkBytes) {
    return false;
  }
  return npc_bark_printable(bark.text) && npc_voice_valid(bark.voice);
}

inline std::vector<std::uint8_t> encode_npc_bark(const NpcBark& bark) {
  if (!npc_bark_valid(bark)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kNpcBarkTag, 8);
  w.write_varuint(bark.npc);
  w.write_bits(bark.localize ? 1 : 0, 8);
  w.write_varuint(bark.text.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(bark.text.data()), bark.text.size());
  w.write_varuint(bark.voice.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(bark.voice.data()), bark.voice.size());
  w.write_varuint(bark.purpose);
  return w.bytes();
}

inline bool decode_npc_bark(const std::vector<std::uint8_t>& p, NpcBark& bark) {
  if (p.empty() || p[0] != kNpcBarkTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  NpcBark decoded;
  const std::uint64_t npc = r.read_varuint();
  const std::uint64_t localize = r.read_bits(8);
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || npc == 0 || npc > 0xFFFFFFFFu || localize > 1 || length > kMaxNpcBarkBytes) {
    return false;
  }
  decoded.text.resize(length);
  for (char& c : decoded.text) {
    c = char(r.read_bits(8));
  }
  const std::uint64_t voice_length = r.read_varuint();
  if (r.overrun() || voice_length > kMaxNpcVoiceBytes) {
    return false;
  }
  decoded.voice.resize(voice_length);
  for (char& c : decoded.voice) {
    c = char(r.read_bits(8));
  }
  const std::uint64_t purpose = r.read_varuint();
  if (purpose > 0xFFFFFFFFu) {
    return false;
  }
  decoded.purpose = std::uint32_t(purpose);
  decoded.npc = std::uint32_t(npc);
  decoded.localize = localize == 1;
  if (r.overrun() || r.bits_remaining() != 0 || !npc_bark_valid(decoded)) {
    return false;
  }
  bark = decoded;
  return true;
}

}  // namespace kcd2s::integration
