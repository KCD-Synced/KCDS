// The client takes the download mark off its own programs only.
#include "download_marks.hpp"

#include "kcd2s/testkit.hpp"

using kcds::runtime::install::takes_mark_off;

KCD2S_TEST(download_marks, programs_of_any_case) {
  KCD2S_CHECK(takes_mark_off(L"kcds-web-helper.exe"));
  KCD2S_CHECK(takes_mark_off(L"libcef.dll"));
  KCD2S_CHECK(takes_mark_off(L"KCDS-LAUNCHER.EXE"));
  KCD2S_CHECK(takes_mark_off(L"Chrome_Elf.Dll"));
}

KCD2S_TEST(download_marks, other_files_keep_it) {
  KCD2S_CHECK(!takes_mark_off(L"resources.pak"));
  KCD2S_CHECK(!takes_mark_off(L"icudtl.dat"));
  KCD2S_CHECK(!takes_mark_off(L"client.log"));
  KCD2S_CHECK(!takes_mark_off(L"helper.exe.config"));
  KCD2S_CHECK(!takes_mark_off(L".exe"));
  KCD2S_CHECK(!takes_mark_off(L""));
}
