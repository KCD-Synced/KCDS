#include "zip_read.hpp"

#include <array>

namespace kcds::runtime::files {
namespace {

constexpr std::uint32_t kEndRecord = 0x06054b50u;
constexpr std::uint32_t kDirectoryRecord = 0x02014b50u;
constexpr std::uint32_t kLocalRecord = 0x04034b50u;
constexpr std::size_t kEndRecordSize = 22;
constexpr std::size_t kDirectoryRecordSize = 46;
constexpr std::size_t kLocalRecordSize = 30;
// The end record sits at most this far from the end (its comment is at most 65535 bytes).
constexpr std::size_t kEndSearch = kEndRecordSize + 0xFFFF;

std::uint16_t read16(const std::uint8_t* at) {
  return std::uint16_t(at[0] | (at[1] << 8));
}

std::uint32_t read32(const std::uint8_t* at) {
  return std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) | (std::uint32_t(at[2]) << 16) |
         (std::uint32_t(at[3]) << 24);
}

// Deflate's bits, the lowest bit of each byte first.
struct Bits {
  const std::uint8_t* data = nullptr;
  std::size_t length = 0;
  std::size_t at = 0;  // the next byte
  std::uint32_t held = 0;
  int count = 0;  // bits held
  bool overrun = false;

  // The next `wanted` bits (at most 24) as a number, the first bit lowest.
  std::uint32_t take(int wanted) {
    while (count < wanted) {
      if (at >= length) {
        overrun = true;
        return 0;
      }
      held |= std::uint32_t(data[at]) << count;
      at += 1;
      count += 8;
    }
    const std::uint32_t value = held & ((1u << wanted) - 1u);
    held >>= wanted;
    count -= wanted;
    return value;
  }

