#include "../map_labels_movie.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "../../native_ui/as2_movie_check.hpp"
#include "kcd2s/testkit.hpp"

namespace {
using kcds::runtime::kcd1::map_labels_movie;
namespace check = kcds::runtime::native_ui::as2::check;

bool has(const std::vector<std::string>& texts, const char* text) {
  return std::find(texts.begin(), texts.end(), text) != texts.end();
}
}  // namespace

KCD2S_TEST(kcd1_map_labels_movie, wraps_the_labels_drawing_and_hides_their_faces) {
  std::vector<std::string> texts;
  KCD2S_CHECK(check::read_movie(map_labels_movie(), texts));
  // The map's label class and its drawing, kept under a name of ours and wrapped once; the face
  // hidden while the client asks for it.
  for (const char* name :
       {"MapLabel", "prototype", "SetData", "kcdsSetData", "apply", "arguments", "kcdsMapLabels",
        "hideReputation", "m_Mc", "reputationIcon", "_visible", "ready"}) {
    KCD2S_CHECK(has(texts, name));
  }
}
