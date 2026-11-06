#include "spatial.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <memory>
#include <string>
#include <type_traits>
#include "phonon.h"
#endif

namespace kcds::voice {
namespace {

constexpr float kPi = 3.14159265f;

Vec3 cross(const Vec3& a, const Vec3& b) {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

// A small binaural: the far ear hears later and softer, with its highs rolled off.
class OwnBinaural final : public Spatializer {
 public:
  void process(const float* mono, const Vec3& direction, float gain, float* left,
               float* right) override {
    // Woodworth's head model: sideways is about 0.66 ms between the ears.
    const float side = std::clamp(direction.x, -1.0f, 1.0f);
    const float angle = std::asin(side);
    const float seconds = 0.0875f / 343.0f * (std::fabs(angle) + std::sin(std::fabs(angle)));
    const float target_delay = seconds * float(kSampleRate);
    // Far ear: -6 dB straight to the side, and a low-pass closing with the angle.
    const float far_gain = 1.0f - 0.5f * std::fabs(side);
    const float cut = 0.15f + 0.85f * (1.0f - std::fabs(side));
    for (std::size_t i = 0; i < kBlockSamples; ++i) {
      const float t = float(i) / float(kBlockSamples);
      const float delay = delay_ + (target_delay - delay_) * t;
      const float level = gain_ + (gain - gain_) * t;
      const float scale = far_ + (far_gain - far_) * t;
      history_[position_] = mono[i];
      const float near_sample = mono[i] * level;
      const float where = float(position_) - delay;
      const int whole = int(std::floor(where));
      const float part = where - float(whole);
      const std::size_t a = std::size_t((whole + int(kHistory)) % int(kHistory));
      const std::size_t b = (a + 1) % kHistory;
      const float delayed = history_[a] * (1.0f - part) + history_[b] * part;
      low_ += (delayed - low_) * cut;
      const float far_sample = low_ * level * scale;
      left[i] += side > 0.0f ? far_sample : near_sample;
      right[i] += side > 0.0f ? near_sample : far_sample;
      position_ = (position_ + 1) % kHistory;
    }
    delay_ = target_delay;
    gain_ = gain;
    far_ = far_gain;
  }

 private:
  static constexpr std::size_t kHistory = 128;
  std::array<float, kHistory> history_{};
  std::size_t position_ = 0;
  float delay_ = 0.0f;
  float gain_ = 0.0f;
  float far_ = 1.0f;
  float low_ = 0.0f;
};

class OwnFactory final : public SpatialFactory {
 public:
  std::unique_ptr<Spatializer> make() override {
    return std::make_unique<OwnBinaural>();
  }
  bool is_hrtf() const override {
    return false;
  }
};

#ifdef _WIN32
// Steam Audio is loaded by hand: without phonon.dll beside the client, voice still works.
struct Phonon {
  HMODULE module = nullptr;
  decltype(&iplContextCreate) context_create = nullptr;
  decltype(&iplContextRelease) context_release = nullptr;
  decltype(&iplHRTFCreate) hrtf_create = nullptr;
  decltype(&iplHRTFRelease) hrtf_release = nullptr;
  decltype(&iplBinauralEffectCreate) effect_create = nullptr;
  decltype(&iplBinauralEffectRelease) effect_release = nullptr;
  decltype(&iplBinauralEffectApply) effect_apply = nullptr;

  bool load() {
    // Beside this DLL, not beside the game: the client's folder is ours.
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&make_spatial_factory), &self);
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
      return false;
    }
    std::wstring beside(path, length);
    const auto slash = beside.find_last_of(L"\/");
    beside = beside.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"phonon.dll";
    module = LoadLibraryExW(beside.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
      return false;
    }
    const auto find = [this](auto& target, const char* name) {
      target =
          reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(module, name));
      return target != nullptr;
    };
    return find(context_create, "iplContextCreate") && find(context_release, "iplContextRelease") &&
           find(hrtf_create, "iplHRTFCreate") && find(hrtf_release, "iplHRTFRelease") &&
           find(effect_create, "iplBinauralEffectCreate") &&
           find(effect_release, "iplBinauralEffectRelease") &&
           find(effect_apply, "iplBinauralEffectApply");
  }
};

