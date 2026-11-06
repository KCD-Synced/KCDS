#include "memory_paks.hpp"

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>

#include "MinHook.h"

namespace kcds::runtime::assets::memory_paks {
namespace {

// The game's IMemoryBlock as its own code reads it (see the header). The game counts references
// at + 8; a block KCDS gives it starts with so many that it never deletes one.
struct MemoryBlock {
  void* const* vtable;
  volatile long references;
  std::uint32_t padding;
  void* data;
  std::int32_t size;
};
static_assert(offsetof(MemoryBlock, references) == 8);
static_assert(offsetof(MemoryBlock, data) == 0x10);
static_assert(offsetof(MemoryBlock, size) == 0x18);

void* __fastcall block_destroy(MemoryBlock* block, unsigned flags) {
  (void)flags;
  return block;
}

void __fastcall block_delete(MemoryBlock* block) {
  (void)block;
}

void* __fastcall block_data(MemoryBlock* block) {
  return block->data;
}

std::int32_t __fastcall block_size(MemoryBlock* block) {
  return block->size;
}

// The zip reader's read from memory copies through slot 4: destination, offset,
// size (it keeps the size within the block itself).
void __fastcall block_copy(MemoryBlock* block, void* destination, std::size_t offset,
                           std::size_t size) {
  std::memcpy(destination, static_cast<const std::uint8_t*>(block->data) + offset, size);
}

void* const kBlockVtable[] = {
    reinterpret_cast<void*>(&block_destroy), reinterpret_cast<void*>(&block_delete),
    reinterpret_cast<void*>(&block_data),    reinterpret_cast<void*>(&block_size),
    reinterpret_cast<void*>(&block_copy),
};

constexpr long kNeverDeleted = 0x3FFFFFFF;

struct Provided {
  std::string path_end;
  std::vector<std::uint8_t> bytes;
  MemoryBlock block{};
};

std::mutex provided_mutex;
std::vector<std::unique_ptr<Provided>> provided;
Logger write_log = nullptr;
std::atomic<bool> installed{false};

using OpenPack = char(__fastcall*)(void* pak, const char* root, const char* name, unsigned flags,
                                   void* data);
OpenPack original_open = nullptr;
OpenPack inner_open = nullptr;

// A path in one form: lower case, backslashes.
std::string plain(const std::string& path) {
  std::string out = path;
  for (char& c : out) {
    c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

MemoryBlock* block_for(const char* name) {
  if (!name) {
    return nullptr;
  }
  const std::string wanted = plain(name);
  std::lock_guard<std::mutex> lock(provided_mutex);
  // The newest first: a pak provided again for the same path end stands in for the older one.
  for (auto one = provided.rbegin(); one != provided.rend(); ++one) {
    const std::string& end = (*one)->path_end;
    if (wanted.size() >= end.size() &&
        wanted.compare(wanted.size() - end.size(), end.size(), end) == 0) {
      return &(*one)->block;
    }
  }
  return nullptr;
}

char __fastcall hooked_open(void* pak, const char* root, const char* name, unsigned flags,
                            void* data) {
  if (!data) {
    if (MemoryBlock* block = block_for(name)) {
      const char opened = inner_open(pak, root, name, flags, block);
      if (write_log) {
        write_log("ASSETS pak %s opened from memory (%d bytes): %d", name, block->size,
                  int(opened));
      }
      return opened;
    }
  }
  return original_open(pak, root, name, flags, data);
}

// The archive's unload from memory (the archive's cache, its file's memory block
// at + 64) lets go of the block and reads from the pak's file again. The game does it with a
// level's pak once the level is loaded. A pak of KCDS's has no file,
// so its archive keeps its block.
using UnloadFromMemory = void(__fastcall*)(std::uint8_t* cache);
UnloadFromMemory original_unload = nullptr;

void __fastcall hooked_unload(std::uint8_t* cache) {
  const auto* block = *reinterpret_cast<MemoryBlock* const*>(cache + 64);
  if (block && block->vtable == kBlockVtable) {
    return;
  }
  original_unload(cache);
}

}  // namespace

bool install(const Resolver& resolve, Logger logger) {
  write_log = logger;
  // The level's pak open calls the public open; calls the inner one.
  void* public_open = resolve(
      "CCryPak::OpenPack", "4C 8B C7 48 8B D3 49 8B CE E8 ?? ?? ?? ?? 40 8A E8 4D 85 FF", 10, 14);
  void* inner = resolve("CCryPak::OpenPackCommon",
                        "4C 8B 85 90 01 00 00 45 8B CC 49 8B D7 4C 89 6C 24 20 48 8B CE E8 ?? ?? "
                        "?? ?? 8A D8",
                        22, 26);
  void* unload = resolve("ZipDir::Cache::UnloadFromMemory",
                         "48 89 5C 24 08 57 48 83 EC 20 48 8B 99 98 09 00 00 48 8B F9 48 8D 4B 08 "
                         "E8 ?? ?? ?? ?? 48 8D 4F 08 E8 ?? ?? ?? ?? 48 8D 4B 08 48 8B 5C 24 30",
                         -1, 0);
  if (!public_open || !inner || !unload) {
    write_log("ASSETS memory paks unavailable");
    return false;
  }
  inner_open = reinterpret_cast<OpenPack>(inner);
  if (MH_CreateHook(unload, reinterpret_cast<void*>(&hooked_unload),
                    reinterpret_cast<void**>(&original_unload)) != MH_OK ||
      MH_EnableHook(unload) != MH_OK ||
      MH_CreateHook(public_open, reinterpret_cast<void*>(&hooked_open),
                    reinterpret_cast<void**>(&original_open)) != MH_OK ||
      MH_EnableHook(public_open) != MH_OK) {
    write_log("ASSETS memory paks hook failed");
    return false;
  }
  installed = true;
  write_log("ASSETS memory paks installed");
  return true;
}

bool available() {
  return installed;
}

std::string provide_virtual(const std::string& key, std::vector<std::uint8_t> bytes) {
  static std::atomic<unsigned> next{1};
  std::string name = "kcds-memory\\" + std::to_string(next.fetch_add(1)) + "\\" + key + ".pak";
  for (char& c : name) {
    if (c == '/') {
      c = '\\';
    }
  }
  provide(name, std::move(bytes));
  return name;
}

void provide(const std::string& path_end, std::vector<std::uint8_t> bytes) {
  auto one = std::make_unique<Provided>();
  one->path_end = plain(path_end);
  one->bytes = std::move(bytes);
  one->block.vtable = kBlockVtable;
  one->block.references = kNeverDeleted;
  one->block.data = one->bytes.data();
  one->block.size = static_cast<std::int32_t>(one->bytes.size());
  std::lock_guard<std::mutex> lock(provided_mutex);
  provided.push_back(std::move(one));
}

}  // namespace kcds::runtime::assets::memory_paks
