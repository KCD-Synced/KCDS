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

KCD2S_TEST(snow_cover, the_snow_volume_is_a_box_without_snow) {
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

namespace {

// Whether the box the shader reads from a shelter's code leaves (x, y) without snow, the way the
// shader decides it (its soft edge aside).
bool bare_at(const snow_shelter::Shelter& shelter, float x, float y) {
  const float code = snow_cover::box_code(shelter.half_length, shelter.half_width, shelter.turn);
  const snow_cover::Box box = snow_cover::box_of(code);
  const float away[2] = {x - shelter.centre[0], y - shelter.centre[1]};
  const float along = away[0] * std::cos(box.turn) + away[1] * std::sin(box.turn);
  const float across = -away[0] * std::sin(box.turn) + away[1] * std::cos(box.turn);
  return std::fabs(along) <= box.half_length && std::fabs(across) <= box.half_width;
}

// A point turned by `turn` radians about the origin.
void turned(float turn, float x, float y, float out[2]) {
  out[0] = x * std::cos(turn) - y * std::sin(turn);
  out[1] = x * std::sin(turn) + y * std::cos(turn);
}

}  // namespace

KCD2S_TEST(snow_shelter, under_a_roof_the_box_covers_the_roof) {
  // A roof 8 m by 6 m from (0, 0) to (8, 6); the camera at (2, 3).
  const auto roof = [](float x, float y, float) {
    return x >= 0.0f && x <= 8.0f && y >= 0.0f && y <= 6.0f;
  };
  const float at[3] = {2.0f, 3.0f, 1.7f};
  const auto shelter = snow_shelter::shelter_at(at, roof);
  KCD2S_CHECK(shelter.covered);
  // The floor under it is bare to its corners.
  for (float x = 0.0f; x <= 8.0f; x += 0.5f) {
    for (float y = 0.0f; y <= 6.0f; y += 0.5f) {
      KCD2S_CHECK(bare_at(shelter, x, y));
    }
  }
  // Two metres past its edges the snow lies.
  KCD2S_CHECK(!bare_at(shelter, -2.5f, 3.0f));
  KCD2S_CHECK(!bare_at(shelter, 10.5f, 3.0f));
  KCD2S_CHECK(!bare_at(shelter, 4.0f, -2.5f));
  KCD2S_CHECK(!bare_at(shelter, 4.0f, 8.5f));
}

KCD2S_TEST(snow_shelter, a_shed_beside_the_camera_in_the_open_stays_bare) {
  // The same 8 m by 6 m roof; the camera 5 m off its side, in the open.
  const auto roof = [](float x, float y, float) {
    return x >= 0.0f && x <= 8.0f && y >= 0.0f && y <= 6.0f;
  };
  const float at[3] = {4.0f, -5.0f, 1.7f};
  const auto shelter = snow_shelter::shelter_near(at, roof, roof);
  KCD2S_CHECK(shelter.covered);
  KCD2S_CHECK(!shelter.over_camera);
  for (float x = 0.0f; x <= 8.0f; x += 0.5f) {
    for (float y = 0.0f; y <= 6.0f; y += 0.5f) {
      KCD2S_CHECK(bare_at(shelter, x, y));
    }
  }
  // Where the camera stands, in the open, the snow lies.
  KCD2S_CHECK(!bare_at(shelter, at[0], at[1]));
  // Under it the roof is over the camera itself.
  const float inside[3] = {2.0f, 3.0f, 1.7f};
  KCD2S_CHECK(snow_shelter::shelter_near(inside, roof, roof).over_camera);
  // No roof within reach: nothing is bare.
  const float far[3] = {40.0f, 40.0f, 1.7f};
  KCD2S_CHECK(!snow_shelter::shelter_near(far, roof, roof).covered);
}

KCD2S_TEST(snow_shelter, a_long_roof_turned_off_the_axes_leaves_the_street_beside_it) {
  // A smithy's roof joined to the next houses: 24 m long and 4 m deep, turned 30 degrees, the
  // camera under it 3 m from one end. A circle of its reach took the snow off the street.
  const float turn = 0.5235988f;
  const auto roof = [turn](float x, float y, float) {
    float local[2];
    turned(-turn, x, y, local);
    return local[0] >= 0.0f && local[0] <= 24.0f && local[1] >= 0.0f && local[1] <= 4.0f;
  };
  float at[3] = {0.0f, 0.0f, 1.7f};
  turned(turn, 3.0f, 2.0f, at);
  const auto shelter = snow_shelter::shelter_at(at, roof);
  KCD2S_CHECK(shelter.covered);
  // Under the roof as far as the rays reach (16 m), bare.
  for (float along = 0.0f; along <= 18.0f; along += 1.0f) {
    for (float across = 0.0f; across <= 4.0f; across += 1.0f) {
      float point[2];
      turned(turn, along, across, point);
      KCD2S_CHECK(bare_at(shelter, point[0], point[1]));
    }
  }
  // The street 3 m in front of it keeps its snow along the whole roof.
  for (float along = 0.0f; along <= 18.0f; along += 1.0f) {
    float street[2];
    turned(turn, along, -3.0f, street);
    KCD2S_CHECK(!bare_at(shelter, street[0], street[1]));
    turned(turn, along, 7.0f, street);
    KCD2S_CHECK(!bare_at(shelter, street[0], street[1]));
  }
}

KCD2S_TEST(snow_cover, a_box_code_is_read_back_whole_and_stays_low) {
  const float code = snow_cover::box_code(5.1f, 2.0f, 1.0f);
  const snow_cover::Box box = snow_cover::box_of(code);
  KCD2S_CHECK_EQ(box.half_length, 5.25f);
  KCD2S_CHECK_EQ(box.half_width, 2.0f);
  KCD2S_CHECK(std::fabs(box.turn - 1.0f) < 0.0125f);
  // The largest box is below 512 (the snow volume stays around the world), and no box is 0.
  KCD2S_CHECK(snow_cover::box_code(100.0f, 100.0f, 3.1f) < 512.0f);
  KCD2S_CHECK(snow_cover::box_code(0.0f, 0.0f, 0.0f) > 0.0f);
  // A turn of a half circle is the same box as none.
  KCD2S_CHECK_EQ(snow_cover::box_of(snow_cover::box_code(1.0f, 1.0f, 3.14159265f)).turn, 0.0f);
}
