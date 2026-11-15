#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The recipes resources register: Crafting.defineRecipe. The
// server writes each as a patch of the game's own recipe table; every ready client loads it into
// its game, a client whose world becomes ready later too.
//
//   RECIPE_TABLE : server -> a ready player's client, on the control channel (0):
//                  [tag][text file][text patch][text icon name][varuint icon bytes][icon]
//                  file: AlchemyRecipe__<suffix>.xml, BlacksmithRecipes__<suffix>.xml or
//                  BlacksmithWorkpieces__<suffix>.xml (a smithing workpiece,
//                  Crafting.defineWorkpiece; its game path is Libs/Tables/minigame/<file>);
//                  patch: the table's XML; the
//                  recipe's own icon (its iconImage) when it has one: the item icon's name (the
//                  row's UIIcon, kcds_...) and a 64 x 64 uncompressed .dds, else both empty.
//   RECIPE_PICTURE : server -> a ready player's client, on the control channel (0), after the
//                  recipe's table: a part of one of an alchemy recipe's two pictures in the
//                  recipe book,
//                  [tag][u2 which][varuint recipe id][varuint from][varuint total][varuint offset]
//                  [varuint part bytes][part]
//                  which: 1 the product's (256 x 256), 2 the recipe's (1024 x 1024); the book
//                  loads them as Libs/UI/Textures/Books/{Products,Recipes}/<recipe id>_ui.dds.
//                  Either `from`, the game's own alchemy RecipeId whose picture is shown
//                  (bookPicturesFrom; one part, total 0), or the .dds (a DXT5 one, its own
//                  picture, `total` bytes) in parts of at most 16 KiB, in order. A message's length
//                  is 16 bits on the wire and one flush is one datagram, so the server sends a few
//                  parts a tick (Server::send_recipe_picture).
//
// Client/runtime/crafting/recipe_tables.hpp loads them.
namespace kcd2s::integration {

inline constexpr std::uint8_t kRecipeTableTag = 155;
inline constexpr std::uint8_t kRecipePictureTag = 156;

inline constexpr std::size_t kMaxRecipeTableFileBytes = 128;
// A message's length is 16 bits on the wire, so the patch and the icon together stay below it
// (registered.cjs refuses a recipe whose patch is longer).
inline constexpr std::size_t kMaxRecipeTablePatchBytes = 40 * 1024;
// A recipe's own icon: 64 x 64 B8G8R8A8 and the .dds header.
inline constexpr std::size_t kMaxRecipeIconBytes = 128 + 64 * 64 * 4;
static_assert(kMaxRecipeTablePatchBytes + kMaxRecipeIconBytes + 3 * kMaxRecipeTableFileBytes + 32 <
                  65536,
              "a RECIPE_TABLE must fit one message");
// Patches a client holds before its game takes them (further ones are dropped).
inline constexpr std::size_t kMaxRecipeTables = 256;
// The recipe book's pictures: the .dds header and one byte a pixel (DXT5) of their square.
inline constexpr std::size_t kMaxProductPictureBytes = 128 + 256 * 256;
inline constexpr std::size_t kMaxRecipePictureBytes = 128 + 1024 * 1024;
inline constexpr std::uint32_t kMaxRecipePictureId = 2147483647;
// Pictures a client holds before its game takes them, or collects at once (further ones are
// dropped).
inline constexpr std::size_t kMaxRecipePictures = 64;
inline constexpr std::size_t kRecipePicturePartBytes = 16 * 1024;

struct RecipeTable {
  std::string file;
  std::string patch;
  std::string icon_name;
  std::vector<std::uint8_t> icon;

  friend bool operator==(const RecipeTable& a, const RecipeTable& b) {
    return a.file == b.file && a.patch == b.patch && a.icon_name == b.icon_name && a.icon == b.icon;
  }
};

enum class RecipePictureKind : std::uint8_t { Product = 1, Recipe = 2 };

struct RecipePicture {
  RecipePictureKind which = RecipePictureKind::Product;
  std::uint32_t recipe_id = 0;
  std::uint32_t from = 0;  // the game's own recipe whose picture is shown, else 0
  std::vector<std::uint8_t> dds;

