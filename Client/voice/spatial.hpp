// Where a voice sounds from: Steam Audio's HRTF when its library is beside the client, else a small
// binaural of our own (a delay and a soft far ear), so a missing file only lowers the quality.
#ifndef KCDS_VOICE_SPATIAL_HPP
#define KCDS_VOICE_SPATIAL_HPP

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "kcds/voice/voice_engine.hpp"

namespace kcds::voice {

constexpr std::size_t kSampleRate = 48000;
constexpr std::size_t kBlockSamples = 960;  // 20 ms

// The direction of a point from the listener, in the listener's frame: x right, y up, z behind.
Vec3 direction_to(const Vec3& listener, const Vec3& forward, const Vec3& up, const Vec3& point);

// How loud a voice is at a distance within its range: full close by, then falling, and silent at
// the range itself, so the end of hearing never cuts off suddenly.
float distance_gain(float distance, float range);

// One talker's way to the two ears.
class Spatializer {
 public:
  virtual ~Spatializer() = default;
  // Adds the mono block to the stereo mix (kBlockSamples each), from `direction` and scaled by
  // gain.
  virtual void process(const float* mono, const Vec3& direction, float gain, float* left,
                       float* right) = 0;
};

class SpatialFactory {
 public:
  virtual ~SpatialFactory() = default;
  virtual std::unique_ptr<Spatializer> make() = 0;
  virtual bool is_hrtf() const = 0;
};

// Steam Audio when `allow_hrtf` and its library loads; otherwise the binaural of our own.
std::unique_ptr<SpatialFactory> make_spatial_factory(bool allow_hrtf);

}  // namespace kcds::voice

#endif
