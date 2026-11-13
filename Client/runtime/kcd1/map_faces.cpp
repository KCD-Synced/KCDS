#include "map_faces.hpp"

#include <cstring>
#include <string_view>

#include "../files/zip_read.hpp"

namespace kcds::runtime::kcd1 {
namespace {

constexpr std::size_t kHeader = 8;  // signature, version, the uncompressed file's length
constexpr std::size_t kZlibHeader = 2;
constexpr std::size_t kZlibTrailer = 4;
constexpr int kPlaceObject2 = 26;
constexpr int kDefineSprite = 39;
constexpr int kExportAssets = 56;
constexpr std::uint8_t kPlaceHasCharacter = 0x02;
constexpr std::string_view kEmptyClip = "mc_empty";
constexpr std::string_view kFaces = "reputationIcon";

std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return std::uint16_t(bytes[at] | (bytes[at + 1] << 8));
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8) |
         (std::uint32_t(bytes[at + 2]) << 16) | (std::uint32_t(bytes[at + 3]) << 24);
}

struct Tag {
  int code = 0;
  std::size_t body = 0;  // where its body starts
  std::size_t length = 0;
};

// The tags from `at` to `end` (a movie's, or a sprite's after its id and frame count).
std::vector<Tag> tags_of(const std::vector<std::uint8_t>& movie, std::size_t at, std::size_t end) {
  std::vector<Tag> tags;
  while (at + 2 <= end) {
    const std::uint16_t head = u16(movie, at);
    Tag tag;
    tag.code = head >> 6;
    tag.length = head & 0x3F;
    at += 2;
    if (tag.length == 0x3F) {
      if (at + 4 > end) {
        break;
      }
      tag.length = u32(movie, at);
      at += 4;
    }
    tag.body = at;
    if (tag.length > end - at) {
      break;
    }
    tags.push_back(tag);
    at += tag.length;
    if (tag.code == 0) {
      break;
    }
  }
  return tags;
}

bool holds(const std::vector<std::uint8_t>& movie, const Tag& tag, std::string_view text) {
  const auto* begin = reinterpret_cast<const char*>(movie.data() + tag.body);
  return std::string_view(begin, tag.length).find(text) != std::string_view::npos;
}

// The character id ExportAssets gives `name`, 0 for none.
std::uint16_t exported(const std::vector<std::uint8_t>& movie, const Tag& tag,
                       std::string_view name) {
  if (tag.length < 2) {
    return 0;
  }
  const std::size_t end = tag.body + tag.length;
  std::size_t at = tag.body + 2;
  for (std::uint16_t index = 0; index < u16(movie, tag.body) && at + 2 < end; ++index) {
    const std::uint16_t id = u16(movie, at);
    at += 2;
    const auto* text = reinterpret_cast<const char*>(movie.data() + at);
    const std::size_t length = strnlen(text, end - at);
    if (std::string_view(text, length) == name) {
      return id;
    }
    at += length + 1;
  }
  return 0;
}

}  // namespace

std::vector<std::uint8_t> map_movie_without_faces(const std::vector<std::uint8_t>& source) {
  if (source.size() < kHeader + kZlibHeader + kZlibTrailer) {
    return {};
  }
  const std::string_view signature(reinterpret_cast<const char*>(source.data()), 3);
  const bool compressed = signature == "CFX" || signature == "CWS";
  if (!compressed && signature != "GFX" && signature != "FWS") {
    return {};
  }
  std::vector<std::uint8_t> movie(source.begin(), source.begin() + kHeader);
  if (compressed) {
    const std::size_t size = u32(source, 4);
    if (size <= kHeader) {
      return {};
    }
    const auto body =
        files::inflate(source.data() + kHeader + kZlibHeader,
                       source.size() - kHeader - kZlibHeader - kZlibTrailer, size - kHeader);
    if (!body) {
      return {};
    }
    movie.insert(movie.end(), body->begin(), body->end());
    movie[0] = signature == "CFX" ? 'G' : 'F';
  } else {
    movie.insert(movie.end(), source.begin() + kHeader, source.end());
  }
  // After the header: the frame's RECT (its bit count in its first five bits), rate and count.
  const std::size_t rect_bits = 5 + 4 * std::size_t(movie[kHeader] >> 3);
  const std::size_t first_tag = kHeader + (rect_bits + 7) / 8 + 4;
  if (first_tag >= movie.size()) {
    return {};
  }
  const std::vector<Tag> top = tags_of(movie, first_tag, movie.size());
  std::uint16_t empty = 0;
  for (const Tag& tag : top) {
    if (tag.code == kExportAssets && empty == 0) {
      empty = exported(movie, tag, kEmptyClip);
    }
  }
  if (empty == 0) {
    return {};
  }
  std::size_t changed = 0;
  for (const Tag& sprite : top) {
    if (sprite.code != kDefineSprite || sprite.length < 4 || !holds(movie, sprite, kFaces)) {
      continue;
    }
    for (const Tag& place : tags_of(movie, sprite.body + 4, sprite.body + sprite.length)) {
      // PlaceObject2: its flags, its depth, then its character when it has one.
      if (place.code != kPlaceObject2 || place.length < 5 ||
          !(movie[place.body] & kPlaceHasCharacter) || !holds(movie, place, kFaces)) {
        continue;
      }
      movie[place.body + 3] = std::uint8_t(empty);
      movie[place.body + 4] = std::uint8_t(empty >> 8);
      changed += 1;
    }
  }
  return changed > 0 ? movie : std::vector<std::uint8_t>{};
}

}  // namespace kcds::runtime::kcd1
