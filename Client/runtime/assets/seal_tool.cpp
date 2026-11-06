// kcds-asset-seal: seals a zip pak into a .kcdsasset (assets/asset_container.hpp).
//
//   kcds-asset-seal <pak> <resource> <out> --secret <secret>
//   kcds-asset-seal <pak> <resource> <out> --this-machine
//
// --this-machine uses the Asset/Animation Studio's local secret ("kcds-studio-local:" and this
// machine's hardware id, studio/studio_scene.cpp), so the container opens only on this machine.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../identity/hardware_id.hpp"
#include "asset_container.hpp"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: kcds-asset-seal <pak> <resource> <out> (--secret <secret> | "
               "--this-machine)\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    return usage();
  }
  const std::string mode = argv[4];
  std::string secret;
  if (mode == "--secret" && argc >= 6) {
    secret = argv[5];
  } else if (mode == "--this-machine") {
    secret = "kcds-studio-local:" + kcds::runtime::identity::hardware_id();
  } else {
    return usage();
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const std::vector<std::uint8_t> pak((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
  const auto sealed = kcds::runtime::assets::container::seal(pak, argv[2], secret);
  if (sealed.empty()) {
    std::fprintf(stderr, "sealing failed\n");
    return 1;
  }
  std::ofstream out(argv[3], std::ios::binary);
  out.write(reinterpret_cast<const char*>(sealed.data()), std::streamsize(sealed.size()));
  std::printf("sealed %zu bytes for %s into %s\n", pak.size(), argv[2], argv[3]);
  return out ? 0 : 1;
}
