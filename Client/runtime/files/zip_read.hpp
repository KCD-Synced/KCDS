// Reading a zip archive: its entries from the central directory and each entry's bytes, stored or
// deflated (RFC 1951, inflated here). Enough for the game's own packs, which are plain zip files
// of at most 2 GB (no zip64, no encryption). The archive is read through a callback, so a large
// pack on disk is read only where needed. Plain code, no game calls.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::files {

struct ZipEntry {
  std::string name;          // as the central directory names it, slashes as written
  std::uint16_t method = 0;  // 0 stored, 8 deflated
  std::uint32_t crc = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t size = 0;
  std::uint32_t local_offset = 0;  // the entry's local header
};

// Reads `length` bytes of the archive at `offset` into `out`; false when they are not all there.
using ZipReadAt =
    std::function<bool(std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& out)>;

// The entries of an archive of `archive_size` bytes, in their central directory's order; nullopt
// when it has no readable central directory.
std::optional<std::vector<ZipEntry>> zip_entries(const ZipReadAt& read, std::uint64_t archive_size);

// An entry's bytes, inflated when deflated; nullopt when its data cannot be read, its method is
// neither stored nor deflated, or the bytes are not its size or CRC.
std::optional<std::vector<std::uint8_t>> zip_entry_bytes(const ZipReadAt& read,
                                                         const ZipEntry& entry);

// A reader of an archive held in memory (`zip` must outlive it).
ZipReadAt zip_in_memory(const std::vector<std::uint8_t>& zip);

// Raw deflate data inflated to exactly `size` bytes; nullopt when the data is not valid deflate
// or does not give that many bytes.
std::optional<std::vector<std::uint8_t>> inflate(const std::uint8_t* data, std::size_t length,
                                                 std::size_t size);

// The CRC-32 of zip files (polynomial 0xEDB88320).
std::uint32_t zip_crc32(const std::uint8_t* data, std::size_t length);

}  // namespace kcds::runtime::files
