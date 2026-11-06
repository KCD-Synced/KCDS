#include "refused_pair.hpp"
#include "kcd2s/testkit.hpp"

using kcds::runtime::combat::RefusedPair;

KCD2S_TEST(refused_pair, only_the_attack_made_after_the_refused_choice_loses_its_mark) {
  RefusedPair refused;
  refused.factory_started(100);
  refused.pair_refused(100);
  refused.factory_made(100, 500);
  KCD2S_CHECK(!refused.take(501));
  // Any check drops the record: the attack is not taken later either.
  KCD2S_CHECK(!refused.take(500));
  refused.factory_started(100);
  refused.pair_refused(100);
  refused.factory_made(100, 500);
  KCD2S_CHECK(refused.take(500));
  KCD2S_CHECK(!refused.take(500));
}

KCD2S_TEST(refused_pair, a_factory_call_without_a_refusal_keeps_the_native_mark) {
  RefusedPair refused;
  refused.factory_started(100);
  refused.factory_made(100, 500);
  KCD2S_CHECK(!refused.take(500));
  // A refusal outside the factory call, or for another fighter, marks nothing.
  refused.pair_refused(100);
  refused.factory_started(100);
  refused.pair_refused(200);
  refused.factory_made(100, 500);
  KCD2S_CHECK(!refused.take(500));
}

KCD2S_TEST(refused_pair, another_factory_call_in_between_drops_the_refusal) {
  RefusedPair refused;
  refused.factory_started(100);
  refused.pair_refused(100);
  refused.factory_started(200);
  refused.factory_made(200, 600);
  refused.factory_made(100, 500);
  KCD2S_CHECK(!refused.take(500));
  KCD2S_CHECK(!refused.take(600));
  // A refused choice that made no ordinary attack marks nothing.
  refused.factory_started(100);
  refused.pair_refused(100);
  refused.factory_made(100, 0);
  KCD2S_CHECK(!refused.take(0));
  KCD2S_CHECK(!refused.take(500));
}
