#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "kcd2s/integration/damage.hpp"

namespace kcds::runtime::combat {

// A quiet condition tick is killing damage only when it is recent and no nonquiet damage
// followed it. Receipt serials are nonzero and monotonic; milliseconds determine age only.
// Missing history, a future timestamp, or reused/equal receipt serials fail closed.
bool quiet_damage_eligible(std::uint64_t now, std::uint64_t quiet_occurrence,
                           std::uint64_t quiet_receipt, std::uint64_t last_nonquiet_receipt);

// Report receipt order is authoritative; occurrence is carried for quiet-damage attribution,
// never used to reorder reports. Access must be serialized by the caller.
class DamageReports {
 public:
  using Ticket = std::uint64_t;
  static constexpr std::size_t kCapacity = 64;

  struct Report {
    kcd2s::integration::PlayerDamage damage;
    std::uint64_t occurrence = 0;
  };

  DamageReports() = default;
  DamageReports(const DamageReports&) = delete;
  DamageReports& operator=(const DamageReports&) = delete;

  // False/nullopt on overflow. Nothing is evicted and no existing report is changed.
  bool push_ready(const kcd2s::integration::PlayerDamage& damage, std::uint64_t occurrence);
  std::optional<Ticket> stage_pending(const kcd2s::integration::PlayerDamage& damage,
                                      std::uint64_t occurrence);

  // A ruling changes only the staged damage, retaining its place and occurrence. Null cancels
  // the report (veto, failed send, or stale context). A ticket can be resolved only once.
  bool resolve(Ticket ticket, std::optional<kcd2s::integration::PlayerDamage> ruled);
  bool resolve_original(Ticket ticket);

  // Drains the resolved prefix, omitting vetoes. An unresolved report holds all later reports
  // across calls/frames, even when those later reports have already resolved.
  std::vector<Report> take_ready();

  // Discards ready and pending entries. Tickets are never recycled, including across clear;
  // staging fails permanently if the uint64 ticket space is exhausted.
  void clear();
  std::size_t size() const;
  std::size_t pending() const;

 private:
  enum class State {
    Pending,
    Ready,
    Vetoed,
  };

  struct Entry {
    Report report;
    Ticket ticket = 0;
    State state = State::Ready;
  };

  Entry* unresolved(Ticket ticket);

  std::array<Entry, kCapacity> entries_{};
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::size_t pending_ = 0;
  Ticket next_ticket_ = 1;
};

}  // namespace kcds::runtime::combat
