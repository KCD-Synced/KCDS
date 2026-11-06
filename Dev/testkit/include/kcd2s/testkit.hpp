// KCD2-Synced in-house test harness — tiny, header-only, zero external deps.
// See Swap for doctest/Catch2 later if the suite outgrows it.
//
//   KCD2S_TEST(suite, name) { ... KCD2S_CHECK(cond); KCD2S_CHECK_EQ(a, b); ... }
//
// Link the test executable against kcd2s::testkit (which provides main()).
#ifndef KCD2S_TESTKIT_HPP
#define KCD2S_TESTKIT_HPP

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace kcd2s::testkit {

struct Case {
  std::string suite;
  std::string name;
  std::function<void(struct Context&)> fn;
};

// Inline (==one instance across TUs) registry; populated by static Registrar objects.
inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void(struct Context&)> fn) {
    registry().push_back(Case{suite, name, std::move(fn)});
  }
};

// Per-test running context: accumulates failures so one assert failing doesn't abort the case.
struct Context {
  int failures = 0;
  const char* suite = "";
  const char* name = "";

  void fail(const char* file, int line, const std::string& msg) {
    ++failures;
    std::fprintf(stderr, "    FAIL %s:%d  %s\n", file, line, msg.c_str());
  }
};

template <class A, class B>
std::string eq_msg(const char* ae, const char* be, const A& a, const B& b) {
  std::ostringstream os;
  os << "expected (" << ae << " == " << be << "), got " << a << " != " << b;
  return os.str();
}

}  // namespace kcd2s::testkit

#define KCD2S_TEST(suite_, name_)                                                                  \
  static void kcd2s_test_##suite_##_##name_(::kcd2s::testkit::Context&);                           \
  static ::kcd2s::testkit::Registrar kcd2s_reg_##suite_##_##name_(#suite_, #name_,                 \
                                                                  &kcd2s_test_##suite_##_##name_); \
  static void kcd2s_test_##suite_##_##name_(::kcd2s::testkit::Context& kcd2s_ctx)

#define KCD2S_CHECK(cond)                                         \
  do {                                                            \
    if (!(cond))                                                  \
      kcd2s_ctx.fail(__FILE__, __LINE__, "check failed: " #cond); \
  } while (0)

#define KCD2S_CHECK_EQ(a, b)                                                                  \
  do {                                                                                        \
    auto&& kcd2s_a = (a);                                                                     \
    auto&& kcd2s_b = (b);                                                                     \
    if (!((kcd2s_a) == (kcd2s_b)))                                                            \
      kcd2s_ctx.fail(__FILE__, __LINE__, ::kcd2s::testkit::eq_msg(#a, #b, kcd2s_a, kcd2s_b)); \
  } while (0)

#endif  // KCD2S_TESTKIT_HPP
