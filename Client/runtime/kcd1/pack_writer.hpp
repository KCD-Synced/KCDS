// Writing a pack (a zip the game's CryPak opens) on disk, entry by entry: an entry of another pack
// is copied as it is stored there (its compressed bytes, never inflated), and new small files are
// stored. The KCD1 translator builds its level and material packs this way from the player's own
// copy of Kingdom Come: Deliverance. Plain code, no game calls.
//
// The game's packs are plain zips under 4 GB; one of more than 65535 entries keeps the low 16
// bits of their count in its end record, as the game's own level packs do (CryPak reads the
// directory to its end).
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../files/zip_read.hpp"

namespace kcds::runtime::kcd1 {

// A reader of a pack on disk for files::zip_entries and zip_entry_bytes; `size` gets its size
// (0 when it cannot be opened).
files::ZipReadAt pack_on_disk(const std::filesystem::path& file, std::uint64_t& size);

class PackWriter {
 public:
  explicit PackWriter(const std::filesystem::path& file);

  // An entry of `source` copied as it is stored there, under `name` (its own name when empty).
  bool copy(const files::ZipReadAt& source, const files::ZipEntry& entry,
            const std::string& name = {});

  // A new stored entry.
  bool add(const std::string& name, const std::vector<std::uint8_t>& data);
  bool add(const std::string& name, const std::string& text);

  // Writes the directory; false when anything failed or the pack grew past 4 GB.
  bool finish();

  std::size_t entries() const {
    return directory_.size();
  }

 private:
  struct Record {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
  };

  bool local_header(const Record& record);

  std::ofstream out_;
  std::uint64_t written_ = 0;
  bool failed_ = false;
  std::vector<Record> directory_;
};

}  // namespace kcds::runtime::kcd1
