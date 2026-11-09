#include "hint_width.hpp"

#include <Windows.h>

#include <string>
#include <utility>
#include <vector>

#include "../native_ui/native_ui.hpp"
#include "hint_width_movie.hpp"

namespace kcds::runtime::action_hints {
namespace {

namespace sdk = kcd2s::sdk;

constexpr char kElement[] = "HUD";
constexpr char kClip[] = "kcdsHintWidth";
constexpr char kMovieKey[] = "_kcds/hint-width-1.swf";
constexpr char kMovieUrl[] = "/%USER%/kcds_ui/_kcds/hint-width-1.swf";
// How often the HUD is looked at, and how long a load may take before it is tried again.
constexpr ULONGLONG kLookMs = 500;
constexpr ULONGLONG kLoadRetryMs = 3000;

bool movie_written = false;
bool loaded = false;
ULONGLONG looked_at = 0;
ULONGLONG load_started = 0;

// One NativeUI clip call on the HUD (below its _root).
sdk::Value clip_call(sdk::Value::Array path, const char* action, sdk::Value::Map fields = {}) {
  fields.emplace_back("op", sdk::Value("elementClip"));
  fields.emplace_back("name", sdk::Value(kElement));
  fields.emplace_back("instance", sdk::Value(std::int64_t(0)));
  fields.emplace_back("path", sdk::Value(std::move(path)));
  fields.emplace_back("action", sdk::Value(action));
  return native_ui::request(sdk::Value(std::move(fields)));
}

bool movie_ready() {
  const sdk::Value ready = clip_call({sdk::Value(kClip)}, "get", {{"member", sdk::Value("ready")}});
  return ready.tag() == sdk::Tag::Bool && ready.as_bool();
}

}  // namespace

void tick_hint_width(WidthLogger log) {
  const ULONGLONG now = GetTickCount64();
  if (now - looked_at < kLookMs) {
    return;
  }
  looked_at = now;
  if (!native_ui::element_instance(kElement, 0)) {
    loaded = false;
    return;
  }
  if (movie_ready()) {
    if (!loaded && log) {
      log("HINTS width movie loaded in the HUD");
    }
    loaded = true;
    return;
  }
  loaded = false;
  if (now - load_started < kLoadRetryMs) {
    return;
  }
  if (!movie_written) {
    const std::vector<std::uint8_t> movie = hint_width_movie();
    movie_written = !movie.empty() && native_ui::write_own_file(kMovieKey, movie);
  }
  load_started = now;
  const sdk::Value made = clip_call({}, "create", {{"child", sdk::Value(kClip)}});
  const sdk::Value exists = clip_call({sdk::Value(kClip)}, "exists");
  clip_call({sdk::Value(kClip)}, "invoke",
            {{"method", sdk::Value("loadMovie")},
             {"args", sdk::Value(sdk::Value::Array{sdk::Value(kMovieUrl)})}});
  if (log) {
    log("HINTS width movie loading (written %d, clip made %d, there %d)", int(movie_written),
        int(made.tag() == sdk::Tag::Bool && made.as_bool()),
        int(exists.tag() == sdk::Tag::Bool && exists.as_bool()));
  }
}

}  // namespace kcds::runtime::action_hints
