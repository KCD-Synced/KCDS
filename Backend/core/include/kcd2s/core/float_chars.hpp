#pragma once
// Reading a float or double from text the way std::from_chars does (general format, no leading
// whitespace or '+', the C locale's decimal point), on every standard library. libstdc++ and the
// Microsoft library have std::from_chars for floating point; libc++, the library of macOS, does
// not, so there the number is scanned here and converted with strtod.
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <system_error>
#include <type_traits>

namespace kcd2s::core {

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
inline constexpr bool kStandardFloatFromChars = true;
#else
inline constexpr bool kStandardFloatFromChars = false;
#endif

namespace detail {

inline bool ascii_digit(char c) {
  return c >= '0' && c <= '9';
}

inline bool same_letters(const char* first, const char* last, const char* lower) {
  for (; *lower; ++lower, ++first) {
    if (first == last) {
      return false;
    }
    const char c = (*first >= 'A' && *first <= 'Z') ? char(*first - 'A' + 'a') : *first;
    if (c != *lower) {
      return false;
    }
  }
  return true;
}

// The length of the longest prefix of [first, last) that from_chars reads as a number in the
// general format: [-] digits [. digits] [e|E [+|-] digits], or [-] inf / infinity / nan. 0 when
// there is none.
inline std::size_t number_length(const char* first, const char* last) {
  const char* at = first;
  if (at != last && *at == '-') {
    ++at;
  }
  if (same_letters(at, last, "infinity")) {
    return std::size_t(at - first) + 8;
  }
  if (same_letters(at, last, "inf") || same_letters(at, last, "nan")) {
    return std::size_t(at - first) + 3;
  }
  const char* digits_start = at;
  while (at != last && ascii_digit(*at)) {
    ++at;
  }
  bool any_digit = at != digits_start;
  if (at != last && *at == '.') {
    const char* fraction = at + 1;
    const char* after = fraction;
    while (after != last && ascii_digit(*after)) {
      ++after;
    }
    if (any_digit || after != fraction) {
      any_digit = true;
      at = after;
    }
  }
  if (!any_digit) {
    return 0;
  }
  if (at != last && (*at == 'e' || *at == 'E')) {
    const char* exponent = at + 1;
    if (exponent != last && (*exponent == '+' || *exponent == '-')) {
      ++exponent;
    }
    const char* after = exponent;
    while (after != last && ascii_digit(*after)) {
      ++after;
    }
    if (after != exponent) {
      at = after;
    }
  }
  return std::size_t(at - first);
}

// The scan above converted with strtod: what float_from_chars uses without a standard
// from_chars for floating point (and what its tests check everywhere).
template <class T>
std::from_chars_result strtod_from_chars(const char* first, const char* last, T& value) {
  const std::size_t length = number_length(first, last);
  if (length == 0) {
    return {first, std::errc::invalid_argument};
  }
  const std::string text(first, first + length);
  errno = 0;
  char* end = nullptr;
  const double read = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) {
    return {first, std::errc::invalid_argument};
  }
  const char* ptr = first + (end - text.c_str());
  if (errno == ERANGE) {
    return {ptr, std::errc::result_out_of_range};
  }
  if constexpr (std::is_same_v<T, float>) {
    const float narrowed = static_cast<float>(read);
    // A finite double beyond float's range turns into an infinity: out of range, as from_chars.
    if (std::isinf(narrowed) && !std::isinf(read)) {
      return {ptr, std::errc::result_out_of_range};
    }
    value = narrowed;
  } else {
    value = read;
  }
  return {ptr, std::errc{}};
}

}  // namespace detail

// std::from_chars(first, last, value) for float and double, on every standard library: the
// number at `first` (no leading whitespace or '+'), `ptr` after it, `ec` invalid_argument when
// there is none and result_out_of_range when it does not fit (`value` then unchanged).
template <class T>
std::from_chars_result float_from_chars(const char* first, const char* last, T& value) {
  static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>,
                "float_from_chars reads float or double");
  if constexpr (kStandardFloatFromChars) {
    return std::from_chars(first, last, value);
  } else {
    return detail::strtod_from_chars(first, last, value);
  }
}

}  // namespace kcd2s::core
