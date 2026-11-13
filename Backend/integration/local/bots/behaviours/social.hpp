#pragma once
// The bots of a crowd: idle (stands, turns now and then, an emote), wander (walks and runs about
// its area, a chat line and an emote now and then), voice (talks in voice chat part of the time)
// and join_leave (stays a while, leaves, comes back).
#include <array>

#include "walking.hpp"

namespace kcd2s::local::bots {

// Chat lines a bot says: plain, short, nothing that looks like a command.
inline const std::array<const char*, 8>& bot_chat_lines() {
  static const std::array<const char*, 8> lines{"hello there",        "anyone going to the market?",
                                                "nice weather today", "where is the smith",
                                                "see you later",      "good day",
                                                "on my way",          "wait for me"};
  return lines;
}

// A chat line and an emote now and then; shared by the behaviours that wander.
class Chatter {
 public:
  Chatter(Range chat_every, Range emote_every)
      : chat_every_(chat_every), emote_every_(emote_every) {
  }
  void step(const Situation& situation, BotRandom& random, Intent& intent) {
    if (next_chat_ < 0.0) {
      next_chat_ = situation.now + random.between(chat_every_);
      next_emote_ = situation.now + random.between(emote_every_);
    }
    if (chat_every_.max > 0.0 && situation.now >= next_chat_) {
      const auto& lines = bot_chat_lines();
      intent.chat = lines[random.next() % lines.size()];
      next_chat_ = situation.now + random.between(chat_every_);
    }
    if (emote_every_.max > 0.0 && situation.now >= next_emote_) {
      intent.emote = true;
      next_emote_ = situation.now + random.between(emote_every_);
    }
  }

 private:
  Range chat_every_;
  Range emote_every_;
  double next_chat_ = -1.0;
  double next_emote_ = -1.0;
};

class IdleBehaviour : public Behaviour {
 public:
  explicit IdleBehaviour(Params& params)
      : turn_every_(params.interval("turn_every", {4.0, 15.0})),
        walk_({{1.0, 1.0}, 0.0, false}),
        chatter_({0.0, 0.0}, params.interval("emote_every", {60.0, 180.0})) {
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    walk_.step(situation, random, intent);
    if (next_turn_ < 0.0 || situation.now >= next_turn_) {
      next_turn_ = situation.now + random.between(turn_every_);
      face_ = random.point_near(situation.position, 5.0);
    }
    if (intent.speed == 0.0) {
      intent.face_point = true;
      intent.face = face_;
    }
    chatter_.step(situation, random, intent);
    return intent;
  }

 private:
  Range turn_every_;
  AreaWalk walk_;
  Chatter chatter_;
  double next_turn_ = -1.0;
  core::Vec3 face_;
};

class WanderBehaviour : public Behaviour {
 public:
  explicit WanderBehaviour(Params& params)
      : walk_({params.interval("pause", {2.0, 8.0}), params.chance("run_chance", 0.2), true}),
        chatter_(params.interval("chat_every", {30.0, 120.0}),
                 params.interval("emote_every", {45.0, 150.0})) {
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    walk_.step(situation, random, intent);
    chatter_.step(situation, random, intent);
    return intent;
  }

 private:
  AreaWalk walk_;
  Chatter chatter_;
};

// Talks for `talk_fraction` of the time, in bursts, and wanders (or stands: moves: false).
class VoiceBehaviour : public Behaviour {
 public:
  explicit VoiceBehaviour(Params& params)
      : fraction_(params.chance("talk_fraction", 0.3)),
        burst_(params.interval("burst", {1.5, 6.0})),
        walk_({params.interval("pause", {2.0, 8.0}), 0.1,
               params.word("moves", "true", {"true", "false"}) == "true"}) {
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    walk_.step(situation, random, intent);
    if (fraction_ <= 0.0) {
      return intent;  // never talks
    }
    if (situation.now >= switch_at_) {
      talking_ = fraction_ >= 1.0 || !talking_;
      const double burst = random.between(burst_);
      if (talking_) {
        switch_at_ = situation.now + burst;
      } else {
        // Silences long enough that the talking adds up to the fraction on average.
        const double silence = burst * (1.0 - fraction_) / fraction_;
        switch_at_ = situation.now + random.uniform(0.5, 1.5) * silence;
      }
    }
    intent.talking = talking_;
    return intent;
  }

 private:
  double fraction_;
  Range burst_;
  AreaWalk walk_;
  bool talking_ = false;
  double switch_at_ = 0.0;
};

// Stays `stay`, leaves, comes back after `away`; wanders while there.
class JoinLeaveBehaviour : public Behaviour {
 public:
  explicit JoinLeaveBehaviour(Params& params)
      : stay_(params.interval("stay", {30.0, 120.0})),
        away_(params.interval("away", {5.0, 20.0})),
        walk_({{2.0, 8.0}, 0.2, true}),
        chatter_(params.interval("chat_every", {40.0, 150.0}), {0.0, 0.0}) {
  }
  Intent step(const Situation& situation, BotRandom& random) override {
    Intent intent;
    if (leave_at_ < 0.0) {
      leave_at_ = situation.now + random.between(stay_);
    }
    walk_.step(situation, random, intent);
    chatter_.step(situation, random, intent);
    if (situation.now >= leave_at_) {
      intent.leave = true;
      intent.rejoin_at = situation.now + random.between(away_);
      // The next stay is counted from the next join (a new behaviour state then).
      leave_at_ = -1.0;
    }
    return intent;
  }

 private:
  Range stay_;
  Range away_;
  AreaWalk walk_;
  Chatter chatter_;
  double leave_at_ = -1.0;
};

}  // namespace kcd2s::local::bots
