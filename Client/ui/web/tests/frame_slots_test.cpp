#include "../frame_slots.hpp"

#include "kcd2s/testkit.hpp"

namespace slots = kcds::web::frame_slots;

KCD2S_TEST(frame_slots, nothing_is_read_before_the_first_whole_frame) {
  slots::Slots state;
  unsigned copied = 0;
  const bool writing[slots::kSlots] = {};
  KCD2S_CHECK_EQ(slots::start_read(state, copied), -1);
  KCD2S_CHECK_EQ(slots::writable(state, writing), 0);
}

KCD2S_TEST(frame_slots, the_game_copies_each_new_frame_once) {
  slots::Slots state;
  unsigned copied = 0;
  slots::written(state, 0);
  KCD2S_CHECK_EQ(slots::start_read(state, copied), 0);
  slots::end_read(state, 0);
  // The same frame again: nothing to copy.
  KCD2S_CHECK_EQ(slots::start_read(state, copied), -1);
  slots::written(state, 1);
  KCD2S_CHECK_EQ(slots::start_read(state, copied), 1);
}

KCD2S_TEST(frame_slots, the_writer_never_takes_the_newest_or_a_read_texture) {
  slots::Slots state;
  unsigned copied = 0;
  bool writing[slots::kSlots] = {};
  slots::written(state, 0);
  KCD2S_CHECK_EQ(slots::start_read(state, copied), 0);
  // The game reads 0 (also the newest): the writer goes to 1.
  KCD2S_CHECK_EQ(slots::writable(state, writing), 1);
  writing[1] = true;
  // 1 still being written: 2.
  KCD2S_CHECK_EQ(slots::writable(state, writing), 2);
  writing[2] = true;
  // All taken: the frame waits.
  KCD2S_CHECK_EQ(slots::writable(state, writing), -1);
  writing[1] = false;
  slots::written(state, 1);
  // 0 is still read, 1 is the newest, 2 is being written: nothing free until one ends.
  KCD2S_CHECK_EQ(slots::writable(state, writing), -1);
  slots::end_read(state, 0);
  KCD2S_CHECK_EQ(slots::writable(state, writing), 0);
}

KCD2S_TEST(frame_slots, with_one_read_at_a_time_a_texture_is_always_free) {
  // The game keeps one copy in flight per view: newest + read + one write leave none, but once the
  // write finishes it becomes the newest and the old newest is free.
  slots::Slots state;
  unsigned copied = 0;
  bool writing[slots::kSlots] = {};
  for (int frame = 0; frame < 30; ++frame) {
    const int slot = slots::writable(state, writing);
    KCD2S_CHECK(slot >= 0);
    slots::written(state, slot);
    const int read = slots::start_read(state, copied);
    KCD2S_CHECK_EQ(read, slot);
    if (frame % 2 == 0) {
      slots::end_read(state, read);
    } else {
      // A slow copy: still read while the next frame is written.
      const int next = slots::writable(state, writing);
      KCD2S_CHECK(next >= 0 && next != read);
      slots::end_read(state, read);
    }
  }
}
