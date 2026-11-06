#include "pickpocket.hpp"

#include <cmath>
#include <cstring>
#include <unordered_set>

#include "../action_hints/resource_action_hints.hpp"
#include "../hud/game_texts.hpp"
#include "MinHook.h"

namespace kcds::runtime::pickpocket {
namespace {

using kcd2s::integration::PickpocketReport;
using kcd2s::integration::PickpocketRule;

using Start = void(__fastcall*)(void* thief, void* victim);
using Result = double(__fastcall*)(void* manager, std::uint8_t result);
using IsStolen = bool(__fastcall*)(void* item);
using Service = void*(__fastcall*)();
using StringFromText = void*(__fastcall*)(void* string, const char* text);
using StringFree = void(__fastcall*)(void* string);
using Notify = void(__fastcall*)(void* notifications, void* text, std::uint64_t, std::uint64_t);

// The engine hint's id (negative: the client's own) and how it is taken.
constexpr std::int64_t kRobHint = -52;
constexpr double kHoldSeconds = 0.5;
// How close behind the victim the thief stands, in metres, and how far behind and how straight
// at the victim it must be (cosines).
constexpr float kReach = 1.4f;
constexpr float kBehind = 0.35f;
constexpr float kFacing = 0.5f;
// The HUD's pickpocketing results ('s second argument).
constexpr std::uint8_t kNoticedByVictim = 2;
constexpr std::uint8_t kSeenBySomeone = 6;
// The UI service: its notification holder, and that holder's notification interface.
constexpr std::size_t kServiceNotifications = 344;
constexpr std::size_t kNotificationInterface = 96;
// The localization key the victim's notice is shown under (the game's notices take keys).
constexpr char kNoticedKey[] = "kcds_pickpocket_noticed";

Natives natives;
Logger log_line = nullptr;
Result original_result = nullptr;
IsStolen original_is_stolen = nullptr;
// Whether the game marks items as stolen (PICKPOCKET_RULE stolen marks).
bool stolen_marks = true;
bool offered = false;
// Whether the server lets the game offer Rob (PICKPOCKET_RULE enabled).
bool enabled = true;
// The player a theft was asked for or is under way against: no hint is offered on it while the
// game's own charge and search hints are up, until the theft ends, the server refuses it or the
// local player steps away.
std::uint32_t robbing = 0;
// A request the server has not answered yet: the actors to start with.
struct Pending {
  std::uint32_t victim = 0;
  void* thief = nullptr;
  void* target = nullptr;
};
Pending pending;
// The victim of the theft under way (started), for its result.
std::uint32_t theft = 0;

bool behind_and_facing(const Body& thief, const Body& victim, float& distance) {
  const float dx = thief.position[0] - victim.position[0];
  const float dy = thief.position[1] - victim.position[1];
  distance = std::sqrt(dx * dx + dy * dy);
  if (distance > kReach || distance < 0.05f ||
      std::fabs(thief.position[2] - victim.position[2]) > 1.0f) {
    return false;
  }
  const float to_thief_x = dx / distance;
  const float to_thief_y = dy / distance;
  // The thief is behind the victim: the victim faces away from it.
  const float victim_facing = victim.forward[0] * to_thief_x + victim.forward[1] * to_thief_y;
  // The thief faces the victim.
  const float thief_facing = -(thief.forward[0] * to_thief_x + thief.forward[1] * to_thief_y);
  return victim_facing < -kBehind && thief_facing > kFacing;
}

void withdraw() {
  if (offered) {
    action_hints::clear_engine_hint(kRobHint);
    offered = false;
  }
}

void report(PickpocketReport::Kind kind, std::uint32_t victim) {
  PickpocketReport message;
  message.kind = kind;
  message.victim = victim;
  if (!natives.send_report || !natives.send_report(message)) {
    if (log_line) {
      log_line("PICKPOCKET report %u for player=%u could not go", unsigned(kind), victim);
    }
  }
}

// The game's own HUD notification with a text, under a KCDS localization key.
void notify(const std::string& text) {
  if (!natives.notifications || !natives.string_from_text || !natives.string_free) {
    return;
  }
  auto* service = static_cast<std::uint8_t*>(reinterpret_cast<Service>(natives.notifications)());
  void* holder = service ? *reinterpret_cast<void**>(service + kServiceNotifications) : nullptr;
  void* notices =
      holder
          ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(holder) + kNotificationInterface)
          : nullptr;
  if (!notices) {
    return;
  }
  hud::set_text(kNoticedKey, text);
  void* string = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&string, kNoticedKey);
  const auto show = reinterpret_cast<Notify>((*static_cast<void***>(notices))[0]);
  show(notices, &string, 0, 0);
  reinterpret_cast<StringFree>(natives.string_free)(&string);
}

bool __fastcall hooked_is_stolen(void* item) {
  return stolen_marks && original_is_stolen(item);
}

double __fastcall hooked_result(void* manager, std::uint8_t result) {
  if (theft != 0) {
    const std::uint32_t victim = theft;
    theft = 0;
    // The game's minigame is over: Rob is offered again at once, also on the same victim.
    robbing = 0;
    if (result == kNoticedByVictim) {
      report(PickpocketReport::Kind::Caught, victim);
    } else if (result == kSeenBySomeone) {
      report(PickpocketReport::Kind::Seen, victim);
    } else {
      report(PickpocketReport::Kind::Finished, victim);
    }
    if (log_line) {
      log_line("PICKPOCKET theft from player=%u ended with result %u", victim, unsigned(result));
    }
  }
  return original_result(manager, result);
}

