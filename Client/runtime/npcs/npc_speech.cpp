#include "npc_speech.hpp"

#include <cmath>
#include <cstdlib>

#include "../hud/speech_bubbles.hpp"

namespace kcds::runtime::npcs {
namespace {

constexpr float kBubbleDistance = 12.0f;
constexpr float kBubbleMinScale = 0.4f;
constexpr float kBubbleHeight = 2.0f;
// IEntity::GetWorldTM.
constexpr std::size_t kWorldTmSlot = 32;

using WorldTm = const float*(__fastcall*)(void* entity);

}  // namespace

bool take_speech(Speech& speech, const std::string& field, const std::string& speaker, double now) {
  if (field == speech.field) {
    return false;
  }
  speech.field = field;
  end_speech(speech);
  speech.until = 0;
  const std::size_t first = field.find('|');
  const std::size_t second = first == std::string::npos ? first : field.find('|', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const double milliseconds = std::atof(field.substr(first + 1, second - first - 1).c_str());
  speech.until = now + milliseconds / 1000.0;
  speech.text = field.substr(second + 1);
  // The game's own line of the NPC starts with U+2063, a localization key then with U+2064
  // (invisible characters: the field takes no control characters).
  constexpr const char* kNativeMark = "\xE2\x81\xA3";
  constexpr const char* kLocalizeMark = "\xE2\x81\xA4";
  constexpr std::size_t kMarkBytes = 3;
  speech.native = speech.text.rfind(kNativeMark, 0) == 0;
  if (speech.native) {
    speech.text.erase(0, kMarkBytes);
  }
  speech.localize = speech.text.rfind(kLocalizeMark, 0) == 0;
  if (speech.localize) {
    speech.text.erase(0, kMarkBytes);
  }
  // Its voice file and purpose then follow between U+2062 marks: <mark>file<mark>purpose<mark>.
  constexpr const char* kVoiceMark = "\xE2\x81\xA2";
  speech.voice.clear();
  speech.purpose = 0;
  if (speech.native && speech.text.rfind(kVoiceMark, 0) == 0) {
    const std::size_t file_end = speech.text.find(kVoiceMark, kMarkBytes);
    const std::size_t purpose_end = file_end == std::string::npos
                                        ? file_end
                                        : speech.text.find(kVoiceMark, file_end + kMarkBytes);
    if (purpose_end != std::string::npos) {
      speech.voice = speech.text.substr(kMarkBytes, file_end - kMarkBytes);
      const std::string purpose =
          speech.text.substr(file_end + kMarkBytes, purpose_end - file_end - kMarkBytes);
      speech.purpose = static_cast<std::uint32_t>(std::strtoul(purpose.c_str(), nullptr, 10));
      speech.text.erase(0, purpose_end + kMarkBytes);
    }
  }
  speech.speaker = speaker;
  return true;
}

void show_speech(Speech& speech, void* entity, const float* view, double now) {
  // A line said without a bubble (only its voice) shows nothing.
  if (now >= speech.until || !entity || !view || speech.text.empty()) {
    end_speech(speech);
    return;
  }
  const auto world_tm = reinterpret_cast<WorldTm>((*static_cast<void***>(entity))[kWorldTmSlot]);
  const float* tm = world_tm(entity);
  const float position[3] = {tm[3], tm[7], tm[11] + kBubbleHeight};
  const float dx = position[0] - view[3];
  const float dy = position[1] - view[7];
  const float dz = position[2] - view[11];
  const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (distance > kBubbleDistance) {
    end_speech(speech);
    return;
  }
  if (!speech.bubble) {
    speech.bubble = kcds::runtime::hud::create_bubble(speech.speaker, speech.text, speech.localize);
    // The NPC's own line shows as a subtitle as well, as the game shows it to the player it spoke
    // to (the game shows it only while the player has subtitles on).
    if (speech.native && speech.subtitle_wanted && !speech.subtitled) {
      speech.subtitled =
          kcds::runtime::hud::show_subtitle(speech.speaker, speech.text, speech.localize);
    }
  }
  const float scale = 1.0f - (1.0f - kBubbleMinScale) * distance / kBubbleDistance;
  kcds::runtime::hud::place_bubble(speech.bubble, position, scale);
}

void end_speech(Speech& speech) {
  if (speech.bubble) {
    kcds::runtime::hud::release_bubble(speech.bubble);
    speech.bubble = 0;
  }
  if (speech.subtitled) {
    kcds::runtime::hud::clear_subtitle();
    speech.subtitled = false;
  }
}

}  // namespace kcds::runtime::npcs
