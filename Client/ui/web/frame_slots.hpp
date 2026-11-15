// The textures a view's frames go to the game through: three per view, written by the CEF frame
// thread and read by the game's present thread, without a keyed mutex.
//
// A keyed mutex made either side wait on the GPU: under a busy GPU the CEF side's AcquireSync
// blocked its thread for milliseconds per frame, and that thread also carried the mouse and keys to
// the pages, which arrived late and in bursts. With three textures neither side waits: the writer
// only writes a texture that is neither the newest whole frame nor one the game is copying from,
// and a texture becomes the newest only once its write finished on the GPU; the game copies only
// the newest, and only once per new frame.
//
// Both sides change these under the web host's state lock.
#pragma once

namespace kcds::web::frame_slots {

constexpr int kSlots = 3;

struct Slots {
  int newest = -1;            // the texture with the last whole frame, -1 before the first
  unsigned serial = 0;        // changes with every new frame in `newest`
  bool reading[kSlots] = {};  // the game copies from it (until its copy finished on the GPU)
};

// A texture the writer may put a new frame into: not the newest, not one the game reads and not
// one whose earlier write has not finished (`writing`); -1 when none is free.
inline int writable(const Slots& slots, const bool writing[kSlots]) {
  for (int slot = 0; slot < kSlots; ++slot) {
    if (slot != slots.newest && !slots.reading[slot] && !writing[slot]) {
      return slot;
    }
  }
  return -1;
}

// A write into `slot` finished on the GPU: its frame is the newest.
inline void written(Slots& slots, int slot) {
  if (slot < 0 || slot >= kSlots) {
    return;
  }
  slots.newest = slot;
  slots.serial++;
}

// The texture the game copies now: the newest, when it holds a frame the game has not copied yet
// (`copied`, the serial it copied last, which this updates); it is then read until end_read. -1
// when there is nothing new.
inline int start_read(Slots& slots, unsigned& copied) {
  if (slots.newest < 0 || slots.serial == copied) {
    return -1;
  }
  copied = slots.serial;
  slots.reading[slots.newest] = true;
  return slots.newest;
}

// The game's copy from `slot` finished on the GPU: the writer may use it again.
inline void end_read(Slots& slots, int slot) {
  if (slot >= 0 && slot < kSlots) {
    slots.reading[slot] = false;
  }
}

}  // namespace kcds::web::frame_slots
