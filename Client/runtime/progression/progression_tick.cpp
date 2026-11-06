#include "progression_tick.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "perk_gate.hpp"
#include "progression_orders.hpp"
#include "progression_report.hpp"
#include "replica_soul.hpp"
#include "xp_gate.hpp"

#include <fstream>
#include <iterator>
#include <vector>

namespace kcds::game_client {

namespace sdk = kcd2s::sdk;

std::uint64_t* __fastcall test_soul_id(TestSoulSource* source) {
  return &source->soul_id;
}
void* const test_soul_vtable[] = {reinterpret_cast<void*>(test_soul_id)};
ScreenLearnPerk screen_learn_perk = nullptr;

// The learn-perk test hook (KCDS_TEST_HOOKS only), on the local player's soul.
void run_learn_perk_test(void* soul) {
  if (!signaled(learn_perk_event) || !soul || !screen_learn_perk) {
    return;
  }
  wchar_t folder[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH + 1, folder);
  wchar_t file[MAX_PATH + 64];
  swprintf_s(file, L"%skcds-learn-%lu.txt", folder, GetCurrentProcessId());
  std::ifstream input(file, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  // The game loads GUIDs with movaps: 16-byte aligned.
  alignas(16) kcd2s::integration::PerkId perk{};
  if (!kcd2s::integration::parse_perk_id(text, perk)) {
    log("PROBE learn perk: not a GUID");
    return;
  }
  TestSoulSource source{test_soul_vtable, 0};
  std::memcpy(&source.soul_id, static_cast<std::uint8_t*>(soul) + 48, 8);
  screen_learn_perk(nullptr, &source, &perk, 1);
  log("PROBE learn perk %s confirmed", text.c_str());
}

// Applies the server's progression orders to the local player.
void tick_progression_orders(void* local_actor) {
  void* soul = multiplayer_world.load() && local_actor
                   ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640)
                   : nullptr;
  std::vector<kcd2s::integration::ProgressionOrder> orders;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    orders = client->peer().take_progression_orders();
  }
  namespace perk_gate = kcds::runtime::progression::perk_gate;
  namespace sdk = kcd2s::sdk;
  perk_gate::tick(soul);
  run_learn_perk_test(soul);
  kcds::runtime::progression::apply(soul, orders);
  // XP from play and perk-screen perks wait for the server (the gates held them back while the
  // game applied effects).
  kcds::runtime::progression::xp_gate::tick(soul, kcds::runtime::progression::native_xp());
  const auto gains = kcds::runtime::progression::xp_gate::take_gains();
  const auto perks = perk_gate::take_requests();
  const auto refusals = perk_gate::take_refusals();
  if (gains.empty() && perks.empty() && refusals.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  for (const auto& gain : gains) {
    client->peer().send_xp_gaining(gain);
  }
  for (const auto& perk : perks) {
    client->peer().send_perk_learning(perk);
  }
  // The perk screen says nothing of a refusal: the resources do (progressionPerkRefused).
  static constexpr const char* kRefusals[] = {"points",  "level",  "parent", "exclusive",
                                              "blocked", "script", "owned",  "hidden"};
  for (const auto& [perk, refusal] : refusals) {
    client->send_to_resource_runtime(sdk::Value(
        sdk::Value::Map{{"op", sdk::Value(std::string("progressionPerkRefused"))},
                        {"perk", sdk::Value(kcd2s::integration::perk_id_text(perk))},
                        {"reason", sdk::Value(std::string(kRefusals[std::size_t(refusal)]))}}));
  }
}

// Reports the local player's progression to the server when it changed,
// read every 250 ms.
void tick_progression(void* local_actor) {
  constexpr ULONGLONG kProgressionPeriodMs = 250;
  static ULONGLONG read_at = 0;
  const ULONGLONG now = GetTickCount64();
  if (!multiplayer_world.load() || !local_actor || now - read_at < kProgressionPeriodMs) {
    return;
  }
  read_at = now;
  void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640);
  kcd2s::integration::Progression progression;
  if (!kcds::runtime::progression::read(soul, progression)) {
    return;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (client->peer().send_progression(progression)) {
    log("PROGRESSION reported perks=%zu", progression.perks.size());
  }
}

// The local player's replicaSoul report: its levels and directly owned perks, which every other
// client gives its replica, read every 250 ms.
const std::string& tick_replica_soul(void* local_actor) {
  constexpr ULONGLONG kReadPeriodMs = 250;
  static ULONGLONG read_at = 0;
  static std::string report;
  const ULONGLONG now = GetTickCount64();
  if (!local_actor || now - read_at < kReadPeriodMs) {
    return report;
  }
  read_at = now;
  void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 1640);
  kcd2s::integration::ReplicaSoul soul_state;
  if (kcds::runtime::progression::replica_soul::read(soul, soul_state)) {
    std::string read_now = kcd2s::integration::replica_soul_field(soul_state);
    if (read_now != report) {
      report = std::move(read_now);
      log("REPLICA SOUL reported strength=%u sword=%u perks=%zu", unsigned(soul_state.levels[15]),
          unsigned(soul_state.levels[8]), soul_state.perks.size());
    }
  }
  return report;
}

}  // namespace kcds::game_client
