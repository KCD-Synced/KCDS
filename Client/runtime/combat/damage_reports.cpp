#include "damage_reports.hpp"

namespace kcds::runtime::combat {

bool quiet_damage_eligible(std::uint64_t now, std::uint64_t quiet_occurrence,
                           std::uint64_t quiet_receipt, std::uint64_t last_nonquiet_receipt) {
  constexpr std::uint64_t kMaxAgeMs = 1000;
  return quiet_occurrence != 0 && quiet_receipt != 0 && now >= quiet_occurrence &&
         now - quiet_occurrence <= kMaxAgeMs && last_nonquiet_receipt < quiet_receipt;
}

bool DamageReports::push_ready(const kcd2s::integration::PlayerDamage& damage,
                               std::uint64_t occurrence) {
  if (size_ == kCapacity) {
    return false;
  }
  auto& entry = entries_[(head_ + size_) % kCapacity];
  entry = Entry{Report{damage, occurrence}, 0, State::Ready};
  ++size_;
  return true;
}

std::optional<DamageReports::Ticket> DamageReports::stage_pending(
    const kcd2s::integration::PlayerDamage& damage, std::uint64_t occurrence) {
  if (size_ == kCapacity || next_ticket_ == 0) {
    return std::nullopt;
  }
  const Ticket ticket = next_ticket_++;
  auto& entry = entries_[(head_ + size_) % kCapacity];
  entry = Entry{Report{damage, occurrence}, ticket, State::Pending};
  ++size_;
  ++pending_;
  return ticket;
}

DamageReports::Entry* DamageReports::unresolved(Ticket ticket) {
  if (!ticket) {
    return nullptr;
  }
  for (std::size_t offset = 0; offset < size_; ++offset) {
    auto& entry = entries_[(head_ + offset) % kCapacity];
    if (entry.ticket == ticket && entry.state == State::Pending) {
      return &entry;
    }
  }
  return nullptr;
}

bool DamageReports::resolve(Ticket ticket, std::optional<kcd2s::integration::PlayerDamage> ruled) {
  auto* entry = unresolved(ticket);
  if (!entry) {
    return false;
  }
  if (ruled) {
    entry->report.damage = *ruled;
    entry->state = State::Ready;
  } else {
    entry->state = State::Vetoed;
  }
  --pending_;
  return true;
}

bool DamageReports::resolve_original(Ticket ticket) {
  auto* entry = unresolved(ticket);
  if (!entry) {
    return false;
  }
  entry->state = State::Ready;
  --pending_;
  return true;
}

std::vector<DamageReports::Report> DamageReports::take_ready() {
  if (!size_ || entries_[head_].state == State::Pending) {
    return {};
  }
  std::vector<Report> ready;
  ready.reserve(size_);
  while (size_ && entries_[head_].state != State::Pending) {
    auto& entry = entries_[head_];
    if (entry.state == State::Ready) {
      ready.push_back(entry.report);
    }
    entry = {};
    head_ = (head_ + 1) % kCapacity;
    --size_;
  }
  return ready;
}

void DamageReports::clear() {
  entries_ = {};
  head_ = 0;
  size_ = 0;
  pending_ = 0;
}

std::size_t DamageReports::size() const {
  return size_;
}

std::size_t DamageReports::pending() const {
  return pending_;
}

}  // namespace kcds::runtime::combat
