#include "kettle_state.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>

namespace kcds::runtime::cookpot::kettle {
namespace {

constexpr char kStateMessage[] = "kettle:signal:onKettleStateChange";
constexpr char kFireMessage[] = "kettle:signal:onFireplacePhaseChange";
constexpr std::uint8_t kEmptyPhase = 1;

using Send = std::uint8_t(__fastcall*)(void* unused, const std::uint64_t* target, void* type,
                                       void* values);
using MakeString = void*(__fastcall*)(void* string, const char* text);
using FreeString = void(__fastcall*)(void* string);

Natives natives;
// fireplace WUID -> the state it was last told
std::unordered_map<std::uint64_t, std::string> told;

}  // namespace

bool install(const Natives& given) {
  natives = given;
  return natives.send && natives.make_string && natives.free_string && natives.wuid_of;
}

const char* state_of_phase(std::uint8_t phase) {
  return phase <= kEmptyPhase ? "empty" : "full";
}

bool tell_fire(void* fireplace, bool lit) {
  if (!natives.send || !fireplace) {
    return false;
  }
  std::uint64_t wuid = 0;
  if (!natives.wuid_of(fireplace, wuid)) {
    return false;
  }
  const char* values = lit ? "state($enum:kettleFireState.on)" : "state($enum:kettleFireState.off)";
  void* type_string = nullptr;
  void* values_string = nullptr;
  reinterpret_cast<MakeString>(natives.make_string)(&type_string, kFireMessage);
  reinterpret_cast<MakeString>(natives.make_string)(&values_string, values);
  const bool sent =
      reinterpret_cast<Send>(natives.send)(nullptr, &wuid, &type_string, &values_string) != 0;
  reinterpret_cast<FreeString>(natives.free_string)(&values_string);
  reinterpret_cast<FreeString>(natives.free_string)(&type_string);
  return sent;
}

bool tell(void* fireplace, std::uint8_t phase) {
  if (!natives.send || !fireplace) {
    return false;
  }
  std::uint64_t wuid = 0;
  if (!natives.wuid_of(fireplace, wuid)) {
    return false;
  }
  const std::string state = state_of_phase(phase);
  char values[64] = {};
  std::snprintf(values, sizeof(values), "state($enum:kettleState.%s)", state.c_str());
  // The game's strings are one pointer each.
  void* type_string = nullptr;
  void* values_string = nullptr;
  reinterpret_cast<MakeString>(natives.make_string)(&type_string, kStateMessage);
  reinterpret_cast<MakeString>(natives.make_string)(&values_string, values);
  const bool sent =
      reinterpret_cast<Send>(natives.send)(nullptr, &wuid, &type_string, &values_string) != 0;
  reinterpret_cast<FreeString>(natives.free_string)(&values_string);
  reinterpret_cast<FreeString>(natives.free_string)(&type_string);
  // Logged once per change of what a kettle is told.
  const auto found = told.find(wuid);
  const bool changed = found == told.end() || found->second != state;
  if (sent) {
    told[wuid] = state;
  }
  if (natives.log && (changed || !sent)) {
    natives.log("POTS fireplace %016llx told %s: %s", static_cast<unsigned long long>(wuid),
                state.c_str(), sent ? "sent" : "the game did not take it");
  }
  return sent;
}

void forget_all() {
  told.clear();
}

}  // namespace kcds::runtime::cookpot::kettle
