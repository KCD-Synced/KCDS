#include "kcd2s/integration/world_markers.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

namespace {

MarkerShow sample_marker() {
  MarkerShow marker;
  marker.marker = 9001;
  marker.material = "Materials/Decals/quest_ring";
  marker.shape = "ring";
  marker.position[0] = 120.5f;
  marker.position[1] = -40.25f;
  marker.position[2] = 17.0f;
  marker.rotation[2] = 0.70710678f;
  marker.rotation[3] = 0.70710678f;
  marker.size = 2.5f;
  marker.height = 1.0f;
  marker.spin = 0.5f;
  marker.bob = 0.25f;
  marker.color = 0xff8800ffu;
  return marker;
}

BlipShow sample_blip() {
  BlipShow blip;
  blip.blip = 77;
  blip.type = "Shop";
  blip.markState = 2;
  blip.label = "Obchod u Jana";
  blip.compass = true;
  blip.map = false;
  blip.position[0] = 5.0f;
  blip.position[1] = 6.0f;
  blip.position[2] = 7.0f;
  return blip;
}

}  // namespace

KCD2S_TEST(world_markers, marker_show_round_trips) {
  const MarkerShow marker = sample_marker();
  MarkerShow decoded;
  KCD2S_CHECK(decode_marker_show(encode_marker_show(marker), decoded));
  KCD2S_CHECK(decoded.marker == marker.marker);
  KCD2S_CHECK(decoded.material == marker.material);
  KCD2S_CHECK(decoded.shape == marker.shape);
  KCD2S_CHECK(decoded.position[0] == marker.position[0]);
  KCD2S_CHECK(decoded.position[2] == marker.position[2]);
  KCD2S_CHECK(decoded.rotation[3] == marker.rotation[3]);
  KCD2S_CHECK(decoded.size == marker.size);
  KCD2S_CHECK(decoded.bob == marker.bob);
  KCD2S_CHECK(decoded.color == marker.color);
}

KCD2S_TEST(world_markers, marker_refuses_bad_values) {
  MarkerShow marker = sample_marker();
  marker.material = "has space";
  KCD2S_CHECK(encode_marker_show(marker).empty());
  marker = sample_marker();
  marker.material.clear();
  KCD2S_CHECK(encode_marker_show(marker).empty());
  marker = sample_marker();
  marker.rotation[3] = 0.0f;
  KCD2S_CHECK(encode_marker_show(marker).empty());
  marker = sample_marker();
  marker.size = -1.0f;
  KCD2S_CHECK(encode_marker_show(marker).empty());
  marker = sample_marker();
  marker.position[1] = kMarkerCoordinateMax * 2.0f;
  KCD2S_CHECK(encode_marker_show(marker).empty());
  MarkerShow decoded;
  KCD2S_CHECK(!decode_marker_show({kMarkerShowTag, 1}, decoded));
}

KCD2S_TEST(world_markers, hides_round_trip) {
  std::uint32_t id = 0;
  KCD2S_CHECK(decode_marker_hide(encode_marker_hide(4000000000u), id));
  KCD2S_CHECK(id == 4000000000u);
  KCD2S_CHECK(decode_blip_hide(encode_blip_hide(12), id));
  KCD2S_CHECK(id == 12);
  KCD2S_CHECK(!decode_blip_hide(encode_marker_hide(12), id));
}

KCD2S_TEST(world_markers, blip_show_round_trips) {
  const BlipShow blip = sample_blip();
  BlipShow decoded;
  KCD2S_CHECK(decode_blip_show(encode_blip_show(blip), decoded));
  KCD2S_CHECK(decoded.blip == blip.blip);
  KCD2S_CHECK(decoded.type == blip.type);
  KCD2S_CHECK(decoded.markState == blip.markState);
  KCD2S_CHECK(decoded.label == blip.label);
  KCD2S_CHECK(decoded.compass);
  KCD2S_CHECK(!decoded.map);
  KCD2S_CHECK(decoded.position[1] == blip.position[1]);
}

KCD2S_TEST(world_markers, blip_refuses_bad_values) {
  BlipShow blip = sample_blip();
  blip.type = "Two Words";
  KCD2S_CHECK(encode_blip_show(blip).empty());
  blip = sample_blip();
  blip.label = std::string(kBlipLabelMax + 1, 'a');
  KCD2S_CHECK(encode_blip_show(blip).empty());
  blip = sample_blip();
  blip.label = "line\nbreak";
  KCD2S_CHECK(encode_blip_show(blip).empty());
}
