#include "studio_grid.hpp"

#include <atomic>

#include "kcd2s/integration/script_channel.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::grid {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// The stage's middle (studio_scene kStage) and the floor's height just above the studio's floor.
constexpr float kCentreX = 32.0f;
constexpr float kCentreY = 32.0f;
constexpr float kHeight = 32.025f;
constexpr int kHalfLines = 10;
constexpr float kSpacing = 1.0f;
constexpr float kGridColour[4] = {0.85f, 0.88f, 0.92f, 0.8f};
constexpr float kGridThickness = 1.0f;
constexpr float kAxisThickness = 2.0f;

std::atomic<bool> showing{false};

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "studioGrid") {
    return false;
  }
  const sdk::Value* on = integration::script_field(message, "on");
  showing.store(on && on->tag() == sdk::Tag::Bool && on->as_bool());
  return true;
}

void tick() {
  view::View shown;
  if (!showing.load() || !view::current(shown)) {
    return;
  }
  const float reach = kHalfLines * kSpacing;
  for (int line = -kHalfLines; line <= kHalfLines; ++line) {
    const float offset = line * kSpacing;
    // Along y at this x, then along x at this y; the lines through the stage are the axes.
    const float along_y_from[3] = {kCentreX + offset, kCentreY - reach, kHeight};
    const float along_y_to[3] = {kCentreX + offset, kCentreY + reach, kHeight};
    const float along_x_from[3] = {kCentreX - reach, kCentreY + offset, kHeight};
    const float along_x_to[3] = {kCentreX + reach, kCentreY + offset, kHeight};
    // Hidden behind the character and the props, as a grid on the floor is.
    const bool over = false;
    if (line == 0) {
      view::clipped_line(shown, along_y_from, along_y_to, view::kAxisColours[1], kAxisThickness,
                         over);
      view::clipped_line(shown, along_x_from, along_x_to, view::kAxisColours[0], kAxisThickness,
                         over);
    } else {
      view::clipped_line(shown, along_y_from, along_y_to, kGridColour, kGridThickness, over);
      view::clipped_line(shown, along_x_from, along_x_to, kGridColour, kGridThickness, over);
    }
  }
}

}  // namespace kcds::runtime::studio::grid
