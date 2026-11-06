#include "stored_zip.hpp"

namespace kcds::runtime::files {
namespace {

std::uint32_t crc32_of(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::uint8_t byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  put16(out, value & 0xFFFFu);
  put16(out, value >> 16);
}

}  // namespace

// A zip archive with one stored (uncompressed) entry.
std::vector<std::uint8_t> stored_zip(const std::string& entry,
                                     const std::vector<std::uint8_t>& data) {
  const std::uint32_t crc = crc32_of(data);
  const auto size = static_cast<std::uint32_t>(data.size());
  const auto name_size = static_cast<std::uint32_t>(entry.size());
  constexpr std::uint32_t kDosDate = 0x21;  // 1980-01-01
  std::vector<std::uint8_t> out;
  put32(out, 0x04034B50u);
  put16(out, 20);
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  put16(out, kDosDate);
  put32(out, crc);
  put32(out, size);
  put32(out, size);
  put16(out, name_size);
  put16(out, 0);
  out.insert(out.end(), entry.begin(), entry.end());
  out.insert(out.end(), data.begin(), data.end());
  const auto directory = static_cast<std::uint32_t>(out.size());
  put32(out, 0x02014B50u);
  put16(out, 20);
  put16(out, 20);
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  put16(out, kDosDate);
  put32(out, crc);
  put32(out, size);
  put32(out, size);
  put16(out, name_size);
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  put16(out, 0);
  put32(out, 0);
  put32(out, 0);
  out.insert(out.end(), entry.begin(), entry.end());
  const auto directory_size = static_cast<std::uint32_t>(out.size()) - directory;
  put32(out, 0x06054B50u);
  put16(out, 0);
  put16(out, 0);
  put16(out, 1);
  put16(out, 1);
  put32(out, directory_size);
  put32(out, directory);
  put16(out, 0);
  return out;
}

// A zip archive with stored (uncompressed) entries, in the order given.
std::vector<std::uint8_t> stored_zip_entries(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries) {
  constexpr std::uint32_t kDosDate = 0x21;  // 1980-01-01
  std::vector<std::uint8_t> out;
  std::vector<std::uint32_t> offsets;
  std::vector<std::uint32_t> crcs;
  for (const auto& [name, data] : entries) {
    offsets.push_back(static_cast<std::uint32_t>(out.size()));
    crcs.push_back(crc32_of(data));
    put32(out, 0x04034B50u);
    put16(out, 20);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, kDosDate);
    put32(out, crcs.back());
    put32(out, static_cast<std::uint32_t>(data.size()));
    put32(out, static_cast<std::uint32_t>(data.size()));
    put16(out, static_cast<std::uint32_t>(name.size()));
    put16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
  }
  const auto directory = static_cast<std::uint32_t>(out.size());
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& [name, data] = entries[index];
    put32(out, 0x02014B50u);
    put16(out, 20);
    put16(out, 20);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, kDosDate);
    put32(out, crcs[index]);
    put32(out, static_cast<std::uint32_t>(data.size()));
    put32(out, static_cast<std::uint32_t>(data.size()));
    put16(out, static_cast<std::uint32_t>(name.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, offsets[index]);
    out.insert(out.end(), name.begin(), name.end());
  }
  const auto directory_size = static_cast<std::uint32_t>(out.size()) - directory;
  put32(out, 0x06054B50u);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<std::uint32_t>(entries.size()));
  put16(out, static_cast<std::uint32_t>(entries.size()));
  put32(out, directory_size);
  put32(out, directory);
  put16(out, 0);
  return out;
}

}  // namespace kcds::runtime::files
