#include "runtime_profiles.hpp"

#include <filesystem>
#include <fstream>

#include "kcd2s/testkit.hpp"

namespace {

namespace fs = std::filesystem;
using kcds::resources::profiles::sweep;

fs::path fresh_base() {
  const fs::path base = fs::temp_directory_path() / "kcds-profiles-test";
  fs::remove_all(base);
  fs::create_directories(base);
  return base;
}

void make_profile(const fs::path& base, const char* name) {
  fs::create_directories(base / name / "EBWebView");
  std::ofstream(base / name / "EBWebView" / "cache.bin") << "cache";
}

}  // namespace

KCD2S_TEST(resource_profiles, a_runtime_that_no_longer_runs_loses_its_folder) {
  const fs::path base = fresh_base();
  make_profile(base, "100");
  make_profile(base, "200");
  make_profile(base, "300");
  const auto removed = sweep(base.wstring(), 300, [](std::uint32_t pid) { return pid == 200; });
  KCD2S_CHECK(removed == 1);
  KCD2S_CHECK(!fs::exists(base / "100"));
  // A running runtime's folder and our own stay.
  KCD2S_CHECK(fs::exists(base / "200") && fs::exists(base / "300"));
  fs::remove_all(base);
}

KCD2S_TEST(resource_profiles, folders_not_named_after_a_process_stay) {
  const fs::path base = fresh_base();
  make_profile(base, "settings");
  make_profile(base, "12a");
  make_profile(base, "99999999999");
  std::ofstream(base / "400") << "a file, not a profile";
  const auto removed = sweep(base.wstring(), 1, [](std::uint32_t) { return false; });
  KCD2S_CHECK(removed == 0);
  KCD2S_CHECK(fs::exists(base / "settings") && fs::exists(base / "12a"));
  KCD2S_CHECK(fs::exists(base / "99999999999") && fs::exists(base / "400"));
  fs::remove_all(base);
}

KCD2S_TEST(resource_profiles, a_missing_base_removes_nothing) {
  const fs::path base = fs::temp_directory_path() / "kcds-profiles-missing";
  fs::remove_all(base);
  KCD2S_CHECK(sweep(base.wstring(), 1, [](std::uint32_t) { return false; }) == 0);
}

KCD2S_TEST(resource_profiles, a_removed_profile_keeps_its_crash_reports) {
  const fs::path root = fs::temp_directory_path() / "kcds-profiles-reports";
  fs::remove_all(root);
  const fs::path base = root / "KCDS" / "ResourceRuntime";
  const fs::path reports = base / "100" / "EBWebView" / "Crashpad" / "reports";
  fs::create_directories(reports);
  std::ofstream(reports / "a1b2.dmp") << "dump";
  std::ofstream(reports / "notes.txt") << "not a dump";
  KCD2S_CHECK(sweep(base.wstring(), 1, [](std::uint32_t) { return false; }) == 1);
  KCD2S_CHECK(!fs::exists(base / "100"));
  const fs::path kept = root / "KCDS" / "crash" / "webview";
  KCD2S_CHECK(fs::exists(kept / "100-a1b2.dmp"));
  KCD2S_CHECK(!fs::exists(kept / "100-notes.txt"));
  fs::remove_all(root);
}
