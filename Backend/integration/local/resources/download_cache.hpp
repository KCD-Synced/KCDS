#pragma once
// The game client's resource download cache. A server's client resources travel as
// content-addressed files: each file is named by its SHA-256 in the server's catalog, and the
// resource runtime asks for it in chunks ({op: "fileRequest", revision, hash, offset, length}).
// The cache keeps every complete file it saw verified on disk, named by its hash, so joining a
// server again (or another server with the same files) reads them from the disk instead of the
// network:
//
//   - A fileRequest for a cached file is answered here, as the server would ({op: "fileData",
//     revision, hash, offset, size, data (base64), requestId}); it never reaches the server.
//   - The server's fileData chunks are collected; once a file is complete and its SHA-256 is the
//     name it was asked by, it is written to the folder (a temporary file renamed into place).
//   - A cached file is hashed again the first time a session reads it; a damaged one is deleted
//     and asked from the server like any other.
//   - The folder is kept under its byte limit: the least recently used files go first.
//
// The runtime still checks every assembled file's hash itself, so the cache can never give a
// resource bytes other than the ones the server's catalog names. Portable (std::filesystem); the
// hash implementation is the caller's. Not thread-safe: the network thread uses it.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcd2s::local::resources {

class DownloadCache {
 public:
  // SHA-256 of the bytes, and of a file read in pieces, in lowercase hex.
  using Hasher = std::function<std::string(const std::string& bytes)>;
  using FileHasher = std::function<std::string(const std::filesystem::path& file)>;

  // Room for a few servers' files (a catalog holds up to 4 GiB, bundles.cjs).
  static constexpr std::uint64_t kDefaultLimitBytes = 16ull * 1024 * 1024 * 1024;
  // The server's limits (bundles.cjs): a file of at most 512 MiB, chunks of at most 16 KiB.
  static constexpr std::int64_t kMaxFileBytes = 512ll * 1024 * 1024;
  static constexpr std::int64_t kMaxChunkBytes = 16 * 1024;
  // A file up to this size is held in memory once read; a bigger one is read from the disk for
  // each chunk asked of it.
  static constexpr std::int64_t kInMemoryBytes = 4 * 1024 * 1024;
  // Files being collected from chunks or held at once, in bytes.
  static constexpr std::uint64_t kMaxMemoryBytes = 640ull * 1024 * 1024;

  DownloadCache(std::filesystem::path folder, Hasher hasher, FileHasher file_hasher = {},
                std::uint64_t limit_bytes = kDefaultLimitBytes)
      : folder_(std::move(folder)),
        hasher_(std::move(hasher)),
        file_hasher_(std::move(file_hasher)),
        limit_bytes_(limit_bytes) {
  }

  // The SHA-256 of these bytes, by the cache's own hasher.
  std::string digest(const std::string& bytes) const {
    return hasher_(bytes);
  }

