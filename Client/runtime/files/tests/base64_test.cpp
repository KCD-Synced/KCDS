#include "../base64.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

namespace files = kcds::runtime::files;

KCD2S_TEST(base64, bytes_are_written_as_btoa_writes_them_and_read_back) {
  const std::string text = "KCDS";
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  KCD2S_CHECK_EQ(files::encode_base64(bytes), std::string("S0NEUw=="));
  KCD2S_CHECK_EQ(files::encode_base64({}), std::string());
  for (std::size_t size = 0; size < 8; ++size) {
    std::vector<std::uint8_t> sample;
    for (std::size_t at = 0; at < size; ++at) {
      sample.push_back(std::uint8_t(250 - at * 37));
    }
    std::vector<std::uint8_t> back;
    KCD2S_CHECK(files::decode_base64(files::encode_base64(sample), back));
    KCD2S_CHECK(back == sample);
  }
}
