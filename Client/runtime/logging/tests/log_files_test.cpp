#include "../log_files.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "kcd2s/testkit.hpp"

namespace {

namespace fs = std::filesystem;
using namespace kcds::game_client;

fs::path fresh_folder(const char* name) {
  const fs::path folder = fs::temp_directory_path() / name;
  fs::remove_all(folder);
  fs::create_directories(folder);
  return folder;
}

// A file of `bytes` bytes, written `age` minutes ago.
void make_file(const fs::path& path, std::size_t bytes, int age) {
  std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
  fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::minutes(age));
}

std::string read_all(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

KCD2S_TEST(log_files, the_earlier_part_sits_beside_the_log) {
  KCD2S_CHECK(log_files::earlier_part("logs/client-42.log") == fs::path("logs/client-42.old.log"));
}

KCD2S_TEST(log_files, only_the_newest_logs_up_to_the_count_stay) {
  const fs::path folder = fresh_folder("kcds-log-files-count");
  make_file(folder / "client-1.log", 10, 30);
  make_file(folder / "client-2.log", 10, 20);
  make_file(folder / "client-2.old.log", 10, 25);
  make_file(folder / "client-3.log", 10, 10);
  const auto removed = log_files::prune(folder, 2, 1000);
  KCD2S_CHECK(removed == 2);
  KCD2S_CHECK(fs::exists(folder / "client-3.log") && fs::exists(folder / "client-2.log"));
  KCD2S_CHECK(!fs::exists(folder / "client-1.log") && !fs::exists(folder / "client-2.old.log"));
  fs::remove_all(folder);
}

KCD2S_TEST(log_files, the_folder_stays_under_its_byte_budget) {
  const fs::path folder = fresh_folder("kcds-log-files-bytes");
  make_file(folder / "client-1.log", 400, 30);
  make_file(folder / "client-2.log", 400, 20);
  make_file(folder / "client-3.log", 400, 10);
  const auto removed = log_files::prune(folder, 20, 1000);
  KCD2S_CHECK(removed == 1);
  KCD2S_CHECK(!fs::exists(folder / "client-1.log"));
  KCD2S_CHECK(fs::exists(folder / "client-2.log") && fs::exists(folder / "client-3.log"));
  fs::remove_all(folder);
}

KCD2S_TEST(log_files, the_newest_log_stays_even_over_the_budget_and_others_are_left_alone) {
  const fs::path folder = fresh_folder("kcds-log-files-newest");
  make_file(folder / "client-1.log", 5000, 10);
  make_file(folder / "launcher.log", 5000, 20);
  make_file(folder / "client-notes.txt", 5000, 30);
  const auto removed = log_files::prune(folder, 0, 100);
  KCD2S_CHECK(removed == 0);
  KCD2S_CHECK(fs::exists(folder / "client-1.log"));
  KCD2S_CHECK(fs::exists(folder / "launcher.log") && fs::exists(folder / "client-notes.txt"));
  fs::remove_all(folder);
}

KCD2S_TEST(log_files, a_full_log_moves_to_its_earlier_part_and_starts_over) {
  const fs::path folder = fresh_folder("kcds-log-files-roll");
  const fs::path log = folder / "client-7.log";
  make_file(log_files::earlier_part(log), 3, 60);
  std::FILE* file = std::fopen(log.string().c_str(), "wb");
  KCD2S_CHECK(file != nullptr);
  std::fputs("first lines\n", file);
  KCD2S_CHECK(log_files::start_over(file, log));
  std::fputs("after\n", file);
  std::fclose(file);
  KCD2S_CHECK(read_all(log_files::earlier_part(log)) == "first lines\n");
  KCD2S_CHECK(read_all(log) == "after\n");
  fs::remove_all(folder);
}
