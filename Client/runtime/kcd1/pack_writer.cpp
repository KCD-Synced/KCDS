#include "pack_writer.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

namespace kcds::runtime::kcd1 {
namespace {

constexpr std::uint32_t kLocalRecord = 0x04034B50u;
constexpr std::uint32_t kDirectoryRecord = 0x02014B50u;
constexpr std::uint32_t kEndRecord = 0x06054B50u;
constexpr std::size_t kLocalRecordSize = 30;
constexpr std::uint16_t kZipVersion = 20;
constexpr std::uint16_t kDosDate = 0x21;  // 1980-01-01
constexpr std::uint64_t kMostPack = 0xFFFFFFFFull;
// Copied in pieces of this size, so a big entry (a level's terrain) never sits in memory whole.
constexpr std::size_t kCopyPiece = 4u * 1024u * 1024u;

void put16(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(std::uint8_t(value));
  out.push_back(std::uint8_t(value >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  put16(out, value & 0xFFFFu);
  put16(out, value >> 16);
}

std::uint16_t read16(const std::uint8_t* at) {
  return std::uint16_t(at[0] | (at[1] << 8));
}

std::uint32_t read32(const std::uint8_t* at) {
  return std::uint32_t(at[0]) | (std::uint32_t(at[1]) << 8) | (std::uint32_t(at[2]) << 16) |
         (std::uint32_t(at[3]) << 24);
}

}  // namespace

files::ZipReadAt pack_on_disk(const std::filesystem::path& file, std::uint64_t& size) {
  size = 0;
  auto stream = std::make_shared<std::ifstream>(file, std::ios::binary);
  if (!*stream) {
    return {};
  }
  stream->seekg(0, std::ios::end);
  size = std::uint64_t(stream->tellg());
  auto lock = std::make_shared<std::mutex>();
  return [stream, lock](std::uint64_t offset, std::size_t length, std::vector<std::uint8_t>& out) {
    std::lock_guard<std::mutex> guard(*lock);
    out.resize(length);
    stream->clear();
    stream->seekg(std::streamoff(offset));
    stream->read(reinterpret_cast<char*>(out.data()), std::streamsize(length));
    return stream->gcount() == std::streamsize(length);
  };
}

PackWriter::PackWriter(const std::filesystem::path& file)
    : out_(file, std::ios::binary | std::ios::trunc) {
  failed_ = !out_;
}

bool PackWriter::local_header(const Record& record) {
  std::vector<std::uint8_t> header;
  put32(header, kLocalRecord);
  put16(header, kZipVersion);
  put16(header, 0);
  put16(header, record.method);
  put16(header, 0);
  put16(header, kDosDate);
  put32(header, record.crc);
  put32(header, record.compressed_size);
  put32(header, record.size);
  put16(header, std::uint32_t(record.name.size()));
  put16(header, 0);
  header.insert(header.end(), record.name.begin(), record.name.end());
  out_.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
  written_ += header.size();
  return bool(out_);
}

bool PackWriter::copy(const files::ZipReadAt& source, const files::ZipEntry& entry,
                      const std::string& name) {
  if (failed_) {
    return false;
  }
  // Where the entry's data starts: after its local header and that header's own name and extra.
  std::vector<std::uint8_t> header;
  if (!source(entry.local_offset, kLocalRecordSize, header) ||
      read32(header.data()) != kLocalRecord) {
    failed_ = true;
    return false;
  }
  const std::uint64_t start = std::uint64_t(entry.local_offset) + kLocalRecordSize +
                              read16(header.data() + 26) + read16(header.data() + 28);
  Record record;
  record.name = name.empty() ? entry.name : name;
  record.method = entry.method;
  record.crc = entry.crc;
  record.compressed_size = entry.compressed_size;
  record.size = entry.size;
  record.offset = std::uint32_t(written_);
  if (written_ + kLocalRecordSize + record.name.size() + entry.compressed_size > kMostPack ||
      !local_header(record)) {
    failed_ = true;
    return false;
  }
  std::vector<std::uint8_t> piece;
  std::uint64_t done = 0;
  while (done < entry.compressed_size) {
    const std::size_t length =
        std::size_t(std::min<std::uint64_t>(kCopyPiece, entry.compressed_size - done));
    if (!source(start + done, length, piece)) {
      failed_ = true;
      return false;
    }
    out_.write(reinterpret_cast<const char*>(piece.data()), std::streamsize(length));
    done += length;
  }
  written_ += entry.compressed_size;
  directory_.push_back(std::move(record));
  failed_ = !out_;
  return !failed_;
}

bool PackWriter::add(const std::string& name, const std::vector<std::uint8_t>& data) {
  if (failed_) {
    return false;
  }
  Record record;
  record.name = name;
  record.crc = files::zip_crc32(data.data(), data.size());
  record.compressed_size = std::uint32_t(data.size());
  record.size = std::uint32_t(data.size());
  record.offset = std::uint32_t(written_);
  if (written_ + kLocalRecordSize + name.size() + data.size() > kMostPack ||
      !local_header(record)) {
    failed_ = true;
    return false;
  }
  out_.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  written_ += data.size();
  directory_.push_back(std::move(record));
  failed_ = !out_;
  return !failed_;
}

bool PackWriter::add(const std::string& name, const std::string& text) {
  return add(name, std::vector<std::uint8_t>(text.begin(), text.end()));
}

bool PackWriter::finish() {
  if (failed_) {
    return false;
  }
  const std::uint64_t directory_start = written_;
  std::vector<std::uint8_t> directory;
  for (const Record& record : directory_) {
    put32(directory, kDirectoryRecord);
    put16(directory, kZipVersion);
    put16(directory, kZipVersion);
    put16(directory, 0);
    put16(directory, record.method);
    put16(directory, 0);
    put16(directory, kDosDate);
    put32(directory, record.crc);
    put32(directory, record.compressed_size);
    put32(directory, record.size);
    put16(directory, std::uint32_t(record.name.size()));
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put32(directory, 0);
    put32(directory, record.offset);
    directory.insert(directory.end(), record.name.begin(), record.name.end());
  }
  if (directory_start + directory.size() + 22 > kMostPack) {
    failed_ = true;
    return false;
  }
  // The count's low 16 bits, as the game's own big packs keep it.
  const std::uint32_t count = std::uint32_t(directory_.size() & 0xFFFFu);
  const std::uint32_t directory_size = std::uint32_t(directory.size());
  put32(directory, kEndRecord);
  put16(directory, 0);
  put16(directory, 0);
  put16(directory, count);
  put16(directory, count);
  put32(directory, directory_size);
  put32(directory, std::uint32_t(directory_start));
  put16(directory, 0);
  out_.write(reinterpret_cast<const char*>(directory.data()), std::streamsize(directory.size()));
  out_.close();
  failed_ = !out_;
  return !failed_;
}

}  // namespace kcds::runtime::kcd1
