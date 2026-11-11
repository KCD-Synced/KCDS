#include "item_icons.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../inventory/item_types.hpp"
#include "../../native_ui/native_ui.hpp"
#include "dds_picture.hpp"
#include "kcds/web/web.hpp"
#include "png_file.hpp"

namespace kcds::runtime::web::icons {
namespace {

namespace views = kcds::web;

constexpr std::size_t kMaxWaiting = 512;
// Loads answered per frame: a page of a few hundred icons fills in over a few frames.
constexpr int kAnswersPerFrame = 16;
// Pictures kept by icon name (an icon is about 16 KB as a .png).
constexpr std::size_t kMaxKept = 1024;
constexpr char kItemPrefix[] = "item/";
constexpr char kPngSuffix[] = ".png";
constexpr char kItemIconRoot[] = "Libs/UI/Textures/Icons/Items/";

std::mutex waiting_mutex;
std::deque<std::pair<int, std::string>> waiting;

// Engine thread only.
std::unordered_map<std::string, std::string> kept;

// The item named by a load's path, "" when the path is not an item's.
std::string item_of(const std::string& path) {
  const std::string prefix = kItemPrefix;
  const std::string suffix = kPngSuffix;
  if (path.size() <= prefix.size() + suffix.size() || path.compare(0, prefix.size(), prefix) != 0 ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return std::string();
  }
  const std::string item = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (item.find('/') != std::string::npos || item.find('\\') != std::string::npos) {
    return std::string();
  }
  return item;
}

// The icon's .png, "" when the item or its icon is unknown.
std::string icon_png(const std::string& item) {
  const std::string icon = inventory::item_icon_name(item);
  if (icon.empty() || icon.find('/') != std::string::npos || icon.find("..") != std::string::npos) {
    return std::string();
  }
  const auto known = kept.find(icon);
  if (known != kept.end()) {
    return known->second;
  }
  std::vector<std::uint8_t> file;
  Picture picture;
  if (!native_ui::game_file(kItemIconRoot + icon + "_icon.dds", file) ||
      !decode_dds(file, picture)) {
    return std::string();
  }
  const std::vector<std::uint8_t> png = png_file(picture);
  std::string bytes(png.begin(), png.end());
  if (kept.size() >= kMaxKept) {
    kept.clear();
  }
  kept.emplace(icon, bytes);
  return bytes;
}

}  // namespace

void queue_request(int id, const std::string& path) {
  {
    std::lock_guard<std::mutex> lock(waiting_mutex);
    if (waiting.size() < kMaxWaiting) {
      waiting.emplace_back(id, path);
      return;
    }
  }
  views::complete_file(id, false, std::string());
}

void answer_requests() {
  for (int answered = 0; answered < kAnswersPerFrame; ++answered) {
    std::pair<int, std::string> request;
    {
      std::lock_guard<std::mutex> lock(waiting_mutex);
      if (waiting.empty()) {
        return;
      }
      request = std::move(waiting.front());
      waiting.pop_front();
    }
    const std::string item = item_of(request.second);
    std::string png = item.empty() ? std::string() : icon_png(item);
    const bool found = !png.empty();
    views::complete_file(request.first, found, std::move(png));
  }
}

}  // namespace kcds::runtime::web::icons
