// The game's image (WHGame.dll as loaded) and finding its functions by byte pattern
// (shared by the features split out of game_client.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace kcds::game_client {

extern std::uint8_t* image;
extern std::size_t image_size;

// The one match of a byte pattern in the image: the function entry, or with `displacement` the
// target of the RIP-relative operand at + displacement (the instruction ending at + length).
// Null when it matches nowhere or more than once.
void* resolve_unique(const char* name, const char* pattern, int displacement = -1, int length = 0);

// One of a function's patterns, as resolve_unique takes them.
struct NativePattern {
  const char* pattern = nullptr;
  int displacement = -1;
  int length = 0;
};

// The first of several patterns that matches once, in order: a game patch that changes one
// spelling of a function leaves the others to find it. Null when none does.
void* resolve_any(const char* name, std::initializer_list<NativePattern> patterns);

// What resolve_unique and resolve_any looked for since the start: how many were found, and the
// names of those not found (at most kMaxMissingNatives), for the client's report to the server.
inline constexpr std::size_t kMaxMissingNatives = 64;
struct NativeLookups {
  std::size_t found = 0;
  std::vector<std::string> missing;
};
NativeLookups native_lookups();

// The match whose RIP-relative operand is the vtable of the RTTI class `type_name`: its own
// address, or (with `vtable`) that vtable.
void* resolve_by_vtable_class(const char* name, const char* pattern, int displacement, int length,
                              const char* type_name, bool vtable);

// The match whose RIP-relative operand (at + displacement, the instruction ending at + length)
// points to the zero-terminated `text`: functions that are the same code but for the string they
// load. Null when none or more than one does.
void* resolve_by_text(const char* name, const char* pattern, int displacement, int length,
                      const char* text);

// The match whose call goes to `callee`.
void* resolve_by_callee(const char* name, const char* pattern, int displacement, int length,
                        const void* callee);

// A virtual function of an object by its vtable slot.
template <class Fn>
Fn slot(void* object, std::size_t i) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[i]);
}

}  // namespace kcds::game_client
