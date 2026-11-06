// Zip archives with stored (uncompressed) entries, the form the game's file system mounts as a
// pack (ICryPak OpenPack/OpenPacks): the book pictures and streamed files of native_ui and the
// female skeleton's animation list (appearance/female_skeleton).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kcds::runtime::files {

// A zip archive with one stored entry.
std::vector<std::uint8_t> stored_zip(const std::string& entry,
                                     const std::vector<std::uint8_t>& data);

// A zip archive with stored entries, in the order given.
std::vector<std::uint8_t> stored_zip_entries(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries);

}  // namespace kcds::runtime::files
