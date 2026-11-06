#include "targetable.hpp"

#include <mutex>
#include <unordered_set>

#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::combat::targetable {
namespace {

namespace sdk = kcd2s::sdk;

std::mutex untargetable_mutex;
std::unordered_set<std::uint32_t> untargetable_players;

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "playerTargetable") {
    return false;
  }
  const sdk::Value* id = kcd2s::integration::script_field(message, "id");
  const sdk::Value* targetable = kcd2s::integration::script_field(message, "targetable");
  if (!id || !targetable || targetable->tag() != sdk::Tag::Bool) {
    return true;
  }
  double number = -1;
  if (id->tag() == sdk::Tag::Int) {
    number = double(id->as_int());
  } else if (id->tag() == sdk::Tag::Double) {
    number = id->as_double();
  }
  if (!(number > 0 && number <= 4294967295.0)) {
    return true;
  }
  const auto player = std::uint32_t(number);
  std::lock_guard<std::mutex> lock(untargetable_mutex);
  if (targetable->as_bool()) {
    untargetable_players.erase(player);
  } else {
    untargetable_players.insert(player);
  }
  return true;
}

bool untargetable(std::uint32_t player) {
  std::lock_guard<std::mutex> lock(untargetable_mutex);
  return untargetable_players.count(player) > 0;
}

void reset() {
  std::lock_guard<std::mutex> lock(untargetable_mutex);
  untargetable_players.clear();
}

}  // namespace kcds::runtime::combat::targetable
