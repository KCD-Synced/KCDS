// The game client's resource download cache: resources/download_cache.hpp.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "../local/resources/download_cache.hpp"
#include "kcd2s/testkit.hpp"

namespace {

namespace fs = std::filesystem;
using kcd2s::local::resources::DownloadCache;
using kcd2s::sdk::Value;

// A stand-in for SHA-256 with the same form (64 lowercase hex characters): FNV-1a, four times.
std::string fake_hash(const std::string& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (std::uint64_t round = 0; round < 4; ++round) {
    std::uint64_t value = 1469598103934665603ull ^ round;
    for (const char c : bytes) {
      value = (value ^ std::uint8_t(c)) * 1099511628211ull;
    }
    for (int shift = 60; shift >= 0; shift -= 4) {
      text.push_back(kHex[(value >> shift) & 15]);
    }
  }
  return text;
}

std::string base64(const std::string& bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const std::uint32_t n = (std::uint32_t(std::uint8_t(bytes[i])) << 16) |
                            (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8) |
                            std::uint32_t(std::uint8_t(bytes[i + 2]));
    text += kAlphabet[n >> 18];
    text += kAlphabet[(n >> 12) & 63];
    text += kAlphabet[(n >> 6) & 63];
    text += kAlphabet[n & 63];
  }
  if (bytes.size() - i == 1) {
    const std::uint32_t n = std::uint32_t(std::uint8_t(bytes[i])) << 16;
    text += kAlphabet[n >> 18];
    text += kAlphabet[(n >> 12) & 63];
    text += "==";
  } else if (bytes.size() - i == 2) {
    const std::uint32_t n = (std::uint32_t(std::uint8_t(bytes[i])) << 16) |
                            (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8);
    text += kAlphabet[n >> 18];
    text += kAlphabet[(n >> 12) & 63];
    text += kAlphabet[(n >> 6) & 63];
    text += '=';
  }
  return text;
}

Value request(const std::string& hash, std::int64_t offset, std::int64_t length) {
  return Value(Value::Map{{"op", Value("fileRequest")},
                          {"revision", Value(std::int64_t(3))},
                          {"hash", Value(hash)},
                          {"offset", Value(offset)},
                          {"length", Value(length)},
                          {"requestId", Value(std::int64_t(17))}});
}

Value chunk(const std::string& hash, const std::string& file, std::size_t offset,
            std::size_t length) {
  return Value(Value::Map{{"op", Value("fileData")},
                          {"revision", Value(std::int64_t(3))},
                          {"hash", Value(hash)},
                          {"offset", Value(std::int64_t(offset))},
                          {"size", Value(std::int64_t(file.size()))},
                          {"data", Value(base64(file.substr(offset, length)))},
                          {"requestId", Value(std::int64_t(9))}});
}

std::string field(const Value& value, const char* name) {
  const auto* found = kcd2s::integration::script_field(value, name);
  return found && found->tag() == kcd2s::sdk::Tag::Str ? found->as_str() : std::string();
}

fs::path fresh_folder(const char* name) {
  const fs::path folder = fs::temp_directory_path() / name;
  std::error_code error;
  fs::remove_all(folder, error);
  return folder;
}

}  // namespace

KCD2S_TEST(download_cache, a_complete_verified_file_is_kept_and_answered_next_time) {
  const fs::path folder = fresh_folder("kcds-download-cache-kept");
  // 20 000 bytes: two chunks, the second one shorter than the chunk size.
  std::string file;
  for (int i = 0; i < 20000; ++i) {
    file.push_back(char(i * 7 % 251));
  }
  const std::string hash = fake_hash(file);
  {
    DownloadCache cache(folder, fake_hash);
    KCD2S_CHECK(!cache.answer(request(hash, 0, 16384)));
    cache.keep(chunk(hash, file, 16384, 3616));
    KCD2S_CHECK(!fs::exists(folder / hash));
    // A chunk twice changes nothing.
    cache.keep(chunk(hash, file, 16384, 3616));
    cache.keep(chunk(hash, file, 0, 16384));
    KCD2S_CHECK(fs::exists(folder / hash) && fs::file_size(folder / hash) == file.size());
  }
  DownloadCache next(folder, fake_hash);
  const auto first = next.answer(request(hash, 0, 16384));
  KCD2S_CHECK(first.has_value());
  if (first) {
    KCD2S_CHECK_EQ(field(*first, "op"), std::string("fileData"));
    KCD2S_CHECK_EQ(field(*first, "hash"), hash);
    KCD2S_CHECK_EQ(field(*first, "data"), base64(file.substr(0, 16384)));
    const auto* id = kcd2s::integration::script_field(*first, "requestId");
    KCD2S_CHECK(id && id->as_int() == 17);
    const auto* size = kcd2s::integration::script_field(*first, "size");
    KCD2S_CHECK(size && size->as_int() == 20000);
  }
  // The last chunk: only the bytes that are left; every base64 remainder decodes back.
  const auto last = next.answer(request(hash, 16384, 16384));
  KCD2S_CHECK(last && field(*last, "data") == base64(file.substr(16384)));
  KCD2S_CHECK(!next.answer(request(hash, 20001, 10)));
  KCD2S_CHECK(!next.answer(request(hash, 0, 20000)));
  KCD2S_CHECK_EQ(next.hits(), std::uint64_t{2});
  for (const std::size_t size : {std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(4)}) {
    const std::string small = file.substr(100, size);
    const std::string small_hash = fake_hash(small);
    next.keep(chunk(small_hash, small, 0, size));
    DownloadCache again(folder, fake_hash);
    const auto answered = again.answer(request(small_hash, 0, 16));
    KCD2S_CHECK(answered && field(*answered, "data") == base64(small));
  }
  fs::remove_all(folder);
}

