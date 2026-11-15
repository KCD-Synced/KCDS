#include "kcd2s/integration/recipe_tables.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::integration;

KCD2S_TEST(recipe_tables, round_trips) {
  static_assert(kRecipeTableTag == 155);
  RecipeTable table;
  table.file = "AlchemyRecipe__kcds_910001.xml";
  table.patch = "<database name=\"barbora\"><AlchemyRecipes version=\"1\" /></database>";
  RecipeTable decoded;
  KCD2S_CHECK(decode_recipe_table(encode_recipe_table(table), decoded));
  KCD2S_CHECK(decoded == table);
  table.file = "BlacksmithRecipes__kcds_kit_shoe.xml";
  KCD2S_CHECK(decode_recipe_table(encode_recipe_table(table), decoded));
  KCD2S_CHECK(decoded == table);
  // A smithing workpiece of a resource's own.
  table.file = "BlacksmithWorkpieces__kcds_crp_plate.xml";
  KCD2S_CHECK(decode_recipe_table(encode_recipe_table(table), decoded));
  KCD2S_CHECK(decoded == table);
  table.file = "BlacksmithRecipes__kcds_kit_shoe.xml";
  // Its own icon: a name and a .dds, both or neither.
  table.icon_name = "kcds_recipe_smithing_kit_shoe_icon";
  table.icon.assign(kMaxRecipeIconBytes, 0x7f);
  KCD2S_CHECK(decode_recipe_table(encode_recipe_table(table), decoded));
  KCD2S_CHECK(decoded == table);
  table.icon.push_back(0);
  KCD2S_CHECK(encode_recipe_table(table).empty());
  table.icon.clear();
  KCD2S_CHECK(encode_recipe_table(table).empty());
  table.icon.assign(16, 1);
  table.icon_name = "Kcds_Upper";
  KCD2S_CHECK(encode_recipe_table(table).empty());
}

KCD2S_TEST(recipe_tables, refuses_other_files_and_sizes) {
  RecipeTable table;
  table.patch = "<database />";
  for (const char* file :
       {"AlchemyRecipe.xml", "AlchemyRecipe__.xml", "Items__x.xml", "BlacksmithWorkpieces__.xml",
        "AlchemyRecipe__../x.xml", "AlchemyRecipe__x.txt", ""}) {
    table.file = file;
    KCD2S_CHECK(encode_recipe_table(table).empty());
  }
  table.file = "AlchemyRecipe__kcds_1.xml";
  table.patch.clear();
  KCD2S_CHECK(encode_recipe_table(table).empty());
  table.patch.assign(kMaxRecipeTablePatchBytes + 1, 'x');
  KCD2S_CHECK(encode_recipe_table(table).empty());
  RecipeTable decoded;
  KCD2S_CHECK(!decode_recipe_table({kRecipeTableTag, 1}, decoded));
  KCD2S_CHECK(!decode_recipe_table({154, 1, 1}, decoded));
  table.patch = "<database />";
  auto cut = encode_recipe_table(table);
  cut.resize(cut.size() - 2);
  KCD2S_CHECK(!decode_recipe_table(cut, decoded));
}

// A picture through its parts: every message decoded and given to the assembler in order.
static bool assemble(kcd2s::testkit::Context& kcd2s_ctx, RecipePictureAssembler& assembler,
                     const RecipePicture& picture, RecipePicture& out, std::size_t& messages) {
  const auto parts = encode_recipe_picture(picture);
  messages = parts.size();
  bool done = false;
  for (const auto& message : parts) {
    KCD2S_CHECK(message.size() < 65536);
    RecipePicturePart part;
    KCD2S_CHECK(decode_recipe_picture_part(message, part));
    KCD2S_CHECK(!done);
    done = assembler.add(std::move(part), out);
  }
  return done;
}

