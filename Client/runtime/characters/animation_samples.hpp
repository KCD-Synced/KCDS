// Read-only comparisons between explicit native diagnostic samples, never a frame update.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace kcds::runtime::characters {

inline constexpr std::size_t kMaximumAnimationSamples = 4096;

struct AnimationLayerSample {
  std::uintptr_t entry = 0;
  int clip = -1;
  float progress = -1;

  bool valid() const {
    return entry != 0 && clip >= 0 && std::isfinite(progress) && progress >= 0 && progress <= 1;
  }
};

struct AnimationBodySample {
  std::uint32_t engine_id = 0;
  std::uintptr_t entity = 0;
  std::uintptr_t character = 0;
  std::uintptr_t skeleton = 0;
  std::array<AnimationLayerSample, 16> layers;
};

struct AnimationComparison {
  unsigned comparable_layers = 0;
  unsigned progressing_layers = 0;
  bool body_comparable = false;
  bool body_progressing = false;
};

class AnimationSamples {
 public:
  void begin(const void* entity_system) {
    current_.clear();
    if (!entity_system || entity_system != entity_system_) {
      previous_.clear();
    }
    entity_system_ = entity_system;
  }

  std::optional<AnimationComparison> observe(const AnimationBodySample& sample) {
    if (!sample.engine_id || !sample.entity || !sample.character || !sample.skeleton ||
        current_.size() == kMaximumAnimationSamples || current_.contains(sample.engine_id)) {
      return std::nullopt;
    }
    AnimationComparison result;
    const auto found = previous_.find(sample.engine_id);
    if (found != previous_.end() && found->second.entity == sample.entity &&
        found->second.character == sample.character && found->second.skeleton == sample.skeleton) {
      for (unsigned layer = 0; layer < sample.layers.size(); ++layer) {
        const auto& before = found->second.layers[layer];
        const auto& now = sample.layers[layer];
        if (!before.valid() || !now.valid() || before.entry != now.entry ||
            before.clip != now.clip) {
          continue;
        }
        ++result.comparable_layers;
        const bool progressing = std::abs(before.progress - now.progress) > 0.00001f;
        if (progressing) {
          ++result.progressing_layers;
        }
        if (layer == 0) {
          result.body_comparable = true;
          result.body_progressing = progressing;
        }
      }
    }
    current_.emplace(sample.engine_id, sample);
    return result;
  }

  void finish() {
    previous_.swap(current_);
    current_.clear();
  }

  std::size_t retained() const {
    return previous_.size();
  }

 private:
  const void* entity_system_ = nullptr;
  std::unordered_map<std::uint32_t, AnimationBodySample> previous_;
  std::unordered_map<std::uint32_t, AnimationBodySample> current_;
};

}  // namespace kcds::runtime::characters
