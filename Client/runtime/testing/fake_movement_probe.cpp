#include "../core/client_internal.hpp"

#include "fake_movement_probe.hpp"

#include <cstdint>
#include <unordered_map>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::testing::fake_movement {
namespace {

using kcds::game_client::image;
using kcds::game_client::image_size;
using kcds::game_client::log;
using kcds::game_client::resolve_by_vtable_class;
using kcds::game_client::resolve_unique;

// The manager's constructor stores its vtable first; slot 0 is the request.
constexpr char kClass[] = ".?AVC_FakeMovementManager@navigation@xgenaimodule@wh@@";
constexpr char kConstructor[] = "40 53 48 83 EC 40 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01";
constexpr std::size_t kRequestSlot = 0;
// The game's frames told for each request, past this probe's own.
constexpr unsigned kFrames = 10;
// Each entity's first requests are told, then every this many.
constexpr unsigned kToldFirst = 3;
constexpr unsigned kToldEvery = 50;

using Request = char(__fastcall*)(void* manager, unsigned entity, void* request, void* done,
                                  void* out, char flag);
Request original_request = nullptr;
std::unordered_map<unsigned, unsigned> requests;

// A frame as its address in the database of WHGame.dll, 0 outside it.
std::uint64_t game_address(void* frame) {
  auto* at = static_cast<std::uint8_t*>(frame);
  if (!image || at < image || at >= image + image_size) {
    return 0;
  }
  return 0x180000000ull + std::uint64_t(at - image);
}

char __fastcall probed_request(void* manager, unsigned entity, void* request, void* done, void* out,
                               char flag) {
  const unsigned count = ++requests[entity];
  if (count <= kToldFirst || count % kToldEvery == 0) {
    void* frames[kFrames] = {};
    const unsigned taken = RtlCaptureStackBackTrace(1, kFrames, frames, nullptr);
    char text[kFrames * 19 + 1] = {};
    std::size_t used = 0;
    for (unsigned index = 0; index < taken; ++index) {
      const std::uint64_t address = game_address(frames[index]);
      if (address == 0) {
        continue;
      }
      used += std::snprintf(text + used, sizeof(text) - used, "%llx ",
                            static_cast<unsigned long long>(address));
    }
    log("FAKEMOVE request entity=0x%x count=%u stack=%s", entity, count, text);
  }
  return original_request(manager, entity, request, done, out, flag);
}

// The gate that sends a movement into fake movement: the context's mover's
// vtable slot 14 (+ 0x70), or the context's flag at + 37. Each mover vtable is told once with the
// answer, the slot's address and the flag.
constexpr char kGate[] =
    "40 53 48 83 EC 20 48 8B D9 48 8B 09 48 8B 01 FF 50 70 32 D2 84 C0 75 0D 38 53 25 75 08";
using Gate = char(__fastcall*)(std::uint8_t* context);
Gate original_gate = nullptr;
std::unordered_map<void*, unsigned> gate_tables;

char __fastcall probed_gate(std::uint8_t* context) {
  const char answer = original_gate(context);
  void* mover = context ? *reinterpret_cast<void**>(context) : nullptr;
  void** table = mover ? *static_cast<void***>(mover) : nullptr;
  if (table) {
    const unsigned count = ++gate_tables[table];
    if (count <= 2 || count % 500 == 0) {
      log("FAKEMOVE gate answer=%u flag=%u mover vtable=%llx slot14=%llx count=%u",
          unsigned(answer), unsigned(context[37]),
          static_cast<unsigned long long>(game_address(table)),
          static_cast<unsigned long long>(game_address(table[14])), count);
    }
  }
  return answer;
}

}  // namespace

bool install() {
  void* gate = resolve_unique("FakeMovementGate", kGate);
  if (gate &&
      MH_CreateHook(gate, reinterpret_cast<void*>(probed_gate),
                    reinterpret_cast<void**>(&original_gate)) == MH_OK &&
      MH_EnableHook(gate) == MH_OK) {
    log("FAKEMOVE gate probe installed");
  }
  void* table =
      resolve_by_vtable_class("FakeMovementManagerVtable", kConstructor, 9, 13, kClass, true);
  void* request = table ? static_cast<void**>(table)[kRequestSlot] : nullptr;
  if (!request ||
      MH_CreateHook(request, reinterpret_cast<void*>(probed_request),
                    reinterpret_cast<void**>(&original_request)) != MH_OK ||
      MH_EnableHook(request) != MH_OK) {
    log("FAKEMOVE probe unavailable");
    return false;
  }
  log("FAKEMOVE probe installed at %llx", static_cast<unsigned long long>(game_address(request)));
  return true;
}

}  // namespace kcds::runtime::testing::fake_movement
