#include "owned_horse.hpp"
#include "kcd2s/testkit.hpp"

using kcds::runtime::riding::OwnedHorse;
using Step = OwnedHorse::Step;

KCD2S_TEST(owned_horse, given_horse_is_made_the_players_horse_once) {
  OwnedHorse owned;
  KCD2S_CHECK(owned.decide() == Step::Keep);
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
  KCD2S_CHECK(owned.wanted() == 500);
  owned.made(500, true);
  for (int frame = 0; frame < 3; ++frame) {
    owned.offer(500);
    KCD2S_CHECK(owned.decide() == Step::Keep);
  }
  KCD2S_CHECK(owned.held() == 500);
}

KCD2S_TEST(owned_horse, held_horse_stays_while_it_is_still_given) {
  OwnedHorse owned;
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
  owned.made(500, true);
  owned.offer(501);
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Keep);
  // Only the other one is given now: it is made the player's horse instead.
  owned.offer(501);
  KCD2S_CHECK(owned.decide() == Step::Make);
  KCD2S_CHECK(owned.wanted() == 501);
}

KCD2S_TEST(owned_horse, newest_of_several_given_horses_is_taken) {
  OwnedHorse owned;
  owned.offer(502);
  owned.offer(500);
  owned.offer(501);
  KCD2S_CHECK(owned.decide() == Step::Make);
  KCD2S_CHECK(owned.wanted() == 502);
}

KCD2S_TEST(owned_horse, horse_given_away_is_let_go) {
  OwnedHorse owned;
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
  owned.made(500, true);
  KCD2S_CHECK(owned.decide() == Step::Clear);
  owned.cleared();
  KCD2S_CHECK(owned.decide() == Step::Keep);
  KCD2S_CHECK(owned.held() == 0);
}

KCD2S_TEST(owned_horse, refused_horse_is_not_asked_again_until_given_anew) {
  OwnedHorse owned;
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
  owned.made(500, false);
  for (int frame = 0; frame < 3; ++frame) {
    owned.offer(500);
    KCD2S_CHECK(owned.decide() == Step::Keep);
  }
  // No longer given, then given again: a new attempt.
  KCD2S_CHECK(owned.decide() == Step::Keep);
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
}

KCD2S_TEST(owned_horse, horse_whose_body_leaves_is_forgotten) {
  OwnedHorse owned;
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
  owned.made(500, true);
  owned.forget(500);
  KCD2S_CHECK(owned.held() == 0);
  KCD2S_CHECK(owned.decide() == Step::Keep);
  owned.offer(500);
  KCD2S_CHECK(owned.decide() == Step::Make);
  owned.reset();
  KCD2S_CHECK(owned.held() == 0);
  KCD2S_CHECK(owned.decide() == Step::Keep);
}
