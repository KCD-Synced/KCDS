#pragma once
#include "kcd2s/replication/world.hpp"

namespace kcd2s::local {
// Shared by the real socket host, diagnostic clients, and the future game adapter.
// Player identity is separate from the one locally controlled engine actor.
enum PlayerField : core::FieldId {
  Position,
  Rotation,
  Velocity,
  Grounded,
  Gait,
  JumpSequence,
  Health,
  PoseValid,
  MotionType,
  MotionVelocity,
  MotionRotation,
  MotionFlags,
  MotionValid,
  MotionParams0,
  MotionParams1,
  MotionParams2,
  MotionParams3,
  MotionParams4,
  MotionParams5,
  MotionMask,
  Animations,
  LookTarget,
  LookEnabled,
  LookValid,
  LookTuning,
  NativeRiderState,
  UiBlocking,
  HeldProp,
  RagdollPose,
  Outfit,
  Appearance,
  BodyVisible,
  BodyParts,
  Disguise,
  Seats,
  Hands,
  Nickname,
  PlayerIndex,
  Vitals,
  Stats,
  Abilities,
  LookDirection,
  Posture,
  MotionClock,
  Carry,
  WeaponAnimations,
  AimTarget,
  AimEnabled,
  WeaponState,
  Language,
  Grime,
  HumanOverlayAnimation,
  RiposteAbility,
  ReplicaSoul,
  ReplicaBuffs,
  AnimationProps,
  StationAction,
  Nameplate,
  VisorClosed,
  KeyBypass,
  Brand,
  PhysicalProp,
  PoseAnimation,
  LeadRope
};
enum class Locomotion : std::int64_t { Idle, Walk, Run, Sprint, Jump, Fall };
// The motion fields on the wire: positions on a 1/256 m grid within 16 km of the origin (23 bits
// a component instead of 32), velocities on a 1/128 m/s grid up to 128 m/s (15 bits), rotations as
// the three smallest components of 12 bits (38 bits instead of 128, about a 20th of a degree).
// With hundreds of players in one place most of a snapshot is their motion.
inline replication::Quant position_quant() {
  return replication::grid_quant(1.0f / 256.0f, 23);
}
inline replication::Quant velocity_quant() {
  return replication::grid_quant(1.0f / 128.0f, 15);
}
inline replication::Quant rotation_quant() {
  return replication::Quant{0.0f, 0.0f, 12};
}

inline replication::SchemaRegistry player_schema() {
  using namespace replication;
  SchemaRegistry s;
  s.define("NetworkPlayer")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner, position_quant())
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Owner, rotation_quant())
      .field("velocity", FieldKind::Vec3, Priority::High, Authority::Owner, velocity_quant())
      .field("grounded", FieldKind::Bool, Priority::High, Authority::Owner)
      .field("locomotion", FieldKind::I64, Priority::High, Authority::Owner)
      .field("jumpSequence", FieldKind::I64, Priority::High, Authority::Owner)
      .field("health", FieldKind::F32, Priority::Med, Authority::Server)
      .field("poseValid", FieldKind::Bool, Priority::High, Authority::Owner)
      .field("motionType", FieldKind::I64, Priority::High, Authority::Owner)
      .field("motionVelocity", FieldKind::Vec3, Priority::High, Authority::Owner, velocity_quant())
      .field("motionRotation", FieldKind::Quat, Priority::High, Authority::Owner, rotation_quant())
      .field("motionFlags", FieldKind::I64, Priority::High, Authority::Owner)
      .field("motionValid", FieldKind::Bool, Priority::High, Authority::Owner)
      .field("motionParams0", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("motionParams1", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("motionParams2", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("motionParams3", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("motionParams4", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("motionParams5", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("motionMask", FieldKind::I64, Priority::High, Authority::Owner)
      .field("animations", FieldKind::Str, Priority::High, Authority::Owner)
      .field("lookTarget", FieldKind::Vec3, Priority::High, Authority::Owner, position_quant())
      .field("lookEnabled", FieldKind::Bool, Priority::High, Authority::Owner)
      .field("lookValid", FieldKind::Bool, Priority::High, Authority::Owner)
      .field("lookTuning", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("nativeRiderState", FieldKind::Str, Priority::High, Authority::Owner)
      // True while the owner has a full-screen game UI open (inventory, map, journal). Its
      // client cannot detect weapon contacts then, so the attacker's client evaluates them.
      .field("uiBlocking", FieldKind::Bool, Priority::Med, Authority::Owner)
      // What the owner holds that other machines must show on its replica (0 none; the ids are
      // Client/runtime/props/held_props.hpp), such as a resource's book in the hands.
      .field("heldProp", FieldKind::I64, Priority::Med, Authority::Owner)
      // The parts of the owner's ragdoll (position and rotation of each physics part relative to
      // the entity) while it lies as one, so other clients show the same fall; empty otherwise.
      .field("ragdollPose", FieldKind::Str, Priority::High, Authority::Owner)
      // The classes of the clothing the owner wears (Client/runtime/appearance/outfit.hpp), so
      // its replica wears the same; empty until the owner reports it.
      .field("outfit", FieldKind::Str, Priority::Med, Authority::Owner)
      // What the owner's body looks like (kcd2s/integration/appearance.hpp, appearance_field), so
      // its replica looks the same and the resources read it; empty until the owner reports it.
      .field("appearance", FieldKind::Str, Priority::Med, Authority::Owner)
      // Whether the owner draws its body (Player.setVisible), so every other client draws its
      // replica the same.
      .field("bodyVisible", FieldKind::Bool, Priority::Med, Authority::Owner)
      // The five components the owner's body is built from (appearance.hpp, body_parts_field),
      // which its replica is built from too; empty until the owner reports them.
      .field("bodyParts", FieldKind::Str, Priority::Med, Authority::Owner)
      // The mesh the owner's body is drawn as (Player.setDisguise), "" for its own body.
      .field("disguise", FieldKind::Str, Priority::Med, Authority::Owner)
      // The seats (smart objects) the owner holds (kcd2s/integration/seats.hpp, seats_field),
      // so every other client holds them for its replica; "" when it holds none.
      .field("seats", FieldKind::Str, Priority::Med, Authority::Owner)
      // The item classes in the owner's hands, such as a drawn sword and a shield
      // (kcd2s/integration/hands.hpp), so its replica holds the same; "" for empty hands.
      .field("hands", FieldKind::Str, Priority::High, Authority::Owner)
      // The name the player connected under, as the server cleaned it
      // (kcd2s/integration/player_identity.hpp); "" until its client named it.
      .field("nickname", FieldKind::Str, Priority::Low, Authority::Server)
      // The connection slot the player holds (Player.playerIndex), 65535 while it has none.
      .field("playerIndex", FieldKind::I64, Priority::Low, Authority::Server)
      // The owner's four writable states and five conditions (kcd2s/integration/player_stats.hpp,
      // vitals_field), which Player.getStat and derivedStat read; "" until it reports.
      .field("vitals", FieldKind::Str, Priority::Med, Authority::Owner)
      // The owner's other derived stats and relative attributes, sampled every 250 ms
      // (player_stats.hpp, stats_field); "" until it reports.
      .field("stats", FieldKind::Str, Priority::Low, Authority::Owner)
      // What the owner's body can hold and do: maxima, main level, attributes and skills,
      // sampled every 250 ms (player_stats.hpp, abilities_field); "" until it reports.
      .field("abilities", FieldKind::Str, Priority::Low, Authority::Owner)
      // Where the owner's head and eyes are turned: the unit vector from its eyes (its own
      // camera) to its look target, to a thousandth; zero until it reports one.
      .field("gaze", FieldKind::Vec3, Priority::Med, Authority::Owner)
      // How the owner's body stands: off the ground, crouched, its physics profile
      // (kcd2s/integration/posture.hpp, posture_field); 0 until it reports.
      .field("posture", FieldKind::I64, Priority::High, Authority::Owner)
      // The owner's clock (ms) when it sent its movement: replicas place its samples on its own
      // timeline and show it smoothly (kcd2s/integration/replica_timeline.hpp);
      // 0 for an owner that sends none.
      .field("motionClock", FieldKind::I64, Priority::High, Authority::Owner)
      // The item class the owner carries in its arms (kcd2s/integration/carry.hpp, carry_field),
      // so its replica carries it too and the resources hear it; "" for none.
      .field("carry", FieldKind::Str, Priority::High, Authority::Owner)
      // The clips the owner's game started on the weapon in its hands, a bow bending, a crossbow's
      // string, a hand cannon's match with its smoke and sparks (animation.hpp's form, as
      // `animations`), so its replica's weapon plays them too; "" until it holds one.
      .field("weaponAnimations", FieldKind::Str, Priority::High, Authority::Owner)
      // Where the owner's drawn bow, crossbow or gun points, up and down included: its aim pose
      // blender's target in the world and whether it aims; zero and off until it does.
      .field("aimTarget", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("aimEnabled", FieldKind::Bool, Priority::High, Authority::Owner)
      // The state of the owner's bow, crossbow or gun (ranged/shots.hpp weapon_state: 1 loaded,
      // 2 ready), which its replica's weapon takes; 0 without one.
      .field("weaponState", FieldKind::I64, Priority::High, Authority::Owner)
      // The language the owner's game shows ("english", "czech", ...: its g_language), which
      // chooses the KCDS texts a server's locale gives it; "" until it reports.
      .field("language", FieldKind::Str, Priority::Low, Authority::Owner)
      // How dirty, bloody and worn the owner's body and worn pieces are
      // (Client/runtime/appearance/grime.hpp), which its replica shows; "" until reported.
      .field("grime", FieldKind::Str, Priority::Low, Authority::Owner)
      // The observed human overlay layer uses portable alias identity, never a native clip index.
      .field("humanOverlay", FieldKind::Str, Priority::High, Authority::Owner)
      // Server projection of the owner's validated progression report: -1 until
      // known, then 0/1. Replicas mirror the actual native ability perk.
      .field("riposteAbility", FieldKind::I64, Priority::High, Authority::Server)
      // The owner's track levels and directly owned perks (kcd2s/integration/replica_soul.hpp,
      // replica_soul_field), which every other client gives its replica's soul so the game's
      // combat math there uses the player's real strength, skills and perks; "" until
      // the owner reports it.
      .field("replicaSoul", FieldKind::Str, Priority::Low, Authority::Owner)
      // The owner's buffs that change how it fights, potions, drinks, injuries
      // (kcd2s/integration/replica_buffs.hpp, replica_buffs_field), which every other client gives
      // its replica's soul; "" for none.
      .field("replicaBuffs", FieldKind::Str, Priority::Low, Authority::Owner)
      // The props the owner's script animation holds in its hands (Player.playAnimation `props`,
      // kcd2s/integration/animation_props.hpp), which every client shows on its body; ""
      // for none.
      .field("animationProps", FieldKind::Str, Priority::Med, Authority::Owner)
      // What the owner does at a crafting station, the station's tools and workpiece where its
      // game holds them and how hot the forge and the work are
      // (kcd2s/integration/station_action.hpp), which every client shows on its replica;
      // "" while it works at none.
      .field("stationAction", FieldKind::Str, Priority::Med, Authority::Owner)
      // What the server says about the player's nameplate (kcd2s/integration/nameplates.hpp,
      // nameplate_field: shown, health bar, colour, text), which every client draws; "" for
      // the default tag.
      .field("nameplate", FieldKind::Str, Priority::Low, Authority::Server)
      // Whether the owner's helmet has its visor down (Player.visorDown); false
      // without a helmet that has one.
      .field("visorDown", FieldKind::Bool, Priority::Med, Authority::Owner)
      // This player opens every lock with no key (player.keyBypass).
      .field("keyBypass", FieldKind::Bool, Priority::Low, Authority::Server)
      // The game's brand on the body (Player.brand): 0 none, 1 or 2 the two marks.
      .field("brand", FieldKind::I64, Priority::Low, Authority::Server)
      // The rope or whip in the player's right hand (Player.setPhysicalProp;
      // kcd2s/integration/physical_prop.hpp): "whip 20 1.60", "" for none.
      .field("physicalProp", FieldKind::Str, Priority::Low, Authority::Server)
      // The studio's pose animation on the body (Player.setAnimation;
      // kcd2s/integration/pose_animation.hpp): "A1LS|0:RightArm=40,0,0|...", "" for none.
      .field("poseAnimation", FieldKind::Str, Priority::Low, Authority::Server)
      // The lead rope the player holds (Player.setLeadRope; kcd2s/integration/lead_rope.hpp):
      // "12|RightHand|Spine|2.50", "" for none.
      .field("leadRope", FieldKind::Str, Priority::Low, Authority::Server);
  s.define("NetworkHorse")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Server, position_quant())
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Server, rotation_quant())
      .field("soul", FieldKind::Str, Priority::High, Authority::Server)
      .field("name", FieldKind::Str, Priority::High, Authority::Server)
      .field("scriptId", FieldKind::I64, Priority::High, Authority::Server)
      .field("horseOwner", FieldKind::I64, Priority::High, Authority::Server)
      .field("dimension", FieldKind::I64, Priority::High, Authority::Server)
      .field("rider", FieldKind::I64, Priority::High, Authority::Server)
      .field("simulator", FieldKind::I64, Priority::High, Authority::Server)
      .field("simulationEpoch", FieldKind::I64, Priority::High, Authority::Server)
      .field("nativeState", FieldKind::Str, Priority::High, Authority::Server)
      // The components every client builds the horse's body from, "<body>|<head>|<hair>|<tail>"
      // (the server chooses them, so the horse looks the same everywhere).
      .field("look", FieldKind::Str, Priority::Low, Authority::Server)
      // True when a lethal blow only knocks the horse out instead of killing it.
      .field("knockOut", FieldKind::Bool, Priority::Low, Authority::Server)
      // The order a resource gave the horse (horse.hpp HorseIntent).
      .field("intent", FieldKind::Str, Priority::Med, Authority::Server)
      // What every body of the horse shows, and which of the three states it is in (horse.hpp
      // HorseHealth, HorseCondition); the life counts the wakes and revives (HorseLife).
      .field("health", FieldKind::F32, Priority::High, Authority::Server)
      .field("state", FieldKind::I64, Priority::High, Authority::Server)
      .field("life", FieldKind::I64, Priority::High, Authority::Server)
      // The most the saddlebags carry, kg; below 0 the game's own (horse.hpp HorseMaxCarryWeight).
      .field("maxCarryWeight", FieldKind::F32, Priority::Low, Authority::Server)
      // Stamina and speed; below 0 the game's own (horse.hpp HorseMaxStamina, HorseSpeed).
      .field("maxStamina", FieldKind::F32, Priority::Low, Authority::Server)
      .field("speed", FieldKind::F32, Priority::Low, Authority::Server)
      // The tack every body of the horse wears (horse.hpp HorseGear).
      .field("gear", FieldKind::Str, Priority::Low, Authority::Server)
      // The status effects the server put on it (horse.hpp HorseBuffs).
      .field("buffs", FieldKind::Str, Priority::Low, Authority::Server)
      // The players besides its owner allowed to ride it (horse.hpp HorseRiders).
      .field("riders", FieldKind::Str, Priority::Low, Authority::Server)
      // Who leads it on a rope (horse.hpp HorseLead).
      .field("lead", FieldKind::I64, Priority::Low, Authority::Server)
      // How dirty it is (horse.hpp HorseDirt).
      .field("dirt", FieldKind::F32, Priority::Low, Authority::Server)
      // The share of its pace it moves at (horse.hpp HorseSpeedScale).
      .field("speedScale", FieldKind::F32, Priority::Low, Authority::Server)
      // Where on the horse its lead rope is tied (horse.hpp HorseLeadAt).
      .field("leadAt", FieldKind::Str, Priority::Low, Authority::Server);
  // An NPC a resource spawned (Npc.create; kcd2s/integration/npc.hpp, whose
  // NpcField names these ids). One player's client simulates it; the server writes every field.
  s.define("NetworkNpc")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Server, position_quant())
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Server, rotation_quant())
      .field("soul", FieldKind::Str, Priority::High, Authority::Server)
      .field("actorClass", FieldKind::Str, Priority::High, Authority::Server)
      .field("name", FieldKind::Str, Priority::High, Authority::Server)
      .field("scriptId", FieldKind::I64, Priority::High, Authority::Server)
      .field("dimension", FieldKind::I64, Priority::High, Authority::Server)
      .field("simulator", FieldKind::I64, Priority::High, Authority::Server)
      .field("simulationEpoch", FieldKind::I64, Priority::High, Authority::Server)
      .field("velocity", FieldKind::Vec3, Priority::High, Authority::Server, velocity_quant())
      .field("nativeState", FieldKind::Str, Priority::High, Authority::Server)
      .field("teleport", FieldKind::I64, Priority::High, Authority::Server)
      .field("frozen", FieldKind::Bool, Priority::High, Authority::Server)
      .field("health", FieldKind::F32, Priority::High, Authority::Server)
      .field("alive", FieldKind::Bool, Priority::High, Authority::Server)
      // What the simulator's body wears (npc.hpp NpcOutfit, NPC_OUTFIT).
      .field("outfit", FieldKind::Str, Priority::Low, Authority::Server)
      // The clip a resource plays on the NPC (npc.hpp NpcAnimation).
      .field("animation", FieldKind::Str, Priority::Med, Authority::Server)
      // What the NPC says (npc.hpp NpcSpeech).
      .field("speech", FieldKind::Str, Priority::Med, Authority::Server)
      // The character components a resource chose for the NPC (npc.hpp NpcAppearance).
      .field("appearance", FieldKind::Str, Priority::Low, Authority::Server)
      // A resource chose what the NPC wears (npc.hpp NpcOutfitFixed).
      .field("outfitFixed", FieldKind::Bool, Priority::Low, Authority::Server)
      // One up at every revive (npc.hpp NpcLife).
      .field("life", FieldKind::I64, Priority::High, Authority::Server)
      // The order a resource gave the NPC (npc.hpp NpcIntent).
      .field("intent", FieldKind::Str, Priority::Med, Authority::Server)
      // The share of attacks the NPC defends against (npc.hpp NpcDefense).
      .field("defense", FieldKind::F32, Priority::Low, Authority::Server)
      // The share of their damage the NPC's blows on players take (npc.hpp NpcDamage).
      .field("damage", FieldKind::F32, Priority::Low, Authority::Server)
      // The NPC's nameplate and how it moves (npc.hpp NpcNameplate, NpcKinematic).
      .field("nameplate", FieldKind::Str, Priority::Low, Authority::Server)
      .field("kinematic", FieldKind::Bool, Priority::Med, Authority::Server)
      // How the simulator's dead body lies (npc.hpp NpcRagdollPose, NPC_POSE).
      .field("ragdollPose", FieldKind::Str, Priority::Low, Authority::Server)
      // Whether the game's brain runs the NPC and whether players can lock on to it
      // (npc.hpp NpcBrain, NpcTargetable).
      .field("brain", FieldKind::Bool, Priority::Med, Authority::Server)
      .field("targetable", FieldKind::Bool, Priority::Med, Authority::Server)
      // A dead animal without the game's Search and Gut (npc.hpp NpcCarcassClosed).
      .field("carcassClosed", FieldKind::Bool, Priority::Med, Authority::Server)
      // Whether players can fight the NPC (npc.hpp NpcAttackable).
      .field("attackable", FieldKind::Bool, Priority::Med, Authority::Server)
      // The style its mouth moves in, "" for none (npc.hpp NpcTalking).
      .field("talking", FieldKind::Str, Priority::Low, Authority::Server)
      // Its facial expression, "" for the game's own (npc.hpp NpcFace).
      .field("face", FieldKind::Str, Priority::Low, Authority::Server)
      // Whether players can talk to it (npc.hpp NpcInteractable).
      .field("interactable", FieldKind::Bool, Priority::Med, Authority::Server)
      // The level's own person it is, "" for a script's NPC (npc.hpp NpcLevelGuid).
      .field("levelGuid", FieldKind::Str, Priority::Med, Authority::Server);
  // A stack of items lying in the world (kcd2s/integration/ground_items.hpp).
  s.define("NetworkGroundItem")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Server)
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Server)
      .field("itemClass", FieldKind::Str, Priority::High, Authority::Server)
      .field("amount", FieldKind::I64, Priority::High, Authority::Server)
      .field("quality", FieldKind::I64, Priority::High, Authority::Server)
      .field("health", FieldKind::F32, Priority::High, Authority::Server)
      .field("resting", FieldKind::Bool, Priority::High, Authority::Server)
      .field("leftBy", FieldKind::I64, Priority::High, Authority::Server)
      .field("scriptId", FieldKind::I64, Priority::High, Authority::Server)
      .field("dimension", FieldKind::I64, Priority::High, Authority::Server)
      .field("pinned", FieldKind::Bool, Priority::High, Authority::Server);
  // A container the server holds (kcd2s/integration/stash.hpp). What it
  // holds goes only to the player who opened it, by STASH_SHOW.
  s.define("NetworkStash")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Server)
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Server)
      .field("scriptId", FieldKind::I64, Priority::High, Authority::Server)
      .field("bodyOf", FieldKind::I64, Priority::High, Authority::Server)
      .field("dimension", FieldKind::I64, Priority::High, Authority::Server)
      // Its lock: locked, lock-picking offered, the key's class GUID.
      .field("locked", FieldKind::Bool, Priority::High, Authority::Server)
      .field("canBePicked", FieldKind::Bool, Priority::High, Authority::Server)
      .field("keyClass", FieldKind::Str, Priority::High, Authority::Server)
      // An adopted level chest's level GUID, empty for any other container.
      .field("placedGuid", FieldKind::Str, Priority::High, Authority::Server)
      // Its lock-picking difficulty set by a script, below 0 the game's own.
      .field("lockLevel", FieldKind::F32, Priority::High, Authority::Server)
      // Which lock its lock-picking shows: 0 the level's, 1 common, 2 fancy.
      .field("lockStyle", FieldKind::I64, Priority::High, Authority::Server)
      // The mesh a script built it as, empty for the default chest.
      .field("model", FieldKind::Str, Priority::High, Authority::Server)
      // Whether its stock holds nothing, what its prompt says.
      .field("empty", FieldKind::Bool, Priority::High, Authority::Server);
  return s;
}
// A player's avatar under `reserved`, an id the world reserved for players (the host's player
// ids), or under the world's next id without one.
inline core::EntityId spawn_player(replication::World& w,
                                   core::EntityId reserved = core::kInvalidEntity) {
  const core::TypeId type = core::type_id("NetworkPlayer");
  auto e = reserved == core::kInvalidEntity ? w.spawn(type) : w.spawn_reserved(type, reserved);
  w.write(e, Position, core::Vec3{});
  w.write(e, Rotation, core::Quat{});
  w.write(e, Velocity, core::Vec3{});
  w.write(e, Grounded, true);
  w.write(e, Gait, std::int64_t{0});
  w.write(e, JumpSequence, std::int64_t{0});
  w.write(e, Health, 100.0f);
  w.write(e, PoseValid, false);
  w.write(e, MotionType, std::int64_t{0});
  w.write(e, MotionVelocity, core::Vec3{});
  w.write(e, MotionRotation, core::Quat{});
  w.write(e, MotionFlags, std::int64_t{0});
  w.write(e, Animations, std::string{});
  w.write(e, NativeRiderState, std::string{});
  w.write(e, UiBlocking, false);
  w.write(e, HeldProp, std::int64_t{0});
  w.write(e, RagdollPose, std::string{});
  w.write(e, Outfit, std::string{});
  w.write(e, Appearance, std::string{});
  w.write(e, BodyVisible, true);
  w.write(e, BodyParts, std::string{});
  w.write(e, Disguise, std::string{});
  w.write(e, Seats, std::string{});
  w.write(e, Hands, std::string{});
  w.write(e, Nickname, std::string{});
  w.write(e, PlayerIndex, std::int64_t{0xFFFF});
  w.write(e, Vitals, std::string{});
  w.write(e, Stats, std::string{});
  w.write(e, Abilities, std::string{});
  w.write(e, LookDirection, core::Vec3{});
  w.write(e, Posture, std::int64_t{0});
  w.write(e, MotionClock, std::int64_t{0});
  w.write(e, Carry, std::string{});
  w.write(e, WeaponAnimations, std::string{});
  w.write(e, AimTarget, core::Vec3{});
  w.write(e, AimEnabled, false);
  w.write(e, WeaponState, std::int64_t{0});
  w.write(e, Language, std::string());
  w.write(e, Grime, std::string{});
  w.write(e, HumanOverlayAnimation, std::string{});
  w.write(e, RiposteAbility, std::int64_t{-1});
  w.write(e, ReplicaSoul, std::string{});
  w.write(e, ReplicaBuffs, std::string{});
  w.write(e, AnimationProps, std::string{});
  w.write(e, StationAction, std::string{});
  w.write(e, Nameplate, std::string{});
  w.write(e, VisorClosed, false);
  w.write(e, KeyBypass, false);
  w.write(e, Brand, std::int64_t{0});
  w.write(e, PhysicalProp, std::string{});
  w.write(e, PoseAnimation, std::string{});
  w.write(e, LeadRope, std::string{});
  w.write(e, LookTarget, core::Vec3{});
  w.write(e, LookEnabled, false);
  w.write(e, LookValid, false);
  w.write(e, LookTuning, core::Vec3{});
  w.write(e, MotionValid, false);
  w.write(e, MotionMask, std::int64_t{0});
  for (core::FieldId f = MotionParams0; f <= MotionParams5; ++f)
    w.write(e, f, core::Vec3{});
  return e;
}
}  // namespace kcd2s::local
