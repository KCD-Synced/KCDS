// What a crash report tells about the moment of the crash, so the next one can be fixed: the stage
// of the mod's frame, whether the crash was on the frame's thread, the last command a client script
// sent and its resource, and the crashing thread's stack as module+offset.
//
// note_script() runs for every script message; it only copies two short strings. annotate() runs in
// the crashing process just before the report is written.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kcds::runtime::crash {

// The command a client script just sent and its resource ("" when the message names none).
void note_script(const std::string& op, const std::string& resource);

// The thread that runs the mod's frame.
void note_frame_thread(std::uint32_t thread_id);

// For the crash backend: tags and the stack, from the crashing thread's EXCEPTION_POINTERS.
void annotate(const void* exception_pointers,
              std::vector<std::pair<std::string, std::string>>& tags, std::string& stack);

}  // namespace kcds::runtime::crash
