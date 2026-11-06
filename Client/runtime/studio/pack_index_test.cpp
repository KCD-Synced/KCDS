// The game's packs' file names and a search in them (studio/pack_index.hpp).
#include "pack_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace {

namespace pack_index = kcds::runtime::studio::pack_index;

void put(std::vector<unsigned char>& out, std::uint32_t value, std::size_t size) {
  for (std::size_t part = 0; part < size; ++part) {
    out.push_back(static_cast<unsigned char>(value >> (part * 8)));
  }
}

// A central directory record for `name` (the zip's other fields zero; 2 extra bytes).
void record(std::vector<unsigned char>& out, const std::string& name) {
  put(out, 0x02014b50, 4);
  for (int field = 0; field < 24; ++field) {
    out.push_back(0);
  }
  put(out, std::uint32_t(name.size()), 2);
  put(out, 2, 2);
  put(out, 0, 2);
  for (int field = 0; field < 12; ++field) {
    out.push_back(0);
  }
  out.insert(out.end(), name.begin(), name.end());
  out.push_back(0xAB);
  out.push_back(0xCD);
}

}  // namespace

KCD2S_TEST(pack_index, reads_the_names_from_the_end_record) {
  std::vector<unsigned char> file(100, 0x11);  // the packed files before the directory
  std::vector<unsigned char> directory;
  record(directory, "Objects/props/whip.cgf");
  record(directory, "Animations/humans/male/x.caf");
  const std::uint32_t offset = std::uint32_t(file.size());
  file.insert(file.end(), directory.begin(), directory.end());
  put(file, 0x06054b50, 4);
  put(file, 0, 4);
  put(file, 2, 2);
  put(file, 2, 2);
  put(file, std::uint32_t(directory.size()), 4);
  put(file, offset, 4);
  put(file, 0, 2);
  // Only the file's last 80 bytes are at hand, as the reader reads a pack's end.
  const unsigned long long tail_start = file.size() - 80;
  const std::vector<unsigned char> tail(file.end() - 80, file.end());
  unsigned long long found_offset = 0;
  unsigned long long found_size = 0;
  KCD2S_CHECK(pack_index::find_directory(tail, tail_start, found_offset, found_size));
  KCD2S_CHECK_EQ(found_offset, (unsigned long long)offset);
  KCD2S_CHECK_EQ(found_size, (unsigned long long)directory.size());
  const auto names = pack_index::directory_names(directory);
  KCD2S_CHECK_EQ(names.size(), std::size_t(2));
  KCD2S_CHECK_EQ(names[0], std::string("Objects/props/whip.cgf"));
  KCD2S_CHECK_EQ(names[1], std::string("Animations/humans/male/x.caf"));
  // Not a zip: no end record.
  const std::vector<unsigned char> other(64, 0x22);
  KCD2S_CHECK(!pack_index::find_directory(other, 0, found_offset, found_size));
}

KCD2S_TEST(pack_index, a_search_wants_every_word_in_any_case) {
  const std::vector<std::string> paths = {"Objects/weapons/swords/sword_a.cgf",
                                          "Objects/props/horse/WHIP.cgf",
                                          "Objects/props/horse/saddle.cgf"};
  KCD2S_CHECK_EQ(pack_index::matching(paths, "whip").size(), std::size_t(1));
  KCD2S_CHECK_EQ(pack_index::matching(paths, "HORSE  props").size(), std::size_t(2));
  KCD2S_CHECK_EQ(pack_index::matching(paths, "horse sword").size(), std::size_t(0));
  KCD2S_CHECK_EQ(pack_index::matching(paths, "").size(), std::size_t(3));
}
