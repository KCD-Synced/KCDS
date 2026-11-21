#include "worn_item_aging.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "stock_projection.hpp"

namespace kcds::runtime::inventory::worn_aging {
namespace {

// (the step's owner, the person's inventory, their worn-item records { begin, end }, the result
// it adds to, the time's scale) -> whether anything changed.
using AgingStep = std::uint8_t(__fastcall*)(void* self, void* inventory, void** records,
                                            void* result, int scale);

constexpr std::size_t kItemFlags = 96;
constexpr std::uint8_t kWornFlag = 0x10;
constexpr std::size_t kItemGuid = 56;
constexpr std::size_t kRecordBytes = 248;
constexpr std::size_t kRecordGuid = 64;
// The most records looked through (a person wears a few dozen pieces at most).
constexpr std::size_t kMostRecords = 4096;
// The most left-out persons logged in a game.
constexpr unsigned kToldMax = 20;

AgingStep original_aging_step = nullptr;
Logger log_line = nullptr;
std::atomic<unsigned> told{0};

bool has_record(void** records, const std::uint8_t* item) {
  const auto* begin = static_cast<const std::uint8_t*>(records[0]);
  const auto* end = static_cast<const std::uint8_t*>(records[1]);
  if (!begin || end < begin || std::size_t(end - begin) > kMostRecords * kRecordBytes) {
    return false;
  }
  for (const std::uint8_t* record = begin; record < end; record += kRecordBytes) {
    if (std::memcmp(record + kRecordGuid, item + kItemGuid, 16) == 0) {
      return true;
    }
  }
  return false;
}

// Whether every worn item of the inventory has its record, as the step takes for granted.
bool every_worn_item_has_record(void* inventory, void** records) {
  for (void* item : items_in(inventory)) {
    const auto* bytes = static_cast<const std::uint8_t*>(item);
    if (!bytes || (bytes[kItemFlags] & kWornFlag) == 0) {
      continue;
    }
    if (!has_record(records, bytes)) {
      return false;
    }
  }
  return true;
}

std::uint8_t __fastcall guarded_aging_step(void* self, void* inventory, void** records,
                                           void* result, int scale) {
  if (inventory && records && !every_worn_item_has_record(inventory, records)) {
    if (log_line && told.fetch_add(1) < kToldMax) {
      log_line(
          "INVENTORY worn-item aging left out a person wearing an item without its record "
          "(inventory %p)",
          inventory);
    }
    return 0;
  }
  return original_aging_step(self, inventory, records, result, scale);
}

}  // namespace

bool install(void* aging_step, Logger logger) {
  log_line = logger;
  if (!aging_step) {
    return false;
  }
  const bool hooked = MH_CreateHook(aging_step, reinterpret_cast<void*>(&guarded_aging_step),
                                    reinterpret_cast<void**>(&original_aging_step)) == MH_OK &&
                      MH_EnableHook(aging_step) == MH_OK;
  if (!hooked) {
    original_aging_step = nullptr;
  }
  return hooked;
}

}  // namespace kcds::runtime::inventory::worn_aging
