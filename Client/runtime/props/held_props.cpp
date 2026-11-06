#include "held_props.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "prop_entity.hpp"

namespace kcds::runtime::props {
namespace {

struct Model {
  const char* file;
  const char* material;  // empty for the model's own
  // The reader's take-out and put-away (the book's reading clips) also play on the replica's body;
  // the reader starts them by name, which does not replicate as its other clips do.
  bool body_clips;
};

// The vanilla documents' models (Tables.pak, item.xml).
const Model* model_of(std::int64_t prop) {
  static const Model kBookRed{"Objects/characters/assets/book/book_01.cdf",
                              "Objects/characters/assets/book/book_red", true};
  static const Model kBookPlain{"Objects/characters/assets/book/book_01.cdf",
                                "Objects/characters/assets/book/book_01", true};
  static const Model kLetter{"Objects/characters/assets/parchment_folded/parchment_folded.cdf", "",
                             false};
  switch (prop) {
    case HeldProp::kBookRed:
      return &kBookRed;
    case HeldProp::kBookPlain:
      return &kBookPlain;
    case HeldProp::kLetter:
      return &kLetter;
    default:
      return nullptr;
  }
}

// Where the document sits: 1:1 where the game's own inventory reading puts it (C_Reading
// and its spawn callback), attached to the reader's entity, not to a joint, at
// (0, 0.2, 1.35), turned 45 degrees down about x (the quaternion sin/cos of -pi/8); the reader's
// own document entity reads the same local transform. One read from the inventory, without
// the take-out and put-away, is in this placement.
//
// A document taken out and put away is in the right hand the whole time (the RightHand joint; the
// RightHandSlot joint stays at the back of the right hip until the grab, so the book there was
// nowhere near the hand, live), at the offset that is the reading placement in the reading pose
// (kHandPlacement). Measured on a replica (local frame, 50 ms samples): the right hand
// reaches across to the left hip, takes the book there 0.8 s into the take-out and has it up in the
// reading pose at 1.35 s; the put-away takes it back to the left hip in 1.75 s. The document is in
// the hand from the grab to the belt, closed on the way, and is never spawned again in between
// (each new document blinked, live).
struct Placement {
  const char* bone;  // nullptr: the holder's entity itself
  float x;
  float y;
  float z;
  float ax;
  float ay;
  float az;
};
const Placement kReadingPlacement{nullptr, 0, 0.2f, 1.35f, -0.785398f, 0, 0};
// Measured in the reading loop: the RightHand joint at (2.547, 1.020, -0.852) rad and
// 12.5 cm from the reading placement; this offset puts the document there.
const Placement kHandPlacement{"RightHand", -0.0357f, 0.0986f, 0.0673f, 2.3406f, 1.1222f, -0.3489f};

// The document's clips, named as the body's reading clips they go with.
constexpr const char* kTakeOutClip = "reading_stand_inv_in";
constexpr const char* kReadingClip = "reading_stand_inv_loop";
// The game's reading has no page-turn clip of the body; the document turns a page (book.dba
// list_next, 0.667 s; live: a page flips over, the reading variations show none) and stays on
// its last frame. Going back to the reading loop after it flipped the page back, and the
// clip started again on its held last frame did not play, so every turn restarts it.
constexpr const char* kPageTurnClip = "list_next";
constexpr const char* kPageTurnBackClip = "list_prev";
// The put-away closes the document (book.dba close) in the hands. The reading's put-away clip
// (reading_stand_inv_out) swung the open book out of the hand, around the hip.
constexpr const char* kPutAwayClip = "close";
// A closed document: the open clip held on its first frame.
constexpr const char* kClosedClip = "open";
// The owner's body puts the book away with the reading's own clip (kcd_book_database.adb
// ReadingBookOut).
constexpr const char* kBodyPutAwayClip = "reading_stand_inv_out";
// Into the take-out: the right hand takes the book at the left hip, then has it up in the reading
// pose, where it opens (measured, see kHandPlacement).
constexpr std::chrono::milliseconds kGrabAfterTakeOut{800};
constexpr std::chrono::milliseconds kReadingAfterTakeOut{1350};
// Into the put-away: the hand has the book back at the left hip, where it goes.
constexpr std::chrono::milliseconds kHideAfterPutAway{1750};

// SChildAttachParams: no flags attaches to the parent entity itself, as the game's reading does;
// flag 4 attaches to a joint of the parent's character by name.
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
constexpr std::int32_t kAttachToEntity = 0;
constexpr std::int32_t kAttachToJoint = 4;

using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);

Natives natives;

// A document holder: a remote player (its network id) or the local reader (kLocalHolder).
constexpr std::uint32_t kLocalHolder = 0;

// Engine thread only.
std::unordered_map<std::uint32_t, std::int64_t> shown;  // player -> prop on the replica
// The state bits of HeldProp next to the model: closing (the book is put away) and a page-turn
// count (bits 8-15).
constexpr std::int64_t kClosing = 16;
// Taken out and put away with the owner's animations; without it (a document read from the
// inventory, whose reading has neither) the document is shown reading and removed when closed.
constexpr std::int64_t kAnimated = 32;
constexpr std::int64_t kModelMask = 15;
constexpr int kTurnShift = 8;
// The last page turn went back.
constexpr std::int64_t kTurnBack = 64;
// holder -> the placement its document waits for, and the frames waited
struct PendingAttach {
  const Placement* placement;
  int frames;
};
std::unordered_map<std::uint32_t, PendingAttach> pending_attach;
// holder -> when the hand takes its document in the take-out, and which one
struct PendingGrab {
  std::chrono::steady_clock::time_point at;
  const Model* model;
};
std::unordered_map<std::uint32_t, PendingGrab> pending_grab;
// holder -> when its taken-out document opens in the reading pose
std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> pending_reading;
// holder -> when its put-away document goes with the lowered hands
std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> pending_hide;
// holder -> the model and placement of its document.
struct Held {
  const Model* model;
  const Placement* placement;
};
std::unordered_map<std::uint32_t, Held> held;

// holder -> the generation of its document. Every spawn has a new name: the entity system removes
// an entity at the end of the frame, so removing the old document and spawning one of the same
// name in one frame let the attach find the old one, which was then removed (no document in
// the hand at the put-away).
std::unordered_map<std::uint32_t, unsigned> generation;

std::string prop_name(std::uint32_t holder) {
  char name[64];
  std::snprintf(name, sizeof(name), "kcds_prop_%u_%u", holder, generation[holder]);
  return name;
}

std::string replica_name(std::uint32_t player) {
  char name[48];
  std::snprintf(name, sizeof(name), "kcd2s_remote_%u", player);
  return name;
}

// The holder's entity: the local player or the replica.
void* owner_of(std::uint32_t holder) {
  if (holder == kLocalHolder) {
    return natives.local_entity ? natives.local_entity() : nullptr;
  }
  return entity::find(replica_name(holder));
}

void remove_prop(std::uint32_t holder) {
  entity::remove(prop_name(holder));
}

// A one-shot clip on the holder's body.
void play_body(std::uint32_t holder, const char* clip) {
  void* owner = owner_of(holder);
  if (owner) {
    entity::play(owner, {clip, 0.15f, 1.0f, false});
  }
}

// Spawns the holder's document playing clip, without physics (its collider pushed the replica
// away from the owner's position every frame: shaking legs and the document off the hands, live);
// it is attached on a later frame (attach).
void spawn_prop(std::uint32_t holder, const Model& model, const char* clip, bool still) {
  remove_prop(holder);
  ++generation[holder];
  void* owner = owner_of(holder);
  if (!owner) {
    return;
  }
  float position[3];
  entity::world_position(owner, position);
  void* prop = entity::spawn(prop_name(holder), position, model.file, model.material);
  if (!prop) {
    if (natives.log) {
      natives.log("PROPS holder=%u document not spawned", holder);
    }
    return;
  }
  if (still) {
    // Held on the clip's first frame.
    entity::play(prop, {clip, 0.0f, 0.0f, false});
  } else {
    entity::play(prop, {clip, 0.15f, 1.0f, false});
  }
}

// Attaches the holder's document at placement; false while either is not there yet.
bool attach(std::uint32_t holder, const Placement& placement) {
  if (!natives.attach_child) {
    return false;
  }
  void* owner = owner_of(holder);
  void* prop = entity::find(prop_name(holder));
  if (!owner || !prop) {
    return false;
  }
  const AttachParams params{placement.bone ? kAttachToJoint : kAttachToEntity, placement.bone};
  reinterpret_cast<AttachChild>(natives.attach_child)(owner, prop, &params);
  if (natives.log) {
    natives.log("PROPS attached holder=%u at %s", holder,
                placement.bone ? placement.bone : "entity");
  }
  const float position[3] = {placement.x, placement.y, placement.z};
  const float angles[3] = {placement.ax, placement.ay, placement.az};
  entity::place(prop, position, angles);
  return true;
}

// Plays a clip on the holder's document; restart plays it from its start even when it is the
// clip the document holds.
void play(std::uint32_t holder, const char* animation, bool loop, bool restart = false) {
  void* prop = entity::find(prop_name(holder));
  if (!prop) {
    return;
  }
  if (restart) {
    entity::stop(prop);
    entity::play(prop, {animation, 0.0f, 1.0f, loop});
  } else {
    entity::play(prop, {animation, 0.15f, 1.0f, loop});
  }
  if (natives.log) {
    natives.log("PROPS holder=%u clip=%s loop=%u", holder, animation, unsigned(loop));
  }
}

// Spawns the holder's document at placement playing clip.
void place(std::uint32_t holder, const Model& model, const Placement& placement, const char* clip,
           bool still = false) {
  spawn_prop(holder, model, clip, still);
  pending_attach[holder] = {&placement, 0};
  held[holder] = {&model, &placement};
}

// The owner's body clips started by name replicate as its others do (the replica got the
// take-out), so only the document is taken out here: into the hand at the grab, opened when read.
void take_out(std::uint32_t holder, const Model& model) {
  const auto now = std::chrono::steady_clock::now();
  pending_grab[holder] = {now + kGrabAfterTakeOut, &model};
  pending_reading[holder] = now + kReadingAfterTakeOut;
}

void remove(std::uint32_t holder) {
  remove_prop(holder);
  pending_attach.erase(holder);
  pending_grab.erase(holder);
  pending_reading.erase(holder);
  pending_hide.erase(holder);
  held.erase(holder);
}

// Puts the held document away: it closes in the hands, which take it to the belt. Put away before
// the grab, it never shows.
void put_away(std::uint32_t holder) {
  pending_grab.erase(holder);
  pending_reading.erase(holder);
  if (held.count(holder) == 0) {
    return;
  }
  play(holder, kPutAwayClip, false);
  pending_hide[holder] = std::chrono::steady_clock::now() + kHideAfterPutAway;
}

}  // namespace

