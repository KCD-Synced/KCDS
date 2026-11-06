// Paks that exist only in memory (Asset/Animation Studio): KCDS hands the
// game a pak's bytes and the game reads it as if it lay in its Data folder. Nothing is written to
// the game's folder or anywhere else on disk.
//
// WHGame 1.5.5:
//   - the public pak open, CCryPak OpenPack, first checks that the pak exists on
//     disk, then opens it through, which checks again and
//     calls the inner open;
//   - the inner open (pak, binding root, name, flags, IMemoryBlock*) logs "Pak '%s'
//     is opened, root: '%s'" and gives the memory block to the archive open (CCryPak vtable
//     + 576). With a memory block and flag 8 (the inner open always sets it) that one
//     never touches the disk: it reads the zip from the block;
//   - IMemoryBlock: vtable [0] destructor, [1] delete, [2] GetData (+ 0x10), [3] GetSize
//     (+ 0x18), the reference count at + 8 (CMemoryBlock); the zip reader also
//     copies out of it through [4] (destination, offset, size), which the game's
//     in-memory zip blocks have and CMemoryBlock does not.
//
//   - the archive's unload from memory drops the block (cache + 64) and reads the
//     pak's file again; the game does it with a level's pak after the level loaded.
//
// install() hooks the public open: a pak KCDS provides (by the end of its path) is opened from
// its memory block through the inner open, every other pak as before. It also hooks the unload
// from memory, which leaves a KCDS block where it is (such a pak has no file to go back to).
//
// provide() any thread; the hook runs on the game's threads.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kcds::runtime::assets::memory_paks {

using Logger = void (*)(const char* format, ...);
using Resolver =
    std::function<void*(const char* name, const char* pattern, int displacement, int length)>;

// False when a function is not found; the game then opens only its own paks.
bool install(const Resolver& resolve, Logger logger);

// Whether the hooks are in: only then is a provided pak served.
bool available();

// Serves `bytes` (a zip pak) whenever the game opens a pak whose path ends with `path_end`
// (case and slashes do not matter, for example "levels/kcds_studio/level.pak"). The bytes stay
// in memory for the rest of the game; a later pak of the same path end is served instead.
void provide(const std::string& path_end, std::vector<std::uint8_t> bytes);

// Serves `bytes` under a name of its own, "kcds-memory\<number>\<key>.pak", and gives that name
// for ICryPak's OpenPack (which opens it from memory through the hook). The number is new for
// every call, so the game never takes an older pak of the same key it still has open.
std::string provide_virtual(const std::string& key, std::vector<std::uint8_t> bytes);

}  // namespace kcds::runtime::assets::memory_paks
