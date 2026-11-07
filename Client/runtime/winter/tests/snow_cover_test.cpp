#include "../snow_cover.hpp"

#include <string>

#include "kcd2s/testkit.hpp"

namespace snow_cover = kcds::runtime::winter::snow_cover;

namespace {

// The lines of the engine's snow shader around its cover, as the game hands them to the compiler.
const std::string kShader =
    "min16float fIceTopZ = saturate((pow(vNormal.z, fSnowAmount)- 0.2h) * 10.h);\n"
    "OUT.Blending.r = fBubbles.y;\n"
    "OUT.Blending.a = fIceTopZ * saturate(fFrostVariation * 8.h);\n"
    "OUT.Blending.r += OUT.Blending.b;\n";

}  // namespace

KCD2S_TEST(snow_cover, the_snow_cover_follows_the_amount) {
  const std::string covered = snow_cover::with_full_cover(kShader);
  KCD2S_CHECK(!covered.empty());
  KCD2S_CHECK(covered.find(std::string(snow_cover::kCover)) != std::string::npos);
  KCD2S_CHECK(covered.find("saturate(fFrostVariation * 8.h);") == std::string::npos);
  // The rest stays as it was.
  KCD2S_CHECK(covered.find("OUT.Blending.r = fBubbles.y;\n") != std::string::npos);
  KCD2S_CHECK(covered.find("OUT.Blending.r += OUT.Blending.b;\n") != std::string::npos);
}

KCD2S_TEST(snow_cover, written_without_spaces_it_is_found_too) {
  const std::string tight = "float fIceTopZ=1;OUT.Blending.a=fIceTopZ*saturate(x*8.h);y=2;";
  const std::string covered = snow_cover::with_full_cover(tight);
  KCD2S_CHECK_EQ(covered, "float fIceTopZ=1;" + std::string(snow_cover::kCover) + "y=2;");
}

KCD2S_TEST(snow_cover, other_shaders_and_comparisons_are_left_alone) {
  KCD2S_CHECK(snow_cover::with_full_cover("OUT.Color = 1;").empty());
  KCD2S_CHECK(
      snow_cover::with_full_cover("bool b = OUT.Blending.a == fIceTopZ; float fIceTopZ;").empty());
}
