// The game's image (WHGame.dll as loaded) and finding its functions by byte pattern
// (shared by the features split out of game_client.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::game_client {

extern std::uint8_t* image;
extern std::size_t image_size;

// The one match of a byte pattern in the image: the function entry, or with `displacement` the
// target of the RIP-relative operand at + displacement (the instruction ending at + length).
// Null when it matches nowhere or more than once.
void* resolve_unique(const char* name, const char* pattern, int displacement = -1, int length = 0);

// The match whose RIP-relative operand is the vtable of the RTTI class `type_name`: its own
// address, or (with `vtable`) that vtable.
void* resolve_by_vtable_class(const char* name, const char* pattern, int displacement, int length,
                              const char* type_name, bool vtable);

// The match whose call goes to `callee`.
void* resolve_by_callee(const char* name, const char* pattern, int displacement, int length,
                        const void* callee);

// A virtual function of an object by its vtable slot.
template <class Fn>
Fn slot(void* object, std::size_t i) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[i]);
}

}  // namespace kcds::game_client
