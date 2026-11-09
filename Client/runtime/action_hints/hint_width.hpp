// The width of the game's action hints: the HUD draws a hint's text in a field of a fixed width,
// right-aligned to its key, and cut a longer text short. A small movie of the client's own
// (hint_width_movie.hpp) is loaded into the HUD's _root.kcdsHintWidth; it wraps the hint's
// drawing (ActionHintNotification.drawInt) so the field grows to its text.
//
// The movie is kept in memory as one of the client's own files (native_ui::write_own_file) and
// loaded as /%USER%/kcds_ui/_kcds/..., which the movie loader opens through NativeUI's file hook.
#ifndef KCDS_RUNTIME_ACTION_HINTS_HINT_WIDTH_HPP
#define KCDS_RUNTIME_ACTION_HINTS_HINT_WIDTH_HPP

namespace kcds::runtime::action_hints {

using WidthLogger = void (*)(const char* format, ...);

// Engine thread, every frame in the world: loads the movie into the HUD once it is there (again
// after the HUD was loaded anew).
void tick_hint_width(WidthLogger log);

}  // namespace kcds::runtime::action_hints

#endif
