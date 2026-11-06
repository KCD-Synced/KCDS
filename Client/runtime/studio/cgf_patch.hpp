// A model's material path rewritten (Asset/Animation Studio): a .cgf names the .mtl it
// is drawn with in its MtlName chunk (type 0x1014: the game path without its extension in the
// first 128 bytes, then the sub-materials). A mesh exported into a resource's stream folder is
// drawn with the resource's copy of its material, objects/kcds/<resource>/<name>, so the copy's
// chunk names that path.
//
// The file (CrCh, version 0x746, as Dev/tools/assets/blender/kcds_cgf.py writes it): a 16-byte
// header (signature, version, chunk count, table offset), then per chunk 16 bytes (type u16,
// version u16, id u32, size u32, offset u32).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::studio::cgf {

// Names `material` (at most 127 characters) in every MtlName chunk of `model`; false (and the
// model as it was) when it is not such a file or has none.
bool rename_material(std::vector<std::uint8_t>& model, const std::string& material);

}  // namespace kcds::runtime::studio::cgf
