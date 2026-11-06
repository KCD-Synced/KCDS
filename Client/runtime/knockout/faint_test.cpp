#include "faint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

namespace faint = kcds::runtime::knockout::faint;

// Stand-ins for the game's vtables: only their addresses are compared (not const, so the linker
// cannot fold the two into one).
std::array<void*, 4> unconscious_vtable{};
std::array<void*, 4> other_vtable{};

// A buff instance as the game lays it out (240 bytes): its vtable and the unconscious reason.
struct Instance {
  alignas(16) std::array<std::uint8_t, 240> bytes{};

  Instance(const void* vtable, std::uint32_t reason) {
    std::memcpy(bytes.data(), &vtable, sizeof(vtable));
    std::memcpy(bytes.data() + faint::kUnconsciousReason, &reason, sizeof(reason));
  }
};

// A soul with its buff vector (begin and end pointers at + 1352 and + 1360).
struct Soul {
  alignas(16) std::array<std::uint8_t, 2048> bytes{};
  std::vector<void*> buffs;

  void give(Instance& instance) {
    buffs.push_back(instance.bytes.data());
    publish();
  }

  void give_null() {
    buffs.push_back(nullptr);
    publish();
  }

  void publish() {
    void** begin = buffs.data();
    void** end = buffs.data() + buffs.size();
    std::memcpy(bytes.data() + faint::kSoulBuffsBegin, &begin, sizeof(begin));
    std::memcpy(bytes.data() + faint::kSoulBuffsEnd, &end, sizeof(end));
  }

  const void* data() const {
    return bytes.data();
  }
};

}  // namespace

KCD2S_TEST(faint, the_exhaustion_faint_is_told_apart_from_a_knockout) {
  Instance knockout(unconscious_vtable.data(), 1);
  Instance permanent(unconscious_vtable.data(), 0);
  Instance faint_instance(unconscious_vtable.data(), faint::kFaintReason);

  Soul knocked_out;
  knocked_out.give(knockout);
  KCD2S_CHECK(!faint::holds_faint(knocked_out.data(), unconscious_vtable.data()));

  Soul kept_down;
  kept_down.give(permanent);
  KCD2S_CHECK(!faint::holds_faint(kept_down.data(), unconscious_vtable.data()));

  Soul fainted;
  Instance unrelated(other_vtable.data(), 7);
  fainted.give(unrelated);
  fainted.give_null();
  fainted.give(faint_instance);
  KCD2S_CHECK(faint::holds_faint(fainted.data(), unconscious_vtable.data()));
}

KCD2S_TEST(faint, only_the_unconscious_buff_class_is_read) {
  // Another buff class keeps something else at + 220: its 3 is no faint.
  Instance other(other_vtable.data(), faint::kFaintReason);
  Soul soul;
  soul.give(other);
  KCD2S_CHECK(!faint::holds_faint(soul.data(), unconscious_vtable.data()));
}

KCD2S_TEST(faint, missing_soul_buffs_or_class_are_no_faint) {
  Instance faint_instance(unconscious_vtable.data(), faint::kFaintReason);
  Soul soul;
  soul.give(faint_instance);
  // Without the validated class nothing is read as a faint (every unconsciousness is then a
  // knock-out, as before the faint was known).
  KCD2S_CHECK(!faint::holds_faint(soul.data(), nullptr));
  KCD2S_CHECK(!faint::holds_faint(nullptr, unconscious_vtable.data()));

  Soul empty;
  KCD2S_CHECK(!faint::holds_faint(empty.data(), unconscious_vtable.data()));
  empty.publish();
  KCD2S_CHECK(!faint::holds_faint(empty.data(), unconscious_vtable.data()));
}
