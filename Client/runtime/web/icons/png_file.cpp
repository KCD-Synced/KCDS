#include "png_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace kcds::runtime::web::icons {
namespace {

constexpr std::size_t kStoredBlockBytes = 65535;

std::array<std::uint32_t, 256> crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t entry = 0; entry < 256; ++entry) {
    std::uint32_t value = entry;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
    }
    table[entry] = value;
  }
  return table;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  static const std::array<std::uint32_t, 256> table = crc_table();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t at = 0; at < size; ++at) {
    crc = table[(crc ^ data[at]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
  std::uint32_t low = 1;
  std::uint32_t high = 0;
  for (const std::uint8_t byte : data) {
    low = (low + byte) % 65521;
    high = (high + low) % 65521;
  }
  return (high << 16) | low;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(std::uint8_t(value >> 24));
  out.push_back(std::uint8_t(value >> 16));
  out.push_back(std::uint8_t(value >> 8));
  out.push_back(std::uint8_t(value));
}

// A chunk: its length, type, data and the CRC of type and data.
void put_chunk(std::vector<std::uint8_t>& out, const char* type,
               const std::vector<std::uint8_t>& data) {
  put32(out, std::uint32_t(data.size()));
  const std::size_t type_at = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(out.data() + type_at, 4 + data.size()));
}

}  // namespace

std::vector<std::uint8_t> png_file(const Picture& picture) {
  if (picture.width == 0 || picture.height == 0 ||
      picture.rgba.size() != std::size_t(picture.width) * picture.height * 4) {
    return {};
  }
  // The rows, each after its filter byte (0: none).
  const std::size_t row_bytes = std::size_t(picture.width) * 4;
  std::vector<std::uint8_t> rows;
  rows.reserve((row_bytes + 1) * picture.height);
  for (std::uint32_t y = 0; y < picture.height; ++y) {
    rows.push_back(0);
    const std::uint8_t* row = picture.rgba.data() + y * row_bytes;
    rows.insert(rows.end(), row, row + row_bytes);
  }
  // A zlib stream of stored blocks.
  std::vector<std::uint8_t> stream{0x78, 0x01};
  std::size_t at = 0;
  do {
    const std::size_t size = std::min(kStoredBlockBytes, rows.size() - at);
    const bool last = at + size == rows.size();
    stream.push_back(last ? 1 : 0);
    stream.push_back(std::uint8_t(size));
    stream.push_back(std::uint8_t(size >> 8));
    stream.push_back(std::uint8_t(~size));
    stream.push_back(std::uint8_t(~size >> 8));
    stream.insert(stream.end(), rows.begin() + at, rows.begin() + at + size);
    at += size;
  } while (at < rows.size());
  put32(stream, adler32(rows));

  std::vector<std::uint8_t> file{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<std::uint8_t> header;
  put32(header, picture.width);
  put32(header, picture.height);
  header.push_back(8);  // bits per channel
  header.push_back(6);  // RGBA
  header.push_back(0);  // deflate
  header.push_back(0);  // adaptive filtering
  header.push_back(0);  // no interlace
  put_chunk(file, "IHDR", header);
  put_chunk(file, "IDAT", stream);
  put_chunk(file, "IEND", {});
  return file;
}

}  // namespace kcds::runtime::web::icons
