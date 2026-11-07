#include "../snow_cover.hpp"
#include "../snow_shelter.hpp"

#include <cmath>
#include <string>

#include "kcd2s/testkit.hpp"

namespace snow_cover = kcds::runtime::winter::snow_cover;
namespace snow_shelter = kcds::runtime::winter::snow_shelter;

namespace {

// The lines of the engine's snow shader around its volume and cover, as the game hands them to
// the compiler.
const std::string kShader =
    "min16float fAttenuation = saturate( GetAttenuation(g_SnowVolumeParams.xyz - vPosition.xyz, "
    "g_SnowVolumeParams.w) );\n"
    "fAttenuation *= (fWaterAtten * fWaterAtten);\n"
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

KCD2S_TEST(snow_cover, the_snow_volume_is_a_column_without_snow) {
  const std::string covered = snow_cover::with_full_cover(kShader);
  KCD2S_CHECK(covered.find("min16float " + std::string(snow_cover::kVolume)) != std::string::npos);
  KCD2S_CHECK(covered.find("GetAttenuation") == std::string::npos);
  // The water's part, multiplied in afterwards, stays.
  KCD2S_CHECK(covered.find("fAttenuation *= (fWaterAtten * fWaterAtten);") != std::string::npos);
}

KCD2S_TEST(snow_cover, written_without_spaces_it_is_found_too) {
  const std::string tight =
      "float fAttenuation=GetAttenuation(a,b);float fIceTopZ=1;"
      "OUT.Blending.a=fIceTopZ*saturate(x*8.h);y=2;";
  const std::string covered = snow_cover::with_full_cover(tight);
  KCD2S_CHECK_EQ(covered, "float " + std::string(snow_cover::kVolume) + "float fIceTopZ=1;" +
                              std::string(snow_cover::kCover) + "y=2;");
}

KCD2S_TEST(snow_cover, other_shaders_and_comparisons_are_left_alone) {
  KCD2S_CHECK(snow_cover::with_full_cover("OUT.Color = 1;").empty());
  KCD2S_CHECK(
      snow_cover::with_full_cover("bool b = OUT.Blending.a == fIceTopZ; float fIceTopZ;").empty());
  // The cover without the volume is not the snow shader as KCDS knows it.
  KCD2S_CHECK(snow_cover::with_full_cover("float fIceTopZ=1;OUT.Blending.a=fIceTopZ*2;").empty());
}

KCD2S_TEST(snow_shelter, in_the_open_there_is_no_shelter) {
  const float at[3] = {10.0f, 10.0f, 5.0f};
  const auto shelter = snow_shelter::shelter_at(at, [](float, float, float) { return false; });
  KCD2S_CHECK(!shelter.covered);
}

KCD2S_TEST(snow_shelter, under_a_roof_the_column_covers_the_roof) {
  // A roof 8 m by 6 m from (0, 0) to (8, 6); the camera at (2, 3).
  const auto roof = [](float x, float y, float) {
    return x >= 0.0f && x <= 8.0f && y >= 0.0f && y <= 6.0f;
  };
  const float at[3] = {2.0f, 3.0f, 1.7f};
  const auto shelter = snow_shelter::shelter_at(at, roof);
  KCD2S_CHECK(shelter.covered);
  // Its middle lies inside the roof, and the column reaches every corner of the floor under it
  // within a step and the margin.
  KCD2S_CHECK(shelter.centre[0] > 0.0f && shelter.centre[0] < 8.0f);
  KCD2S_CHECK(shelter.centre[1] > 0.0f && shelter.centre[1] < 6.0f);
  const float corners[4][2] = {{0.0f, 0.0f}, {8.0f, 0.0f}, {0.0f, 6.0f}, {8.0f, 6.0f}};
  for (const auto& corner : corners) {
    const float reach = std::hypot(corner[0] - shelter.centre[0], corner[1] - shelter.centre[1]);
    KCD2S_CHECK(reach <= shelter.radius + snow_shelter::kStep * 2.0f);
  }
  // And it stays a house's size, not the street's.
  KCD2S_CHECK(shelter.radius < 10.0f);
}