KCD2S_TEST(download_cache, wrong_or_damaged_files_are_never_answered) {
  const fs::path folder = fresh_folder("kcds-download-cache-damaged");
  const std::string file = "the server's real file";
  const std::string hash = fake_hash(file);
  DownloadCache cache(folder, fake_hash);
  // Bytes that are not the file the hash names: not kept.
  const std::string wrong(file.size(), 'x');
  cache.keep(chunk(hash, wrong, 0, wrong.size()));
  KCD2S_CHECK(!fs::exists(folder / hash));
  cache.keep(chunk(hash, file, 0, file.size()));
  KCD2S_CHECK(fs::exists(folder / hash));
  // Changed on disk afterwards: deleted on the next read and asked from the server.
  {
    std::ofstream out(folder / hash, std::ios::binary | std::ios::trunc);
    out << "tampered with on the disk";
  }
  DownloadCache next(folder, fake_hash);
  KCD2S_CHECK(!next.answer(request(hash, 0, 16)));
  KCD2S_CHECK(!fs::exists(folder / hash));
  // Malformed requests and replies are ignored.
  KCD2S_CHECK(!next.answer(request("../escape", 0, 16)));
  next.keep(Value(Value::Map{{"op", Value("fileData")}, {"hash", Value(hash)}}));
  KCD2S_CHECK(!fs::exists(folder / hash));
  fs::remove_all(folder);
}

KCD2S_TEST(download_cache, the_folder_stays_under_its_limit_oldest_first) {
  const fs::path folder = fresh_folder("kcds-download-cache-limit");
  DownloadCache cache(folder, fake_hash, {}, 250);
  std::string hashes[3];
  for (int i = 0; i < 3; ++i) {
    const std::string file(100, char('a' + i));
    hashes[i] = fake_hash(file);
    cache.keep(chunk(hashes[i], file, 0, file.size()));
    // Distinct write times, oldest first.
    fs::last_write_time(folder / hashes[i],
                        fs::file_time_type::clock::now() - std::chrono::hours(3 - i));
  }
  // The third write found 300 bytes: the oldest went.
  KCD2S_CHECK(!fs::exists(folder / hashes[0]));
  KCD2S_CHECK(fs::exists(folder / hashes[1]) && fs::exists(folder / hashes[2]));
  fs::remove_all(folder);
}

// a big file that came whole (over HTTP) is moved in only when it is the file
// its hash names, verified on the disk in pieces, and its chunks are read from the disk.
KCD2S_TEST(download_cache, a_big_file_stays_on_the_disk) {
  const fs::path folder = fresh_folder("kcds-download-cache-big");
  fs::create_directories(folder);
  const auto file_hash = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return fake_hash(bytes);
  };
  DownloadCache cache(folder, fake_hash, file_hash);
  std::string file(std::size_t(DownloadCache::kInMemoryBytes) + 4096, '\0');
  for (std::size_t i = 0; i < file.size(); ++i) {
    file[i] = char('a' + i % 23);
  }
  const std::string hash = fake_hash(file);
  const auto write = [&](const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
  };
  // The wrong bytes: deleted, not kept.
  write(folder / "wrong.part", std::string(file.size(), 'x'));
  KCD2S_CHECK(!cache.put_file(hash, folder / "wrong.part"));
  KCD2S_CHECK(!fs::exists(folder / "wrong.part") && !fs::exists(folder / hash));
  // The right ones: in place, verified, and every chunk read from the disk.
  write(folder / "right.part", file);
  KCD2S_CHECK(cache.put_file(hash, folder / "right.part"));
  KCD2S_CHECK(fs::exists(folder / hash) && cache.ensure(hash));
  const std::int64_t at = std::int64_t(file.size()) - 100;
  const auto answer = cache.answer(request(hash, at, 16 * 1024));
  KCD2S_CHECK(answer.has_value());
  // A new session hashes the file again; changed on the disk, it is deleted.
  {
    std::fstream out(folder / hash, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(10);
    out.put('#');
  }
  DownloadCache next(folder, fake_hash, file_hash);
  KCD2S_CHECK(!next.ensure(hash));
  KCD2S_CHECK(!fs::exists(folder / hash));
  fs::remove_all(folder);
}
