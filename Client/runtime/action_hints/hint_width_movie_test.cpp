#include "hint_width_movie.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "../native_ui/as2_movie_check.hpp"
#include "kcd2s/testkit.hpp"

namespace {
using kcds::runtime::action_hints::hint_width_movie;
namespace check = kcds::runtime::native_ui::as2::check;

bool has(const std::vector<std::string>& texts, const char* text) {
  return std::find(texts.begin(), texts.end(), text) != texts.end();
}
}  // namespace

KCD2S_TEST(hint_width_movie, wraps_the_hints_drawing_and_widens_the_text_field) {
  std::vector<std::string> texts;
  KCD2S_CHECK(check::read_movie(hint_width_movie(), texts));
  // The game's hint class and its drawing, kept under a name of ours and wrapped once.
  for (const char* name : {"ActionHintNotification", "prototype", "drawInt", "kcdsDrawInt", "m_mc",
                           "tfText", "_width", "textWidth", "ready"}) {
    KCD2S_CHECK(has(texts, name));
  }
}