class SteamBinaural final : public Spatializer {
 public:
  SteamBinaural(const Phonon& phonon, IPLContext context, IPLHRTF hrtf)
      : phonon_(phonon), hrtf_(hrtf) {
    IPLAudioSettings audio{};
    audio.samplingRate = int(kSampleRate);
    audio.frameSize = int(kBlockSamples);
    IPLBinauralEffectSettings settings{};
    settings.hrtf = hrtf;
    phonon_.effect_create(context, &audio, &settings, &effect_);
  }
  ~SteamBinaural() override {
    if (effect_) {
      phonon_.effect_release(&effect_);
    }
  }
  bool ready() const {
    return effect_ != nullptr;
  }
  void process(const float* mono, const Vec3& direction, float gain, float* left,
               float* right) override {
    float* in_channels[1] = {in_.data()};
    float* out_channels[2] = {out_left_.data(), out_right_.data()};
    for (std::size_t i = 0; i < kBlockSamples; ++i) {
      const float t = float(i) / float(kBlockSamples);
      in_[i] = mono[i] * (gain_ + (gain - gain_) * t);
    }
    IPLAudioBuffer in{1, int(kBlockSamples), in_channels};
    IPLAudioBuffer out{2, int(kBlockSamples), out_channels};
    IPLBinauralEffectParams params{};
    params.direction = IPLVector3{direction.x, direction.y, direction.z};
    params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    params.spatialBlend = 1.0f;
    params.hrtf = hrtf_;
    phonon_.effect_apply(effect_, &params, &in, &out);
    for (std::size_t i = 0; i < kBlockSamples; ++i) {
      left[i] += out_left_[i];
      right[i] += out_right_[i];
    }
    gain_ = gain;
  }

 private:
  const Phonon& phonon_;
  IPLHRTF hrtf_ = nullptr;
  IPLBinauralEffect effect_ = nullptr;
  std::array<float, kBlockSamples> in_{};
  std::array<float, kBlockSamples> out_left_{};
  std::array<float, kBlockSamples> out_right_{};
  float gain_ = 0.0f;
};

class SteamFactory final : public SpatialFactory {
 public:
  bool start() {
    if (!phonon_.load()) {
      return false;
    }
    IPLContextSettings settings{};
    settings.version = STEAMAUDIO_VERSION;
    if (phonon_.context_create(&settings, &context_) != IPL_STATUS_SUCCESS) {
      return false;
    }
    IPLAudioSettings audio{};
    audio.samplingRate = int(kSampleRate);
    audio.frameSize = int(kBlockSamples);
    IPLHRTFSettings hrtf{};
    hrtf.type = IPL_HRTFTYPE_DEFAULT;
    hrtf.volume = 1.0f;
    return phonon_.hrtf_create(context_, &audio, &hrtf, &hrtf_) == IPL_STATUS_SUCCESS;
  }
  ~SteamFactory() override {
    if (hrtf_) {
      phonon_.hrtf_release(&hrtf_);
    }
    if (context_) {
      phonon_.context_release(&context_);
    }
  }
  std::unique_ptr<Spatializer> make() override {
    auto effect = std::make_unique<SteamBinaural>(phonon_, context_, hrtf_);
    if (effect->ready()) {
      return effect;
    }
    return std::make_unique<OwnBinaural>();
  }
  bool is_hrtf() const override {
    return true;
  }

 private:
  Phonon phonon_;
  IPLContext context_ = nullptr;
  IPLHRTF hrtf_ = nullptr;
};
#endif

}  // namespace

Vec3 direction_to(const Vec3& listener, const Vec3& forward, const Vec3& up, const Vec3& point) {
  Vec3 way{point.x - listener.x, point.y - listener.y, point.z - listener.z};
  const float length = std::sqrt(dot(way, way));
  if (length < 0.001f) {
    return Vec3{0.0f, 0.0f, -1.0f};
  }
  way = Vec3{way.x / length, way.y / length, way.z / length};
  const Vec3 right = cross(forward, up);
  // Steam Audio's frame: x right, y up, z backward.
  return Vec3{dot(way, right), dot(way, up), -dot(way, forward)};
}

float distance_gain(float distance, float range) {
  if (range <= 0.0f || distance >= range) {
    return 0.0f;
  }
  const float near_meters = std::min(1.0f, range * 0.25f);
  if (distance <= near_meters) {
    return 1.0f;
  }
  const float t = (distance - near_meters) / (range - near_meters);
  // An ear hears about 1/distance; the last part of the range fades it fully out.
  const float falloff = near_meters / (near_meters + (distance - near_meters) * 0.6f);
  const float edge = 1.0f - t * t * t;
  return falloff * edge;
}

std::unique_ptr<SpatialFactory> make_spatial_factory(bool allow_hrtf) {
#ifdef _WIN32
  if (allow_hrtf) {
    auto steam = std::make_unique<SteamFactory>();
    if (steam->start()) {
      return steam;
    }
  }
#else
  (void)allow_hrtf;
#endif
  return std::make_unique<OwnFactory>();
}

}  // namespace kcds::voice
