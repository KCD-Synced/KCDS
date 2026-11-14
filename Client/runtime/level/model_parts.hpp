// Parts of a level model not drawn: a tub's water, a cart's load. A model's parts are the
// sub-materials of its multi-material (CMatInfo, flag 0x100: SetSubMtlCount slot 27,
// GetSubMtlCount slot 28, GetSubMtl slot 30, SetSubMtl slot 31; GetName slot 8). The model is
// drawn with a copy of that material made for it (the material manager's CreateMaterial with flag
// 0x80, so the manager lists no copy by name) whose named parts are one shared material with the
// engine's no-draw flag (0x400, the flag the material loader gives a "nodraw" shader). The other
// parts are the model's own, shared: only this model changes, never its other copies in the level.
//
// Engine thread.
#pragma once

#include <string>
#include <vector>

namespace kcds::runtime::level::parts {

// A copy of `source` (a multi-material) with the sub-materials named in `hidden` (lower case)
// not drawn. It holds one reference, given back with release(). `names` gets the source's part
// names, comma separated (for a log), `matched` how many of them are hidden. Null when the source
// has no parts or a copy could not be made.
void* without(void* source, const std::vector<std::string>& hidden, std::string& names,
              int& matched);

// Takes a reference to a material (a level's own, kept to be put back); null does nothing.
void hold(void* material);

// Gives back a reference without() or hold() took; null does nothing.
void release(void* material);

}  // namespace kcds::runtime::level::parts