  friend bool operator==(const RecipePicture& a, const RecipePicture& b) {
    return a.which == b.which && a.recipe_id == b.recipe_id && a.from == b.from && a.dds == b.dds;
  }
};

namespace recipe_table_detail {

inline bool starts_with(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

// AlchemyRecipe__, BlacksmithRecipes__ or BlacksmithWorkpieces__, then letters, digits, '_', '-'
// or '.', then .xml.
inline bool valid_file(const std::string& file) {
  if (file.empty() || file.size() > kMaxRecipeTableFileBytes || file.size() < 4 ||
      file.compare(file.size() - 4, 4, ".xml") != 0) {
    return false;
  }
  std::size_t suffix = 0;
  if (starts_with(file, "AlchemyRecipe__")) {
    suffix = 15;
  } else if (starts_with(file, "BlacksmithRecipes__")) {
    suffix = 19;
  } else if (starts_with(file, "BlacksmithWorkpieces__")) {
    suffix = 22;
  } else {
    return false;
  }
  if (file.size() - 4 <= suffix) {
    return false;
  }
  for (std::size_t i = suffix; i < file.size() - 4; ++i) {
    const char c = file[i];
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

// An icon's name: empty, or kcds_ then lower-case letters, digits and '_'.
inline bool valid_icon_name(const std::string& name) {
  if (name.empty()) {
    return true;
  }
  return name.size() <= kMaxRecipeTableFileBytes && name.rfind("kcds_", 0) == 0 &&
         name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}

inline bool valid_icon(const RecipeTable& table) {
  return valid_icon_name(table.icon_name) && table.icon.size() <= kMaxRecipeIconBytes &&
         table.icon_name.empty() == table.icon.empty();
}

inline bool valid_picture(const RecipePicture& picture) {
  const bool product = picture.which == RecipePictureKind::Product;
  if (!product && picture.which != RecipePictureKind::Recipe) {
    return false;
  }
  const std::size_t max_bytes = product ? kMaxProductPictureBytes : kMaxRecipePictureBytes;
  const bool own = !picture.dds.empty();
  return picture.recipe_id >= 1 && picture.recipe_id <= kMaxRecipePictureId &&
         picture.from <= kMaxRecipePictureId && own == (picture.from == 0) &&
         picture.dds.size() <= max_bytes;
}

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  for (const char c : text) {
    w.write_bits(std::uint8_t(c), 8);
  }
}

inline bool read_text(core::BitReader& r, std::size_t max_bytes, std::string& text) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size > max_bytes) {
    return false;
  }
  text.clear();
  for (std::uint64_t i = 0; i < size; ++i) {
    text.push_back(char(r.read_bits(8)));
  }
  return !r.overrun();
}

}  // namespace recipe_table_detail

// Empty when the patch cannot travel.
inline std::vector<std::uint8_t> encode_recipe_table(const RecipeTable& table) {
  using namespace recipe_table_detail;
  if (!valid_file(table.file) || table.patch.empty() ||
      table.patch.size() > kMaxRecipeTablePatchBytes || !valid_icon(table)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kRecipeTableTag, 8);
  write_text(w, table.file);
  write_text(w, table.patch);
  write_text(w, table.icon_name);
  w.write_varuint(table.icon.size());
  for (const std::uint8_t byte : table.icon) {
    w.write_bits(byte, 8);
  }
  return w.bytes();
}

inline bool decode_recipe_table(const std::vector<std::uint8_t>& p, RecipeTable& table) {
  using namespace recipe_table_detail;
  if (p.size() < 3 || p[0] != kRecipeTableTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  RecipeTable decoded;
  if (!read_text(r, kMaxRecipeTableFileBytes, decoded.file) ||
      !read_text(r, kMaxRecipeTablePatchBytes, decoded.patch) || !valid_file(decoded.file) ||
      decoded.patch.empty() || !read_text(r, kMaxRecipeTableFileBytes, decoded.icon_name)) {
    return false;
  }
  const std::uint64_t icon_bytes = r.read_varuint();
  if (r.overrun() || icon_bytes > kMaxRecipeIconBytes) {
    return false;
  }
  decoded.icon.resize(std::size_t(icon_bytes));
  for (std::uint8_t& byte : decoded.icon) {
    byte = std::uint8_t(r.read_bits(8));
  }
  if (r.overrun() || !valid_icon(decoded)) {
    return false;
  }
  table = std::move(decoded);
  return true;
}

// One message of a picture: its bytes from `offset`, at most kRecipePicturePartBytes of them.
struct RecipePicturePart {
  RecipePictureKind which = RecipePictureKind::Product;
  std::uint32_t recipe_id = 0;
  std::uint32_t from = 0;
  std::uint32_t total = 0;
  std::uint32_t offset = 0;
  std::vector<std::uint8_t> bytes;
};

namespace recipe_table_detail {

inline std::vector<std::uint8_t> encode_part(const RecipePicturePart& part) {
  core::BitWriter w;
  w.write_bits(kRecipePictureTag, 8);
  w.write_bits(std::uint8_t(part.which), 2);
  w.write_varuint(part.recipe_id);
  w.write_varuint(part.from);
  w.write_varuint(part.total);
  w.write_varuint(part.offset);
  w.write_varuint(part.bytes.size());
  for (const std::uint8_t byte : part.bytes) {
    w.write_bits(byte, 8);
  }
  return w.bytes();
}

}  // namespace recipe_table_detail

// The messages of a picture, in order; none when it cannot travel.
inline std::vector<std::vector<std::uint8_t>> encode_recipe_picture(const RecipePicture& picture) {
  if (!recipe_table_detail::valid_picture(picture)) {
    return {};
  }
  RecipePicturePart part;
  part.which = picture.which;
  part.recipe_id = picture.recipe_id;
  part.from = picture.from;
  part.total = std::uint32_t(picture.dds.size());
  std::vector<std::vector<std::uint8_t>> messages;
  if (picture.dds.empty()) {
    messages.push_back(recipe_table_detail::encode_part(part));
    return messages;
  }
  for (std::size_t offset = 0; offset < picture.dds.size(); offset += kRecipePicturePartBytes) {
    const std::size_t end = std::min(picture.dds.size(), offset + kRecipePicturePartBytes);
    part.offset = std::uint32_t(offset);
    part.bytes.assign(picture.dds.begin() + std::ptrdiff_t(offset),
                      picture.dds.begin() + std::ptrdiff_t(end));
    messages.push_back(recipe_table_detail::encode_part(part));
  }
  return messages;
}

inline bool decode_recipe_picture_part(const std::vector<std::uint8_t>& p,
                                       RecipePicturePart& part) {
  if (p.size() < 2 || p[0] != kRecipePictureTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  RecipePicturePart decoded;
  decoded.which = RecipePictureKind(r.read_bits(2));
  const std::uint64_t recipe_id = r.read_varuint();
  const std::uint64_t from = r.read_varuint();
  const std::uint64_t total = r.read_varuint();
  const std::uint64_t offset = r.read_varuint();
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || recipe_id > kMaxRecipePictureId || from > kMaxRecipePictureId ||
      total > kMaxRecipePictureBytes || size > kRecipePicturePartBytes || offset + size > total ||
      (total == 0) != (from != 0) || (total > 0 && size == 0)) {
    return false;
  }
  decoded.recipe_id = std::uint32_t(recipe_id);
  decoded.from = std::uint32_t(from);
  decoded.total = std::uint32_t(total);
  decoded.offset = std::uint32_t(offset);
  decoded.bytes.resize(std::size_t(size));
  for (std::uint8_t& byte : decoded.bytes) {
    byte = std::uint8_t(r.read_bits(8));
  }
  if (r.overrun()) {
    return false;
  }
  part = std::move(decoded);
  return true;
}

// Puts a client's pictures together from their parts.
class RecipePictureAssembler {
 public:
  // True with the whole picture in `out` once its last part came; a part out of order (the
  // channel keeps them in order, so only a forged one) drops what was collected of it.
  bool add(RecipePicturePart part, RecipePicture& out) {
    RecipePicture picture;
    picture.which = part.which;
    picture.recipe_id = part.recipe_id;
    picture.from = part.from;
    if (part.total == 0) {
      return finish(std::move(picture), out);
    }
    const auto key = std::make_pair(std::uint8_t(part.which), part.recipe_id);
    auto found = collecting_.find(key);
    if (part.offset == 0) {
      if (found == collecting_.end() && collecting_.size() >= kMaxRecipePictures) {
        return false;
      }
      collecting_[key] = Collecting{part.total, {}};
      found = collecting_.find(key);
    }
    if (found == collecting_.end() || found->second.total != part.total ||
        found->second.bytes.size() != part.offset) {
      if (found != collecting_.end()) {
        collecting_.erase(found);
      }
      return false;
    }
    std::vector<std::uint8_t>& bytes = found->second.bytes;
    bytes.insert(bytes.end(), part.bytes.begin(), part.bytes.end());
    if (bytes.size() < part.total) {
      return false;
    }
    picture.dds = std::move(bytes);
    collecting_.erase(found);
    return finish(std::move(picture), out);
  }

 private:
  struct Collecting {
    std::uint32_t total = 0;
    std::vector<std::uint8_t> bytes;
  };

  static bool finish(RecipePicture picture, RecipePicture& out) {
    if (!recipe_table_detail::valid_picture(picture)) {
      return false;
    }
    out = std::move(picture);
    return true;
  }

  std::map<std::pair<std::uint8_t, std::uint32_t>, Collecting> collecting_;
};

}  // namespace kcd2s::integration
