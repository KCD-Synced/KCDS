// The file names in the game's packs (zip files) and a search in them, for the studio's catalogue
// of the game's own files (studio/game_files.hpp). Plain code, no game calls.
#pragma once

#include <string>
#include <vector>

namespace kcds::runtime::studio::pack_index {

// Where a zip's central directory is (its offset and size in the file), from the file's last
// bytes (`tail`, which start at `tail_start` in it): the end record (0x06054b50) searched from the
// end. False when there is none.
bool find_directory(const std::vector<unsigned char>& tail, unsigned long long tail_start,
                    unsigned long long& offset, unsigned long long& size);

// The file names in a zip's central directory (records 0x02014b50).
std::vector<std::string> directory_names(const std::vector<unsigned char>& directory);

// The paths with every word of `query` in them (any case), in their order.
std::vector<std::string> matching(const std::vector<std::string>& paths, const std::string& query);

}  // namespace kcds::runtime::studio::pack_index