// The server's NPCs, whose pickpocketing never starts.
std::unordered_set<void*> unrobbable;
Start original_start = nullptr;

void __fastcall hooked_start(void* thief, void* victim) {
  if (unrobbable.count(victim)) {
    if (log_line) {
      log_line("PICKPOCKET of a server NPC %p refused", victim);
    }
    return;
  }
  original_start(thief, victim);
}

// human:CanBeRobbed on a server NPC answers false to the script, so Rob is not offered at all.
using CanBeRobbed = std::int64_t(__fastcall*)(void* binding, void* arguments);
using RobbedActor = void*(__fastcall*)(void* binding, void* arguments);
using ReturnBool = std::int64_t(__fastcall*)(void* arguments, bool* value);
CanBeRobbed original_can_be_robbed = nullptr;

std::int64_t __fastcall hooked_can_be_robbed(void* binding, void* arguments) {
  void* actor = reinterpret_cast<RobbedActor>(natives.robbed_actor)(binding, arguments);
  if (actor && unrobbable.count(actor)) {
    bool robbable = false;
    return reinterpret_cast<ReturnBool>(natives.return_bool)(arguments, &robbable);
  }
  return original_can_be_robbed(binding, arguments);
}

}  // namespace

void set_unrobbable(std::vector<void*> actors) {
  unrobbable.clear();
  unrobbable.insert(actors.begin(), actors.end());
}

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.start || !natives.send_report) {
    return false;
  }
  if (MH_CreateHook(natives.start, reinterpret_cast<void*>(hooked_start),
                    reinterpret_cast<void**>(&original_start)) != MH_OK ||
      MH_EnableHook(natives.start) != MH_OK) {
    return false;
  }
  if (natives.result && (MH_CreateHook(natives.result, reinterpret_cast<void*>(hooked_result),
                                       reinterpret_cast<void**>(&original_result)) != MH_OK ||
                         MH_EnableHook(natives.result) != MH_OK)) {
    return false;
  }
  if (natives.is_stolen &&
      (MH_CreateHook(natives.is_stolen, reinterpret_cast<void*>(hooked_is_stolen),
                     reinterpret_cast<void**>(&original_is_stolen)) != MH_OK ||
       MH_EnableHook(natives.is_stolen) != MH_OK)) {
    return false;
  }
  if (natives.can_be_robbed && natives.robbed_actor && natives.return_bool &&
      (MH_CreateHook(natives.can_be_robbed, reinterpret_cast<void*>(hooked_can_be_robbed),
                     reinterpret_cast<void**>(&original_can_be_robbed)) != MH_OK ||
       MH_EnableHook(natives.can_be_robbed) != MH_OK)) {
    return false;
  }
  return true;
}

void tick(const Body& local, const std::vector<Body>& replicas, bool allowed) {
  if (!allowed || !enabled || !natives.start || !local.actor) {
    withdraw();
    return;
  }
  const Body* victim = nullptr;
  float best = kReach;
  for (const Body& body : replicas) {
    float distance = 0;
    if (body.actor && behind_and_facing(local, body, distance) && distance <= best) {
      best = distance;
      victim = &body;
    }
  }
  if (!victim) {
    robbing = 0;
    withdraw();
    return;
  }
  if (victim->player == robbing) {
    withdraw();
    return;
  }
  action_hints::EngineHint hint;
  hint.id = kRobHint;
  hint.key = "primary_action";
  hint.text = "@ui_hud_basic_steal";
  hint.localized = true;
  hint.hold = true;
  hint.hold_seconds = kHoldSeconds;
  void* thief = local.actor;
  void* target = victim->actor;
  const std::uint32_t player = victim->player;
  hint.on_fire = [thief, target, player] {
    if (log_line) {
      log_line("PICKPOCKET local player asks to rob player=%u", player);
    }
    robbing = player;
    pending = Pending{player, thief, target};
    report(PickpocketReport::Kind::Request, player);
  };
  action_hints::set_engine_hint(hint);
  offered = true;
}

void apply_rule(const PickpocketRule& rule) {
  switch (rule.kind) {
    case PickpocketRule::Kind::Enabled:
      enabled = rule.flag;
      if (log_line) {
        log_line("PICKPOCKET Rob %s by the server", enabled ? "offered" : "not offered");
      }
      break;
    case PickpocketRule::Kind::Answer: {
      const Pending asked = pending;
      pending = Pending{};
      if (asked.victim == 0 || asked.victim != rule.player) {
        break;
      }
      if (log_line) {
        log_line("PICKPOCKET robbing player=%u %s", asked.victim,
                 rule.flag ? "allowed" : "refused by the server");
      }
      if (!rule.flag) {
        robbing = 0;
      } else {
        theft = asked.victim;
        reinterpret_cast<Start>(natives.start)(asked.thief, asked.target);
      }
      break;
    }
    case PickpocketRule::Kind::StolenMarks:
      stolen_marks = rule.flag;
      if (log_line) {
        log_line("PICKPOCKET items %s marked stolen", stolen_marks ? "are" : "are never");
      }
      break;
    case PickpocketRule::Kind::Noticed:
      if (log_line) {
        log_line("PICKPOCKET caught player=%u at the local player's pockets", rule.player);
      }
      if (!rule.text.empty()) {
        notify(rule.text);
      }
      break;
  }
}

}  // namespace kcds::runtime::pickpocket
