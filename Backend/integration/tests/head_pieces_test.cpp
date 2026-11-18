#include "kcd2s/integration/head_pieces.hpp"

#include "kcd2s/testkit.hpp"

namespace integration = kcd2s::integration;

KCD2S_TEST(head_pieces, field_round_trip) {
  integration::HeadPiece piece;
  KCD2S_CHECK(integration::head_piece_field(piece).empty());
  piece.crown = "sigismund";
  KCD2S_CHECK(integration::head_piece_field(piece) == "sigismund|1");
  piece.hair_crown = false;
  KCD2S_CHECK(integration::head_piece_field(piece) == "sigismund|0");
  piece.crown.clear();
  KCD2S_CHECK(integration::head_piece_field(piece) == "|0");

  for (const char* field : {"", "sigismund|1", "wreath|0", "twitchWreath|1", "|0"}) {
    integration::HeadPiece back;
    KCD2S_CHECK(integration::parse_head_piece_field(field, back));
    KCD2S_CHECK(integration::head_piece_field(back) == field);
  }
}

KCD2S_TEST(head_pieces, bad_fields) {
  integration::HeadPiece piece;
  piece.crown = "wreath";
  for (const char* field : {"sigismund", "sigismund|", "sigismund|2", "sigismund|10", "crown|1",
                            "Sigismund|1", "|", "sigismund|1|1"}) {
    KCD2S_CHECK(!integration::parse_head_piece_field(field, piece));
  }
  // A refused field leaves what was there.
  KCD2S_CHECK(piece.crown == "wreath");
}
