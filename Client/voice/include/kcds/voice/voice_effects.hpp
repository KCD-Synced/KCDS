// How a voice sounds where it is: a talker's muffling, and the room's echo
// on the mix. Listener's side only: whether someone is heard at all stays the server's decision.
#ifndef KCDS_VOICE_VOICE_EFFECTS_HPP
#define KCDS_VOICE_VOICE_EFFECTS_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace kcds::voice {

// How a talker's voice is changed, each 0 to 1 unless said otherwise. All move smoothly.
struct TalkerEffect {
  // Through a wall or door: highs go first, then the level.
  float occlusion = 0.0f;
  // A closed helmet, a gag: highs go, the level stays.
  float muffle = 0.0f;
  // Drunk: the pitch wanders a little.
  float wobble = 0.0f;
  // Out of breath or hurt: the voice loses some level.
  float weakness = 0.0f;
  // Under water: nearly only lows, and quieter.
  float underwater = 0.0f;
  // Wind and rain cover a far voice: 0 to 1 of masking.
  float masking = 0.0f;
};

// What the game says of a talker, on the listener's own copy of them.
struct TalkerState {
  // The game's drunkenness, 0 to 1; rpg_param AlcoholDrunkThreshold is where drunk begins.
  float drunkenness = 0.0f;
  // The worst injury, 0 to 1.
  float injury = 0.0f;
  // Stamina left, 0 to 1. Below kOutOfBreath the voice loses some level.
  float stamina = 1.0f;
  bool visor_closed = false;
  bool unconscious = false;
  bool riding = false;
};

inline constexpr float kDrunkThreshold = 0.32f;
inline constexpr float kOutOfBreath = 0.25f;

// Adds the talker's state to their effect. It changes how they sound, never whether they are heard.
inline void apply_talker_state(const TalkerState& state, TalkerEffect& effect) {
  if (state.drunkenness > kDrunkThreshold) {
    const float over = (state.drunkenness - kDrunkThreshold) / (1.0f - kDrunkThreshold);
    effect.wobble = std::fmin(over, 1.0f);
  }
  effect.weakness = std::fmax(effect.weakness, std::fmin(std::fmax(state.injury, 0.0f), 1.0f));
  if (state.stamina < kOutOfBreath) {
    const float spent = (kOutOfBreath - std::fmax(state.stamina, 0.0f)) / kOutOfBreath;
    effect.weakness = std::fmax(effect.weakness, spent * 0.5f);
  }
  if (state.visor_closed) {
    effect.muffle = std::fmax(effect.muffle, 0.6f);
  }
  if (state.unconscious) {
    effect.muffle = 1.0f;
    effect.weakness = 1.0f;
  }
  if (state.riding) {
    effect.masking = std::fmax(effect.masking, 0.25f);
  }
}

// One talker's chain, mono and in place.
class VoiceFilter {
 public:
  void process(float* samples, std::size_t count, const TalkerEffect& effect, float sample_rate) {
    // Each block moves a share of the way, so a change never clicks.
    const float closeness = 0.25f;
    occlusion_ += (effect.occlusion - occlusion_) * closeness;
    muffle_ += (effect.muffle - muffle_) * closeness;
    wobble_ += (effect.wobble - wobble_) * closeness;
    weakness_ += (effect.weakness - weakness_) * closeness;
    underwater_ += (effect.underwater - underwater_) * closeness;
    masking_ += (effect.masking - masking_) * closeness;

    // The cutoff falls with whichever closes the voice most: 16 kHz open, 500 Hz through a wall.
    const float closed = std::fmax(std::fmax(occlusion_, muffle_ * 0.7f), underwater_);
    const float cutoff = 16000.0f * std::pow(500.0f / 16000.0f, closed);
    const float pi = 3.14159265f;
    const float a = 1.0f - std::exp(-2.0f * pi * cutoff / sample_rate);
    const float level = (1.0f - 0.55f * occlusion_) * (1.0f - 0.3f * weakness_) *
                        (1.0f - 0.6f * underwater_) * (1.0f - 0.5f * masking_);
    for (std::size_t i = 0; i < count; ++i) {
      float sample = samples[i];
      if (wobble_ > 0.001f) {
        sample = wander(sample, sample_rate);
      }
      low_ += (sample - low_) * a;
      low2_ += (low_ - low2_) * a;
      samples[i] = low2_ * level;
    }
  }