// The local reader sees the game's own reading document; a document of ours in its hand as well
// flashed before the reading took over and lay the wrong way round in the first-person view
//. Only its body plays the take-out and put-away, which reach other players.
void local_take_out(std::int64_t prop) {
  const Model* model = model_of(prop & kModelMask);
  if (model && model->body_clips) {
    play_body(kLocalHolder, kTakeOutClip);
  }
}

void local_put_away(std::int64_t prop) {
  const Model* model = model_of(prop & kModelMask);
  if (model && model->body_clips) {
    play_body(kLocalHolder, kBodyPutAwayClip);
  }
}

void local_remove() {
  remove(kLocalHolder);
}

void install(const Natives& given) {
  natives = given;
  entity::Natives entity_natives;
  entity_natives.entity_system = natives.entity_system;
  entity_natives.spawn_params_init = natives.spawn_params_init;
  entity_natives.physics_component = natives.physics_component;
  entity_natives.engine_3d = natives.engine_3d;
  entity::install(entity_natives);
}

void observe(std::uint32_t player, std::int64_t value) {
  const std::int64_t prop = value & kModelMask;
  const Model* model = model_of(prop);
  auto found = shown.find(player);
  const std::int64_t state = found == shown.end() ? HeldProp::kNone : found->second;
  const std::int64_t current = state & kModelMask;
  if (model && prop == current) {
    // The same document: put it away or turn its page with the owner's body.
    if ((value & kClosing) && !(state & kClosing)) {
      if (value & kAnimated) {
        put_away(player);
      } else {
        remove(player);
      }
    } else if (!(value & kClosing) && (value >> kTurnShift) != (state >> kTurnShift)) {
      // Still in the take-out the document is not read yet.
      if (held.count(player) != 0 && pending_reading.count(player) == 0 &&
          pending_hide.count(player) == 0) {
        play(player, (value & kTurnBack) ? kPageTurnBackClip : kPageTurnClip, false, true);
      }
    }
    shown[player] = value;
    return;
  }
  if (value == state || (!model && current == HeldProp::kNone)) {
    return;
  }
  if (natives.log) {
    natives.log("PROPS player=%u prop=%lld", player, static_cast<long long>(prop));
  }
  if (model) {
    if (value & kAnimated) {
      take_out(player, *model);
    } else {
      place(player, *model, kReadingPlacement, kReadingClip);
      play(player, kReadingClip, true);
    }
    shown[player] = value;
  } else {
    // A document still on its way to the belt goes when it gets there (frame).
    if (pending_hide.count(player) == 0) {
      remove(player);
    }
    shown.erase(player);
  }
}