KCD2S_TEST(recipe_tables, pictures_travel_in_parts) {
  static_assert(kRecipePictureTag == 156);
  RecipePicture picture;
  picture.which = RecipePictureKind::Recipe;
  picture.recipe_id = 910001;
  picture.dds.resize(kMaxRecipePictureBytes);
  for (std::size_t i = 0; i < picture.dds.size(); ++i) {
    picture.dds[i] = std::uint8_t(i * 7);
  }
  RecipePictureAssembler assembler;
  RecipePicture decoded;
  std::size_t messages = 0;
  KCD2S_CHECK(assemble(kcd2s_ctx, assembler, picture, decoded, messages));
  KCD2S_CHECK(messages == 65);
  KCD2S_CHECK(decoded == picture);
  // The game's own picture of another recipe (bookPicturesFrom): one part, no .dds.
  picture.which = RecipePictureKind::Product;
  picture.dds.clear();
  picture.from = 12;
  KCD2S_CHECK(assemble(kcd2s_ctx, assembler, picture, decoded, messages));
  KCD2S_CHECK(messages == 1);
  KCD2S_CHECK(decoded == picture);
}

KCD2S_TEST(recipe_tables, refuses_pictures_out_of_bounds) {
  RecipePicture picture;
  picture.recipe_id = 910001;
  // Neither its own .dds nor the game's, then both.
  KCD2S_CHECK(encode_recipe_picture(picture).empty());
  picture.from = 12;
  picture.dds.assign(16, 1);
  KCD2S_CHECK(encode_recipe_picture(picture).empty());
  // A product's picture is at most 256 x 256.
  picture.from = 0;
  picture.dds.assign(kMaxProductPictureBytes + 1, 1);
  KCD2S_CHECK(encode_recipe_picture(picture).empty());
  picture.which = RecipePictureKind::Recipe;
  KCD2S_CHECK(!encode_recipe_picture(picture).empty());
  picture.dds.assign(kMaxRecipePictureBytes + 1, 1);
  KCD2S_CHECK(encode_recipe_picture(picture).empty());
  picture.dds.assign(16, 1);
  picture.recipe_id = 0;
  KCD2S_CHECK(encode_recipe_picture(picture).empty());
  picture.recipe_id = 910001;
  picture.which = RecipePictureKind(3);
  KCD2S_CHECK(encode_recipe_picture(picture).empty());
  picture.which = RecipePictureKind::Recipe;
  auto cut = encode_recipe_picture(picture).front();
  cut.resize(cut.size() - 2);
  RecipePicturePart part;
  KCD2S_CHECK(!decode_recipe_picture_part(cut, part));
  KCD2S_CHECK(!decode_recipe_picture_part({kRecipeTableTag, 1}, part));
}

KCD2S_TEST(recipe_tables, drops_a_picture_whose_parts_skip) {
  RecipePicture picture;
  picture.which = RecipePictureKind::Recipe;
  picture.recipe_id = 910002;
  picture.dds.assign(3 * kRecipePicturePartBytes, 9);
  const auto parts = encode_recipe_picture(picture);
  KCD2S_CHECK(parts.size() == 3);
  RecipePictureAssembler assembler;
  RecipePicture out;
  RecipePicturePart part;
  // The first, then the last: the middle one is missing, so nothing comes of it.
  KCD2S_CHECK(decode_recipe_picture_part(parts[0], part));
  KCD2S_CHECK(!assembler.add(part, out));
  KCD2S_CHECK(decode_recipe_picture_part(parts[2], part));
  KCD2S_CHECK(!assembler.add(part, out));
  // A part beyond its total does not decode.
  part.offset = part.total;
  part.bytes.assign(1, 0);
  KCD2S_CHECK(!decode_recipe_picture_part(recipe_table_detail::encode_part(part), part));
  // Sent again whole, it comes together.
  bool done = false;
  for (const auto& message : parts) {
    KCD2S_CHECK(decode_recipe_picture_part(message, part));
    done = assembler.add(part, out);
  }
  KCD2S_CHECK(done);
  KCD2S_CHECK(out == picture);
}
