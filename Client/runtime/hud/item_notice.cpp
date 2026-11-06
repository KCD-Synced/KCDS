#include "item_notice.hpp"

#include <cstring>

namespace kcds::runtime::hud {
namespace {

using GameServices = void*(__fastcall*)();
using ShowItemTransfer = void(__fastcall*)(void* transfer, const std::uint8_t* item, int amount,
                                           std::uint8_t direction);

// The game's services: the UI; the UI's item transfer.
constexpr std::size_t kServicesUi = 224;
constexpr std::size_t kUiItemTransfer = 376;
constexpr std::uint8_t kReceived = 0;
constexpr std::uint8_t kGiven = 1;

GameServices game_services = nullptr;

void* field(void* object, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, static_cast<std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

}  // namespace

bool install_item_notice(void* services) {
  game_services = reinterpret_cast<GameServices>(services);
  return game_services != nullptr;
}

bool show_item_notice(const std::array<std::uint8_t, 16>& item, std::uint32_t amount, bool gained) {
  void* services = game_services ? game_services() : nullptr;
  void* ui = services ? field(services, kServicesUi) : nullptr;
  void* transfer = ui ? field(ui, kUiItemTransfer) : nullptr;
  if (!transfer || amount == 0) {
    return false;
  }
  alignas(16) std::uint8_t guid[16];
  std::memcpy(guid, item.data(), sizeof(guid));
  const auto show = reinterpret_cast<ShowItemTransfer>((*static_cast<void***>(transfer))[0]);
  show(transfer, guid, int(amount), gained ? kReceived : kGiven);
  return true;
}

}  // namespace kcds::runtime::hud