  // Drops the bits left of the current byte (a stored block starts on a byte).
  void align() {
    held = 0;
    count = 0;
  }
};

constexpr int kMaxBits = 15;
constexpr int kLiteralCodes = 288;
constexpr int kDistanceCodes = 30;

// A canonical Huffman code: how many codes have each length, and the symbols in code order.
struct Huffman {
  std::array<std::uint16_t, kMaxBits + 1> count{};
  std::array<std::uint16_t, kLiteralCodes> symbol{};
};

// The code of `lengths` (0 for a symbol not used); false when the lengths ask for more codes
// than there are (an incomplete code is allowed, as zlib allows it).
bool build(Huffman& code, const std::uint8_t* lengths, int symbols) {
  code.count.fill(0);
  for (int index = 0; index < symbols; ++index) {
    code.count[lengths[index]] += 1;
  }
  int left = 1;
  for (int length = 1; length <= kMaxBits; ++length) {
    left <<= 1;
    left -= code.count[length];
    if (left < 0) {
      return false;
    }
  }
  std::array<std::uint16_t, kMaxBits + 1> offsets{};
  for (int length = 1; length < kMaxBits; ++length) {
    offsets[length + 1] = std::uint16_t(offsets[length] + code.count[length]);
  }
  for (int index = 0; index < symbols; ++index) {
    if (lengths[index] != 0) {
      code.symbol[offsets[lengths[index]]] = std::uint16_t(index);
      offsets[lengths[index]] += 1;
    }
  }
  return true;
}

// The next symbol of `code`; -1 when the bits name no symbol or run out.
int decode(Bits& bits, const Huffman& code) {
  int value = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= kMaxBits; ++length) {
    value |= int(bits.take(1));
    if (bits.overrun) {
      return -1;
    }
    const int count = code.count[length];
    if (value - count < first) {
      return code.symbol[index + (value - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    value <<= 1;
  }
  return -1;
}

constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                       15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                       67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                         4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                         9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// One block's symbols until its end (256) into `out`; false on bad data or more than `size`.
bool inflate_codes(Bits& bits, const Huffman& literals, const Huffman& distances,
                   std::vector<std::uint8_t>& out, std::size_t size) {
  for (;;) {
    const int symbol = decode(bits, literals);
    if (symbol < 0) {
      return false;
    }
    if (symbol < 256) {
      if (out.size() >= size) {
        return false;
      }
      out.push_back(std::uint8_t(symbol));
      continue;
    }
    if (symbol == 256) {
      return true;
    }
    const int length_code = symbol - 257;
    if (length_code >= int(kLengthBase.size())) {
      return false;
    }
    const std::size_t length = kLengthBase[length_code] + bits.take(kLengthExtra[length_code]);
    const int distance_code = decode(bits, distances);
    if (distance_code < 0 || distance_code >= int(kDistanceBase.size())) {
      return false;
    }
    const std::size_t distance =
        kDistanceBase[distance_code] + bits.take(kDistanceExtra[distance_code]);
    if (bits.overrun || distance > out.size() || out.size() + length > size) {
      return false;
    }
    // Byte by byte: a copy may overlap what it writes (a run).
    const std::size_t from = out.size() - distance;
    for (std::size_t index = 0; index < length; ++index) {
      out.push_back(out[from + index]);
    }
  }
}

bool fixed_codes(Huffman& literals, Huffman& distances) {
  std::array<std::uint8_t, kLiteralCodes> lengths{};
  for (int index = 0; index < kLiteralCodes; ++index) {
    if (index < 144) {
      lengths[index] = 8;
    } else if (index < 256) {
      lengths[index] = 9;
    } else if (index < 280) {
      lengths[index] = 7;
    } else {
      lengths[index] = 8;
    }
  }
  std::array<std::uint8_t, kDistanceCodes> distance_lengths{};
  distance_lengths.fill(5);
  return build(literals, lengths.data(), kLiteralCodes) &&
         build(distances, distance_lengths.data(), kDistanceCodes);
}

// A dynamic block's code lengths (RFC 1951 3.2.7) and its two codes.
bool dynamic_codes(Bits& bits, Huffman& literals, Huffman& distances) {
  static constexpr std::array<std::uint8_t, 19> kOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                          11, 4,  12, 3, 13, 2, 14, 1, 15};
  const int literal_count = int(bits.take(5)) + 257;
  const int distance_count = int(bits.take(5)) + 1;
  const int length_count = int(bits.take(4)) + 4;
  if (bits.overrun || literal_count > 286 || distance_count > kDistanceCodes) {
    return false;
  }
  std::array<std::uint8_t, 19> length_lengths{};
  for (int index = 0; index < length_count; ++index) {
    length_lengths[kOrder[index]] = std::uint8_t(bits.take(3));
  }
  Huffman lengths_code;
  if (bits.overrun || !build(lengths_code, length_lengths.data(), 19)) {
    return false;
  }
  std::array<std::uint8_t, kLiteralCodes + kDistanceCodes> lengths{};
  int index = 0;
  const int total = literal_count + distance_count;
  while (index < total) {
    const int symbol = decode(bits, lengths_code);
    if (symbol < 0) {
      return false;
    }
    if (symbol < 16) {
      lengths[index] = std::uint8_t(symbol);
      index += 1;
      continue;
    }
    std::uint8_t repeated = 0;
    int times = 0;
    if (symbol == 16) {
      if (index == 0) {
        return false;
      }
      repeated = lengths[index - 1];
      times = 3 + int(bits.take(2));
    } else if (symbol == 17) {
      times = 3 + int(bits.take(3));
    } else {
      times = 11 + int(bits.take(7));
    }
    if (bits.overrun || index + times > total) {
      return false;
    }
    for (int step = 0; step < times; ++step) {
      lengths[index] = repeated;
      index += 1;
    }
  }
  // A block without its end code cannot end.
  if (lengths[256] == 0) {
    return false;
  }
  return build(literals, lengths.data(), literal_count) &&
         build(distances, lengths.data() + literal_count, distance_count);
}

}  // namespace

std::optional<std::vector<std::uint8_t>> inflate(const std::uint8_t* data, std::size_t length,
                                                 std::size_t size) {
  Bits bits;
  bits.data = data;
  bits.length = length;
  std::vector<std::uint8_t> out;
  out.reserve(size);
  bool last = false;
  while (!last) {
    last = bits.take(1) == 1;
    const std::uint32_t type = bits.take(2);
    if (bits.overrun) {
      return std::nullopt;
    }
    if (type == 0) {
      bits.align();
      if (bits.at + 4 > length) {
        return std::nullopt;
      }
      const std::uint16_t stored = read16(data + bits.at);
      const std::uint16_t check = read16(data + bits.at + 2);
      bits.at += 4;
      if (std::uint16_t(~check) != stored || bits.at + stored > length ||
          out.size() + stored > size) {
        return std::nullopt;
      }
      out.insert(out.end(), data + bits.at, data + bits.at + stored);
      bits.at += stored;
      continue;
    }
    Huffman literals;
    Huffman distances;
    if (type == 1) {
      fixed_codes(literals, distances);
    } else if (type == 2) {
      if (!dynamic_codes(bits, literals, distances)) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
    if (!inflate_codes(bits, literals, distances, out, size)) {
      return std::nullopt;
    }
  }
  if (out.size() != size) {
    return std::nullopt;
  }
  return out;
}

std::uint32_t zip_crc32(const std::uint8_t* data, std::size_t length) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
      }
      values[index] = value;
    }
    return values;
  }();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t index = 0; index < length; ++index) {
    crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

ZipReadAt zip_in_memory(const std::vector<std::uint8_t>& zip) {
  return [&zip](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& out) {
    if (offset > zip.size() || length > zip.size() - offset) {
      return false;
    }
    out.assign(zip.begin() + std::ptrdiff_t(offset), zip.begin() + std::ptrdiff_t(offset + length));
    return true;
  };
}

std::optional<std::vector<ZipEntry>> zip_entries(const ZipReadAt& read,
                                                 std::uint64_t archive_size) {
  if (archive_size < kEndRecordSize) {
    return std::nullopt;
  }
  // The end record, searched from the end of the archive's last bytes.
  const std::uint64_t tail_start = archive_size > kEndSearch ? archive_size - kEndSearch : 0;
  std::vector<std::uint8_t> tail;
  if (!read(tail_start, std::size_t(archive_size - tail_start), tail)) {
    return std::nullopt;
  }
  std::size_t end = tail.size() - kEndRecordSize + 1;
  bool found = false;
  while (end > 0) {
    end -= 1;
    if (read32(tail.data() + end) == kEndRecord) {
      found = true;
      break;
    }
  }
  if (!found) {
    return std::nullopt;
  }
  const std::uint16_t count = read16(tail.data() + end + 10);
  const std::uint32_t directory_size = read32(tail.data() + end + 12);
  const std::uint32_t directory_offset = read32(tail.data() + end + 16);
  std::vector<std::uint8_t> directory;
  if (!read(directory_offset, directory_size, directory)) {
    return std::nullopt;
  }
  std::vector<ZipEntry> entries;
  entries.reserve(count);
  std::size_t at = 0;
  for (std::uint16_t index = 0; index < count; ++index) {
    if (at + kDirectoryRecordSize > directory.size() ||
        read32(directory.data() + at) != kDirectoryRecord) {
      return std::nullopt;
    }
    const std::uint8_t* record = directory.data() + at;
    ZipEntry entry;
    entry.method = read16(record + 10);
    entry.crc = read32(record + 16);
    entry.compressed_size = read32(record + 20);
    entry.size = read32(record + 24);
    const std::uint16_t name_length = read16(record + 28);
    const std::uint16_t extra_length = read16(record + 30);
    const std::uint16_t comment_length = read16(record + 32);
    entry.local_offset = read32(record + 42);
    const std::size_t next =
        at + kDirectoryRecordSize + name_length + extra_length + comment_length;
    if (next > directory.size()) {
      return std::nullopt;
    }
    entry.name.assign(reinterpret_cast<const char*>(record + kDirectoryRecordSize), name_length);
    entries.push_back(std::move(entry));
    at = next;
  }
  return entries;
}

std::optional<std::vector<std::uint8_t>> zip_entry_bytes(const ZipReadAt& read,
                                                         const ZipEntry& entry) {
  std::vector<std::uint8_t> header;
  if (!read(entry.local_offset, kLocalRecordSize, header) ||
      read32(header.data()) != kLocalRecord) {
    return std::nullopt;
  }
  // The local header's own name and extra lengths (the game's packs write the name with the
  // other slash there).
  const std::uint64_t start = std::uint64_t(entry.local_offset) + kLocalRecordSize +
                              read16(header.data() + 26) + read16(header.data() + 28);
  std::vector<std::uint8_t> data;
  if (!read(start, entry.compressed_size, data)) {
    return std::nullopt;
  }
  std::optional<std::vector<std::uint8_t>> bytes;
  if (entry.method == 0) {
    if (entry.compressed_size != entry.size) {
      return std::nullopt;
    }
    bytes = std::move(data);
  } else if (entry.method == 8) {
    bytes = inflate(data.data(), data.size(), entry.size);
  }
  if (!bytes || zip_crc32(bytes->data(), bytes->size()) != entry.crc) {
    return std::nullopt;
  }
  return bytes;
}

}  // namespace kcds::runtime::files