 private:
  // A slowly moving short delay: the pitch rises and falls with its change.
  float wander(float sample, float sample_rate) {
    delay_line_[write_] = sample;
    phase_ += 2.0f * 3.14159265f * 0.7f / sample_rate;
    if (phase_ > 6.2831853f) {
      phase_ -= 6.2831853f;
    }
    const float depth = 0.004f * sample_rate * wobble_;
    const float delay = depth * (1.0f + std::sin(phase_)) + 1.0f;
    const float where = float(write_) - delay;
    const float wrapped = where < 0.0f ? where + float(kDelay) : where;
    const std::size_t a = std::size_t(wrapped) % kDelay;
    const std::size_t b = (a + 1) % kDelay;
    const float part = wrapped - std::floor(wrapped);
    write_ = (write_ + 1) % kDelay;
    return delay_line_[a] * (1.0f - part) + delay_line_[b] * part;
  }

  static constexpr std::size_t kDelay = 1024;
  std::array<float, kDelay> delay_line_{};
  std::size_t write_ = 0;
  float phase_ = 0.0f;
  float low_ = 0.0f;
  float low2_ = 0.0f;
  float occlusion_ = 0.0f;
  float muffle_ = 0.0f;
  float wobble_ = 0.0f;
  float weakness_ = 0.0f;
  float underwater_ = 0.0f;
  float masking_ = 0.0f;
};

enum class RoomPreset : unsigned char { Outdoors, Hut, Tavern, CastleHall, Church, Dungeon };

struct RoomValues {
  float size = 0.0f;  // the echo's length, 0 to 1
  float damp = 0.5f;  // how quickly the highs die
  float wet = 0.0f;   // how much of it is heard
};

inline RoomValues room_values(RoomPreset preset) {
  switch (preset) {
    case RoomPreset::Hut:
      return RoomValues{0.25f, 0.65f, 0.10f};
    case RoomPreset::Tavern:
      return RoomValues{0.45f, 0.60f, 0.14f};
    case RoomPreset::CastleHall:
      return RoomValues{0.75f, 0.35f, 0.20f};
    case RoomPreset::Church:
      return RoomValues{0.90f, 0.30f, 0.24f};
    case RoomPreset::Dungeon:
      return RoomValues{0.70f, 0.25f, 0.26f};
    case RoomPreset::Outdoors:
      break;
  }
  return RoomValues{};
}

// A plain Schroeder reverb on the stereo mix: 4 combs and 2 all-passes per side. Skips itself
// entirely outdoors, once its tail has died.
class RoomEcho {
 public:
  explicit RoomEcho(float sample_rate) {
    const float scale = sample_rate / 44100.0f;
    constexpr std::array<int, 4> kCombs{1116, 1188, 1277, 1356};
    constexpr std::array<int, 2> kPasses{556, 441};
    for (int side = 0; side < 2; ++side) {
      for (std::size_t i = 0; i < kCombs.size(); ++i) {
        comb_[side][i].line.assign(std::size_t(float(kCombs[i] + side * 23) * scale), 0.0f);
      }
      for (std::size_t i = 0; i < kPasses.size(); ++i) {
        pass_[side][i].line.assign(std::size_t(float(kPasses[i] + side * 23) * scale), 0.0f);
      }
    }
  }

  void set_preset(RoomPreset preset) {
    target_ = room_values(preset);
  }

  // Adds the echo to the stereo mix, in place.
  void process(float* left, float* right, std::size_t count) {
    current_.size += (target_.size - current_.size) * 0.1f;
    current_.damp += (target_.damp - current_.damp) * 0.1f;
    current_.wet += (target_.wet - current_.wet) * 0.1f;
    if (current_.wet < 0.002f && tail_ < 0.0001f) {
      return;
    }
    const float feedback = 0.7f + 0.28f * current_.size;
    float peak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
      const float input = (left[i] + right[i]) * 0.5f * 0.05f;
      float* const out[2] = {&left[i], &right[i]};
      for (int side = 0; side < 2; ++side) {
        float sum = 0.0f;
        for (Comb& comb : comb_[side]) {
          const float held = comb.line[comb.at];
          comb.low += (held - comb.low) * (1.0f - current_.damp);
          comb.line[comb.at] = input + comb.low * feedback;
          comb.at = (comb.at + 1) % comb.line.size();
          sum += held;
        }
        for (Pass& pass : pass_[side]) {
          const float held = pass.line[pass.at];
          pass.line[pass.at] = sum + held * 0.5f;
          pass.at = (pass.at + 1) % pass.line.size();
          sum = held - sum;
        }
        peak = std::fmax(peak, std::fabs(sum));
        *out[side] += sum * current_.wet * 4.0f;
      }
    }
    tail_ = peak;
  }

 private:
  struct Comb {
    std::vector<float> line;
    std::size_t at = 0;
    float low = 0.0f;
  };
  struct Pass {
    std::vector<float> line;
    std::size_t at = 0;
  };
  std::array<std::array<Comb, 4>, 2> comb_{};
  std::array<std::array<Pass, 2>, 2> pass_{};
  RoomValues target_{};
  RoomValues current_{};
  float tail_ = 0.0f;
};

}  // namespace kcds::voice

#endif
