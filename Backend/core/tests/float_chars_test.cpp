#include "kcd2s/core/float_chars.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/testkit.hpp"

using kcd2s::core::float_from_chars;
using kcd2s::core::detail::strtod_from_chars;

namespace {

struct Read {
  double value = 0.0;
  std::size_t used = 0;
  std::errc error{};
};

template <class T>
Read read_with(bool standard, const std::string& text, T start = T(-7)) {
  T value = start;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto result =
      standard ? float_from_chars(first, last, value) : strtod_from_chars(first, last, value);
  return {double(value), std::size_t(result.ptr - first), result.ec};
}

}  // namespace

KCD2S_TEST(float_chars, reads_numbers_as_from_chars) {
  struct Case {
    const char* text;
    double value;
    std::size_t used;
  };
  const std::vector<Case> cases{
      {"1.5", 1.5, 3},
      {"-0.25", -0.25, 5},
      {"3", 3.0, 1},
      {"12.5e2", 1250.0, 6},
      {"1E-2", 0.01, 4},
      {".5", 0.5, 2},
      {"5.", 5.0, 2},
      {"7.25,8", 7.25, 4},
      {"2e", 2.0, 1},
      {"2e+", 2.0, 1},
      {"1.5x", 1.5, 3},
      {"0x10", 0.0, 1},
      {"6.000000001", 6.000000001, 11},
  };
  for (const auto& one : cases) {
    for (const bool standard : {true, false}) {
      const Read read = read_with<double>(standard, one.text);
      KCD2S_CHECK(read.error == std::errc{});
      KCD2S_CHECK(std::fabs(read.value - one.value) < 1e-12);
      KCD2S_CHECK_EQ(read.used, one.used);
    }
  }
}

KCD2S_TEST(float_chars, refuses_what_from_chars_refuses) {
  for (const char* text : {"", "+1", " 1", "-", ".", "e5", "abc", "-.e1"}) {
    for (const bool standard : {true, false}) {
      const Read read = read_with<double>(standard, text);
      KCD2S_CHECK(read.error == std::errc::invalid_argument);
      KCD2S_CHECK_EQ(read.used, std::size_t{0});
      KCD2S_CHECK_EQ(read.value, -7.0);  // unchanged
    }
  }
}

KCD2S_TEST(float_chars, out_of_range_leaves_the_value) {
  for (const bool standard : {true, false}) {
    const Read big = read_with<float>(standard, "1e60");
    KCD2S_CHECK(big.error == std::errc::result_out_of_range);
    KCD2S_CHECK_EQ(big.value, -7.0);
    KCD2S_CHECK_EQ(big.used, std::size_t{4});
    const Read huge = read_with<double>(standard, "1e400");
    KCD2S_CHECK(huge.error == std::errc::result_out_of_range);
    KCD2S_CHECK_EQ(huge.value, -7.0);
  }
}

KCD2S_TEST(float_chars, infinity_and_nan) {
  for (const bool standard : {true, false}) {
    const Read inf = read_with<float>(standard, "-inf");
    KCD2S_CHECK(inf.error == std::errc{});
    KCD2S_CHECK(std::isinf(inf.value) && inf.value < 0);
    KCD2S_CHECK_EQ(inf.used, std::size_t{4});
    const Read infinity = read_with<double>(standard, "Infinity!");
    KCD2S_CHECK(std::isinf(infinity.value));
    KCD2S_CHECK_EQ(infinity.used, std::size_t{8});
    const Read nan = read_with<double>(standard, "nan");
    KCD2S_CHECK(std::isnan(nan.value));
  }
}

KCD2S_TEST(float_chars, reads_only_its_range) {
  // The text goes on past `last`: only "12" is read.
  const char text[] = "12345";
  for (const bool standard : {true, false}) {
    double value = 0;
    const auto result = standard ? float_from_chars(text, text + 2, value)
                                 : strtod_from_chars(text, text + 2, value);
    KCD2S_CHECK_EQ(value, 12.0);
    KCD2S_CHECK(result.ptr == text + 2);
  }
}
