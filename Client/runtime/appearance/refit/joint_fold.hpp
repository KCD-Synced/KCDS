// A man's head piece made to add no joint to a woman's skeleton.
//
// A piece's skin lists the joints of the skeleton it was made on (every man's skin lists the male
// skeleton's own joints, some of which a woman's skeleton lacks) and its own (a helmet's straps).
// The engine adds each joint the wearer's skeleton lacks to an extended skeleton of the
// character. On a woman some men's helmets with such joints were not drawn at all. fold() gives
// the weights of every joint her skeleton lacks to its nearest ancestor she has (so the straps
// move rigidly with the head) and renames each such joint to one of her joints the skin does not
// name yet, its controller id the CRC32 of the new name, so the skin names only joints she has.
// The copy keeps every other byte of the file.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "skin_file.hpp"

namespace kcds::runtime::appearance::refit {

// The CRC32 (IEEE, as zlib) of `text`, as the skin's controller ids are.
std::uint32_t crc32_of(const std::string& text);

// `bytes` (the file `skin` was read from) with the joints that are not among `her_joints` folded
// away; empty when there is nothing to fold or her skeleton has too few joints to rename them to.
std::vector<std::uint8_t> fold(const std::vector<std::uint8_t>& bytes, const SkinFile& skin,
                               const std::vector<Joint>& her_joints);

}  // namespace kcds::runtime::appearance::refit