  // Whether the folder holds this file, verified this session: hashed the first time (in pieces,
  // so a big file is never held in memory). A damaged one is deleted.
  bool ensure(const std::string& hash) {
    if (!valid_hash(hash)) {
      return false;
    }
    if (verified_.count(hash) || loaded_.count(hash)) {
      return true;
    }
    const std::filesystem::path file = folder_ / hash;
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > std::uint64_t(kMaxFileBytes)) {
      return false;
    }
    const std::string actual = file_hasher_ ? file_hasher_(file) : whole_file_hash(file);
    if (actual != hash) {
      std::filesystem::remove(file, error);
      return false;
    }
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), error);
    verified_.insert(hash);
    return true;
  }

  // A whole file that came over HTTP into `downloaded` (a file of the folder's volume): moved
  // into place when its SHA-256 is the name it was asked by, else deleted. False when it is not.
  bool put_file(const std::string& hash, const std::filesystem::path& downloaded) {
    std::error_code error;
    const auto size = std::filesystem::file_size(downloaded, error);
    const bool right =
        !error && valid_hash(hash) && size <= std::uint64_t(kMaxFileBytes) &&
        (file_hasher_ ? file_hasher_(downloaded) : whole_file_hash(downloaded)) == hash;
    if (!right) {
      std::filesystem::remove(downloaded, error);
      return false;
    }
    std::filesystem::rename(downloaded, folder_ / hash, error);
    if (error) {
      std::filesystem::remove(downloaded, error);
      return false;
    }
    verified_.insert(hash);
    trim();
    return true;
  }

  // The fileData reply to a fileRequest the cache can answer, or nullopt.
  std::optional<sdk::Value> answer(const sdk::Value& request) {
    const std::string hash = text(request, "hash");
    const auto revision = number(request, "revision");
    const auto offset = number(request, "offset");
    const auto length = number(request, "length");
    const auto* request_id = integration::script_field(request, "requestId");
    if (!valid_hash(hash) || !revision || !offset || !length || !request_id) {
      return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::path file = folder_ / hash;
    const auto on_disk = std::filesystem::file_size(file, error);
    if (error) {
      return std::nullopt;
    }
    std::string piece;
    std::int64_t size = 0;
    if (std::int64_t(on_disk) > kInMemoryBytes) {
      // A big file: verified once, then each chunk is read from the disk.
      if (!ensure(hash)) {
        return std::nullopt;
      }
      size = std::int64_t(on_disk);
      if (*offset < 0 || *offset > size || *length < 1 || *length > kMaxChunkBytes) {
        return std::nullopt;
      }
      piece.assign(std::size_t(std::min(*length, size - *offset)), '\0');
      std::ifstream in(file, std::ios::binary);
      in.seekg(std::streamoff(*offset));
      if (!piece.empty() && !in.read(piece.data(), std::streamsize(piece.size()))) {
        return std::nullopt;
      }
    } else {
      const std::shared_ptr<const std::string> bytes = cached(hash);
      if (!bytes) {
        return std::nullopt;
      }
      size = std::int64_t(bytes->size());
      if (*offset < 0 || *offset > size || *length < 1 || *length > kMaxChunkBytes) {
        return std::nullopt;
      }
      piece = bytes->substr(std::size_t(*offset), std::size_t(std::min(*length, size - *offset)));
    }
    ++hits_;
    return sdk::Value(sdk::Value::Map{
        {"op", sdk::Value("fileData")},
        {"revision", sdk::Value(*revision)},
        {"hash", sdk::Value(hash)},
        {"offset", sdk::Value(*offset)},
        {"size", sdk::Value(size)},
        {"data", sdk::Value(base64(piece))},
        {"requestId", *request_id},
    });
  }

  // A fileData chunk from the server: kept until its file is complete, then the verified file is
  // written to the folder.
  void keep(const sdk::Value& reply) {
    const std::string hash = text(reply, "hash");
    const auto offset = number(reply, "offset");
    const auto size = number(reply, "size");
    const std::optional<std::string> data = unbase64(text(reply, "data"));
    if (!valid_hash(hash) || !offset || !size || !data || *size < 0 || *size > kMaxFileBytes ||
        *offset < 0 || *offset + std::int64_t(data->size()) > *size || loaded_.count(hash)) {
      return;
    }
    auto found = staging_.find(hash);
    if (found == staging_.end()) {
      if (memory_ + std::uint64_t(*size) > kMaxMemoryBytes) {
        return;
      }
      Staged staged;
      staged.bytes.assign(std::size_t(*size), '\0');
      found = staging_.emplace(hash, std::move(staged)).first;
      memory_ += std::uint64_t(*size);
    }
    Staged& staged = found->second;
    if (std::int64_t(staged.bytes.size()) != *size || !staged.offsets.insert(*offset).second) {
      return;
    }
    std::copy(data->begin(), data->end(), staged.bytes.begin() + std::ptrdiff_t(*offset));
    staged.received += std::int64_t(data->size());
    if (staged.received < *size) {
      return;
    }
    std::string bytes = std::move(staged.bytes);
    memory_ -= std::uint64_t(*size);
    staging_.erase(found);
    if (hasher_(bytes) == hash) {
      store(hash, bytes);
    }
  }

  // A whole file that came another way (the server's HTTP files, http_files.hpp): kept when its
  // SHA-256 is the name it was asked by. False when it is not.
  bool put(const std::string& hash, const std::string& bytes) {
    if (!valid_hash(hash) || std::int64_t(bytes.size()) > kMaxFileBytes || hasher_(bytes) != hash) {
      return false;
    }
    store(hash, bytes);
    return true;
  }

  // Requests answered from the disk since the cache was made (for the log and the tests).
  std::uint64_t hits() const {
    return hits_;
  }
  const std::filesystem::path& folder() const {
    return folder_;
  }

 private:
  struct Staged {
    std::string bytes;
    std::set<std::int64_t> offsets;
    std::int64_t received = 0;
  };

  static bool valid_hash(const std::string& hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
  }

  // The hash of a whole file read at once (without a file hasher: the tests).
  std::string whole_file_hash(const std::filesystem::path& file) const {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return hasher_(bytes);
  }

  static std::string text(const sdk::Value& value, const char* name) {
    const auto* field = integration::script_field(value, name);
    return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
  }

  // A whole number field, or nullopt.
  static std::optional<std::int64_t> number(const sdk::Value& value, const char* name) {
    const auto* field = integration::script_field(value, name);
    if (!field) {
      return std::nullopt;
    }
    if (field->tag() == sdk::Tag::Int) {
      return field->as_int();
    }
    if (field->tag() == sdk::Tag::Double) {
      const double number = field->as_double();
      if (number >= -9007199254740991.0 && number <= 9007199254740991.0 &&
          double(std::int64_t(number)) == number) {
        return std::int64_t(number);
      }
    }
    return std::nullopt;
  }

  // The file's verified bytes: read from the folder and hashed the first time, or null.
  std::shared_ptr<const std::string> cached(const std::string& hash) {
    const auto loaded = loaded_.find(hash);
    if (loaded != loaded_.end()) {
      return loaded->second;
    }
    const std::filesystem::path file = folder_ / hash;
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > std::uint64_t(kMaxFileBytes) || memory_ + size > kMaxMemoryBytes) {
      return nullptr;
    }
    std::ifstream in(file, std::ios::binary);
    std::string bytes(std::size_t(size), '\0');
    if (!in.read(bytes.data(), std::streamsize(size)) || hasher_(bytes) != hash) {
      in.close();
      std::filesystem::remove(file, error);
      return nullptr;
    }
    in.close();
    // Recently used: kept longest.
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), error);
    memory_ += size;
    auto shared = std::make_shared<const std::string>(std::move(bytes));
    loaded_.emplace(hash, shared);
    return shared;
  }

  void store(const std::string& hash, const std::string& bytes) {
    std::error_code error;
    std::filesystem::create_directories(folder_, error);
    const std::filesystem::path file = folder_ / hash;
    // Its own temporary name: two games of this machine may write the same file at once.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary =
        folder_ /
        (hash + "." + std::to_string(std::uintptr_t(this)) + "." + std::to_string(stamp) + ".part");
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      if (!out.write(bytes.data(), std::streamsize(bytes.size()))) {
        out.close();
        std::filesystem::remove(temporary, error);
        return;
      }
    }
    std::filesystem::rename(temporary, file, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return;
    }
    trim();
  }

  // Deletes the least recently used files until the folder is under its limit.
  void trim() {
    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder_, error)) {
      std::error_code item;
      if (!entry.is_regular_file(item)) {
        continue;
      }
      total += entry.file_size(item);
      files.emplace_back(entry.last_write_time(item), entry.path());
    }
    if (total <= limit_bytes_) {
      return;
    }
    std::sort(files.begin(), files.end());
    for (const auto& [when, path] : files) {
      (void)when;
      if (total <= limit_bytes_) {
        break;
      }
      std::error_code item;
      const auto size = std::filesystem::file_size(path, item);
      if (std::filesystem::remove(path, item)) {
        total -= item ? 0 : size;
      }
    }
  }

  static std::string base64(const std::string& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
      const std::uint32_t triple = (std::uint32_t(std::uint8_t(bytes[i])) << 16) |
                                   (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8) |
                                   std::uint32_t(std::uint8_t(bytes[i + 2]));
      text.push_back(kAlphabet[(triple >> 18) & 63]);
      text.push_back(kAlphabet[(triple >> 12) & 63]);
      text.push_back(kAlphabet[(triple >> 6) & 63]);
      text.push_back(kAlphabet[triple & 63]);
      i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
      std::uint32_t triple = std::uint32_t(std::uint8_t(bytes[i])) << 16;
      if (rest == 2) {
        triple |= std::uint32_t(std::uint8_t(bytes[i + 1])) << 8;
      }
      text.push_back(kAlphabet[(triple >> 18) & 63]);
      text.push_back(kAlphabet[(triple >> 12) & 63]);
      text.push_back(rest == 2 ? kAlphabet[(triple >> 6) & 63] : '=');
      text.push_back('=');
    }
    return text;
  }

  // Standard base64 with padding, or nullopt.
  static std::optional<std::string> unbase64(const std::string& text) {
    if (text.size() % 4 != 0) {
      return std::nullopt;
    }
    auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') {
        return c - 'A';
      }
      if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
      }
      if (c >= '0' && c <= '9') {
        return c - '0' + 52;
      }
      if (c == '+') {
        return 62;
      }
      if (c == '/') {
        return 63;
      }
      return -1;
    };
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
      const bool last = i + 4 == text.size();
      const int padding = last ? int(text[i + 3] == '=') + int(text[i + 2] == '=') : 0;
      std::uint32_t quad = 0;
      for (int k = 0; k < 4; ++k) {
        const char c = text[i + std::size_t(k)];
        int digit = 0;
        if (k >= 4 - padding) {
          if (c != '=') {
            return std::nullopt;
          }
        } else {
          digit = value(c);
          if (digit < 0) {
            return std::nullopt;
          }
        }
        quad = (quad << 6) | std::uint32_t(digit);
      }
      bytes.push_back(char((quad >> 16) & 255));
      if (padding < 2) {
        bytes.push_back(char((quad >> 8) & 255));
      }
      if (padding < 1) {
        bytes.push_back(char(quad & 255));
      }
    }
    return bytes;
  }

  std::filesystem::path folder_;
  Hasher hasher_;
  FileHasher file_hasher_;
  // Big files verified on the disk this session.
  std::set<std::string> verified_;
  std::uint64_t limit_bytes_;
  std::map<std::string, std::shared_ptr<const std::string>> loaded_;
  std::map<std::string, Staged> staging_;
  std::uint64_t memory_ = 0;
  std::uint64_t hits_ = 0;
};

}  // namespace kcd2s::local::resources
