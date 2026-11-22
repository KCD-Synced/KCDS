#include "joint_fold.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <utility>

namespace kcds::runtime::appearance::refit {
namespace {

constexpr std::size_t kBoneSize = 584;
constexpr std::size_t kBoneName = 312;
constexpr std::size_t kBoneNameSize = 256;
constexpr std::size_t kBoneMapSize = 12;
constexpr std::size_t kInternalSize = 64;
constexpr std::size_t kInternalJoints = 36;
constexpr std::size_t kInternalWeights = 44;

// A vertex's four (joint, weight) pairs with the folded joints given to their targets: weights of
// one target added up, the heaviest four kept, heaviest first.
template <class Weight>
void refold(std::array<std::uint16_t, 4>& joints, std::array<Weight, 4>& weights,
            const std::vector<std::uint16_t>& target) {
  std::map<std::uint16_t, Weight> summed;
  for (std::size_t k = 0; k < 4; ++k) {
    if (weights[k] > Weight(0) && joints[k] < target.size()) {
      summed[target[joints[k]]] += weights[k];
    }
  }
  std::vector<std::pair<std::uint16_t, Weight>> ordered(summed.begin(), summed.end());
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  for (std::size_t k = 0; k < 4; ++k) {
    joints[k] = k < ordered.size() ? ordered[k].first : 0;
    weights[k] = k < ordered.size() ? ordered[k].second : Weight(0);
  }
}

}  // namespace

std::uint32_t crc32_of(const std::string& text) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char c : text) {
    crc ^= std::uint8_t(c);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

std::vector<std::uint8_t> fold(const std::vector<std::uint8_t>& bytes, const SkinFile& skin,
                               const std::vector<Joint>& her_joints) {
  std::unordered_set<std::string> hers;
  for (const Joint& joint : her_joints) {
    hers.insert(joint.name);
  }
  std::unordered_set<std::string> named;
  for (const Joint& joint : skin.joints) {
    named.insert(joint.name);
  }
  // Each joint's target: itself when she has it, else its nearest ancestor she has (the root
  // when none is).
  std::vector<std::uint16_t> target(skin.joints.size(), 0);
  std::vector<std::size_t> foreign;
  for (std::size_t index = 0; index < skin.joints.size(); ++index) {
    int at = int(index);
    while (at >= 0 && !hers.count(skin.joints[std::size_t(at)].name)) {
      at = skin.joints[std::size_t(at)].parent;
    }
    target[index] = std::uint16_t(at >= 0 ? at : 0);
    // The root keeps its name: a player woman's skeleton takes the male root's name, so men's
    // skins find their root on her.
    if (index != 0 && !hers.count(skin.joints[index].name)) {
      foreign.push_back(index);
    }
  }
  if (foreign.empty() || skin.joints_offset == 0) {
    return {};
  }
  // New names: her joints the skin does not name, in her skeleton's order.
  std::vector<std::string> free_names;
  for (const Joint& joint : her_joints) {
    if (!named.count(joint.name) && joint.name.size() < kBoneNameSize) {
      free_names.push_back(joint.name);
    }
  }
  if (free_names.size() < foreign.size()) {
    return {};
  }
  std::vector<std::uint8_t> copy = bytes;
  for (std::size_t n = 0; n < foreign.size(); ++n) {
    const std::size_t record = skin.joints_offset + foreign[n] * kBoneSize;
    if (record + kBoneSize > copy.size()) {
      return {};
    }
    const std::string& name = free_names[n];
    std::memset(&copy[record + kBoneName], 0, kBoneNameSize);
    std::memcpy(&copy[record + kBoneName], name.data(), name.size());
    const std::uint32_t id = crc32_of(name);
    std::memcpy(&copy[record], &id, sizeof(id));
  }
  // The bone map: four u16 joints, then four u8 weights.
  if (skin.bone_map_offset != 0) {
    for (std::size_t vertex = 0; vertex < skin.influences.size(); ++vertex) {
      const std::size_t at = skin.bone_map_offset + vertex * kBoneMapSize;
      std::array<std::uint16_t, 4> joints{};
      std::array<int, 4> weights{};
      for (std::size_t k = 0; k < 4; ++k) {
        std::memcpy(&joints[k], &copy[at + k * 2], 2);
        weights[k] = copy[at + 8 + k];
      }
      refold(joints, weights, target);
      for (std::size_t k = 0; k < 4; ++k) {
        std::memcpy(&copy[at + k * 2], &joints[k], 2);
        copy[at + 8 + k] = std::uint8_t(std::min(weights[k], 255));
      }
    }
  }
  // The internal skin vertices: four u16 joints at + 36, four float weights at + 44.
  for (std::size_t vertex = 0; vertex < skin.internal_count; ++vertex) {
    const std::size_t at = skin.internal_offset + vertex * kInternalSize;
    if (at + kInternalSize > copy.size()) {
      break;
    }
    std::array<std::uint16_t, 4> joints{};
    std::array<float, 4> weights{};
    std::memcpy(joints.data(), &copy[at + kInternalJoints], sizeof(joints));
    std::memcpy(weights.data(), &copy[at + kInternalWeights], sizeof(weights));
    refold(joints, weights, target);
    std::memcpy(&copy[at + kInternalJoints], joints.data(), sizeof(joints));
    std::memcpy(&copy[at + kInternalWeights], weights.data(), sizeof(weights));
  }
  return copy;
}

}  // namespace kcds::runtime::appearance::refit