void forget(std::uint32_t player) {
  pending_attach.erase(player);
  pending_grab.erase(player);
  pending_reading.erase(player);
  pending_hide.erase(player);
  held.erase(player);
  if (shown.erase(player) != 0) {
    remove_prop(player);
  }
}

void reset() {
  shown.clear();
  pending_attach.clear();
  pending_grab.clear();
  pending_reading.clear();
  pending_hide.clear();
  held.clear();
}

void frame() {
  for (auto it = pending_attach.begin(); it != pending_attach.end();) {
    // The replica may not be there yet; give up after a few seconds.
    if (attach(it->first, *it->second.placement) || ++it->second.frames > 300) {
      it = pending_attach.erase(it);
    } else {
      ++it;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  for (auto it = pending_grab.begin(); it != pending_grab.end();) {
    if (now < it->second.at) {
      ++it;
      continue;
    }
    const std::uint32_t holder = it->first;
    const Model& model = *it->second.model;
    it = pending_grab.erase(it);
    place(holder, model, kHandPlacement, kClosedClip, true);
  }
  for (auto it = pending_reading.begin(); it != pending_reading.end();) {
    if (now < it->second) {
      ++it;
      continue;
    }
    const std::uint32_t holder = it->first;
    it = pending_reading.erase(it);
    if (held.count(holder) != 0) {
      play(holder, kReadingClip, true);
    }
  }
  for (auto it = pending_hide.begin(); it != pending_hide.end();) {
    if (now < it->second) {
      ++it;
      continue;
    }
    const std::uint32_t holder = it->first;
    it = pending_hide.erase(it);
    remove(holder);
  }
}

}  // namespace kcds::runtime::props
