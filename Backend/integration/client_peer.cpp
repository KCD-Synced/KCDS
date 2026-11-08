#include "kcd2s/integration/client_peer.hpp"

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/control.hpp"
#include "kcd2s/integration/mount_order.hpp"
#include "kcd2s/integration/protocol.hpp"
#include "kcd2s/integration/server_name.hpp"
#include "kcd2s/replication/field_codec.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::integration {

std::optional<std::uint8_t> ClientPeer::combat_epoch(core::EntityId entity) const {
  if (!world_config_ || !world_ready_) {
    return std::nullopt;
  }
  const auto* live = mirror_.get(entity);
  if (!live) {
    return std::nullopt;
  }
  const auto adopted = combat_epochs_.find(entity);
  return adopted == combat_epochs_.end() ? live->epoch : adopted->second;
}

bool ClientPeer::current_combat_context(const MeleeEvent& event) const {
  if (!world_config_ || !world_ready_ || event.revision != world_config_->revision) {
    return false;
  }
  const auto source_epoch = combat_epoch(event.source);
  const auto target_epoch = combat_epoch(event.target);
  const auto grant = owned_.find(event.authority_is_target ? event.source : event.target);
  if (!source_epoch || !target_epoch) {
    return false;
  }
  // A watcher's copy of a contact, or of the victim's reaction, between two other players.
  if (((event.contact && event.attack_generation) || event.reaction) &&
      !owned_.count(event.source) && !owned_.count(event.target)) {
    return *source_epoch == event.source_epoch && *target_epoch == event.target_epoch;
  }
  if (grant == owned_.end()) {
    return false;
  }
  return *source_epoch == event.source_epoch && *target_epoch == event.target_epoch &&
         grant->second.epoch ==
             (event.authority_is_target ? event.source_epoch : event.target_epoch);
}

void ClientPeer::tick(double now) {
  body_state_clock_ = now;
  player_input_clock_ = now;
  for (auto& msg : conn_.receive(now)) {
    if (msg.channel == kScriptChannel) {
      if (world_config_) {
        auto message = script_receiver_.receive(msg.bytes, world_config_->revision);
        if (message && script_inbox_.size() < 128)
          script_inbox_.push_back(std::move(*message));
      }
      continue;
    }
    if (msg.channel == kChatChannel) {
      ChatMessage e;
      std::uint32_t revision = 0;
      if (world_config_ && world_ready_ && decode_chat_message(msg.bytes, revision, e) &&
          revision == world_config_->revision && chat_inbox_.size() < 256)
        chat_inbox_.push_back(std::move(e));
      continue;
    }
    if (msg.channel == static_cast<std::uint8_t>(wire::Channel::Snapshot)) {
      mirror_.apply(msg.bytes);  // malformed snapshots are ignored (returns false)
    } else if (msg.channel == static_cast<std::uint8_t>(wire::Channel::Control)) {
      if (control_kind(msg.bytes) == 3) {
        WorldConfig config;
        if (decode_world_config(msg.bytes, config)) {
          if (!world_config_ || config.revision > world_config_->revision) {
            world_config_ = std::move(config);
            world_ready_ = false;
            body_publications_.clear();
            player_input_publications_.clear();
            gameplay_started_ = false;
            melee_inbox_.clear();
            observer_contacts_.clear();
            melee_received_.clear();
            combat_epochs_.clear();
            forced_transforms_.clear();  // a world-config change resets queued teleports too
            player_engine_commands_.reset_world(world_config_->revision);
            progression_sent_.reset();    // the new world hears the progression again
            progression_orders_.clear();  // and orders meant for the world it left are dropped
            buffs_sent_.reset();
            buff_orders_.clear();
            custom_buffs_.clear();
            animation_orders_.clear();
            pillory_orders_.clear();
            prop_orders_.clear();
            vfx_orders_.clear();
            light_orders_.clear();
            cart_orders_.clear();
            world_rays_.clear();  // rays and camera orders meant for the world it left as well
            camera_orders_.clear();
            siege_orders_.clear();
            siege_shots_.clear();
            dialogue_orders_.clear();
            vendor_orders_.clear();
            ghost_orders_.clear();
            placer_orders_.clear();
            gizmo_orders_.clear();
            siege_ladder_orders_.clear();
            stone_pile_orders_.clear();
            inventory_show_.reset();
            inventory_consumes_.clear();
            chat_inbox_.clear();
            script_inbox_.clear();
            script_receiver_.reset();
          }
        }
      } else if (control_kind(msg.bytes) == kReviveTag) {
        core::EntityId player = 0;
        if (decode_revive(msg.bytes, player) && world_config_ && !player_engine_commands_closed_)
          player_engine_commands_.push_revive(world_config_->revision, player);
      } else if (control_kind(msg.bytes) == kAppearanceTag) {
        AppearanceRequest request;
        if (decode_appearance(msg.bytes, request) && appearance_requests_.size() < 64)
          appearance_requests_.push_back(std::move(request));
      } else if (control_kind(msg.bytes) == kSetStatTag) {
        StatRequest request;
        if (decode_set_stat(msg.bytes, request) && world_config_ && !player_engine_commands_closed_)
          player_engine_commands_.push_stat(world_config_->revision, request);
      } else if (control_kind(msg.bytes) == kAddStatTag) {
        // Player.addStat: in the same ordered stream as setStat and revive.
        StatRequest request;
        if (decode_add_stat(msg.bytes, request) && world_config_ && !player_engine_commands_closed_)
          player_engine_commands_.push_stat(world_config_->revision, request);
      } else if (control_kind(msg.bytes) == kProgressionOrderTag) {
        ProgressionOrder order;
        if (decode_progression_order(msg.bytes, order) && world_ready_ &&
            progression_orders_.size() < 256)
          progression_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kBuffOrderTag) {
        BuffOrder order;
        if (decode_buff_order(msg.bytes, order) && world_ready_ && buff_orders_.size() < 256)
          buff_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kCustomBuffTag) {
        CustomBuff buff;
        if (decode_custom_buff(msg.bytes, buff) && world_ready_ &&
            custom_buffs_.size() < kMaxCustomBuffOrders)
          custom_buffs_.push_back(std::move(buff));
      } else if (control_kind(msg.bytes) == kItemNamesTag) {
        ItemNames names;
        if (decode_item_names(msg.bytes, names) && item_names_.size() < 64)
          item_names_.push_back(std::move(names));
      } else if (control_kind(msg.bytes) == kInventoryShowTag) {
        InventoryShow show;
        if (decode_inventory_show(msg.bytes, show) && world_ready_) {
          if (!show.quickslots && inventory_show_ && inventory_show_->quickslots)
            show.quickslots = std::move(inventory_show_->quickslots);
          inventory_show_ = std::move(show);
        }
      } else if (control_kind(msg.bytes) == kItemOverridesTag) {
        ItemOverrides overrides;
        if (decode_item_overrides(msg.bytes, overrides)) {
          item_overrides_ = std::move(overrides);
        }
      } else if (control_kind(msg.bytes) == kItemLimitNoticeTag) {
        ItemLimitNotice notice;
        if (decode_item_limit_notice(msg.bytes, notice) &&
            item_limit_notices_.size() < kMaxQueuedItemLimitNotices) {
          item_limit_notices_.push_back(std::move(notice));
        }
      } else if (control_kind(msg.bytes) == kItemTypesTag) {
        ItemTypes types;
        if (decode_item_types(msg.bytes, types) && item_types_.size() < 64)
          item_types_.push_back(std::move(types));
      } else if (control_kind(msg.bytes) == kStashShowTag) {
        StashShow show;
        if (decode_stash_show(msg.bytes, show) && world_ready_ && stash_shows_.size() < 64)
          stash_shows_.push_back(std::move(show));
      } else if (control_kind(msg.bytes) == kCraftVerdictTag) {
        CraftVerdict verdict;
        if (decode_craft_verdict(msg.bytes, verdict) && world_ready_ && craft_verdicts_.size() < 64)
          craft_verdicts_.push_back(verdict);
      } else if (control_kind(msg.bytes) == kDiceStateTag) {
        DiceState state;
        if (decode_dice_state(msg.bytes, state) && world_ready_ && dice_states_.size() < 256)
          dice_states_.push_back(std::move(state));
      } else if (control_kind(msg.bytes) == kDiceTablesTag) {
        DiceTables tables;
        if (decode_dice_tables(msg.bytes, tables) && world_ready_)
          dice_tables_ = tables;
      } else if (control_kind(msg.bytes) == kDiceWatchTag) {
        DiceWatch watch;
        if (decode_dice_watch(msg.bytes, watch) && world_ready_ && dice_watches_.size() < 256)
          dice_watches_.push_back(std::move(watch));
      } else if (control_kind(msg.bytes) == kGatherVerdictTag) {
        GatherVerdict verdict;
        if (decode_gather_verdict(msg.bytes, verdict) && world_ready_ &&
            gather_verdicts_.size() < 64)
          gather_verdicts_.push_back(verdict);
      } else if (control_kind(msg.bytes) == kGatherPlantsTag) {
        GatherPlants plants;
        if (decode_gather_plants(msg.bytes, plants) && world_ready_ && gather_plants_.size() < 256)
          gather_plants_.push_back(std::move(plants));
      } else if (control_kind(msg.bytes) == kPotLooksTag) {
        PotLooks looks;
        if (decode_pot_looks(msg.bytes, looks) && world_ready_ && pot_looks_.size() < 64)
          pot_looks_.push_back(std::move(looks));
      } else if (control_kind(msg.bytes) == kInventoryConsumeTag) {
        InventoryConsume consume;
        if (decode_inventory_consume(msg.bytes, consume) && world_ready_ &&
            inventory_consumes_.size() < 64)
          inventory_consumes_.push_back(consume);
      } else if (control_kind(msg.bytes) == kMovementModeTag) {
        MovementMode mode;
        if (decode_movement_mode(msg.bytes, mode))
          movement_mode_ = mode;
      } else if (control_kind(msg.bytes) == kBodyVisibleTag) {
        bool visible = true;
        if (decode_body_visible(msg.bytes, visible))
          body_visible_ = visible;
      } else if (control_kind(msg.bytes) == kDisguiseTag) {
        std::string model;
        if (decode_disguise(msg.bytes, model))
          disguise_ = std::move(model);
      } else if (control_kind(msg.bytes) == kWornTag) {
        std::vector<ItemClassId> items;
        if (decode_worn(msg.bytes, items)) {
          worn_ = std::move(items);
          ++worn_revision_;
        }
      } else if (control_kind(msg.bytes) == kCombatCancelTag) {
        std::uint32_t opponent = 0;
        if (decode_combat_cancel(msg.bytes, opponent) && combat_cancels_.size() < 64)
          combat_cancels_.push_back(opponent);
      } else if (control_kind(msg.bytes) == kWorldPickupVerdictTag) {
        std::string key;
        bool granted = false;
        if (decode_world_pickup_verdict(msg.bytes, key, granted) &&
            world_pickup_verdicts_.size() < 64)
          world_pickup_verdicts_.emplace_back(std::move(key), granted);
      } else if (control_kind(msg.bytes) == kPickpocketPocketsTag) {
        PickpocketPockets pockets;
        if (decode_pickpocket_pockets(msg.bytes, pockets) && pickpocket_pockets_.size() < 8)
          pickpocket_pockets_.push_back(std::move(pockets));
      } else if (control_kind(msg.bytes) == kWorldTimeTag) {
        WorldTime time;
        if (decode_world_time(msg.bytes, time) && world_ready_)
          world_time_ = time;
      } else if (control_kind(msg.bytes) == kGateTag) {
        GateState gate;
        if (decode_gate(msg.bytes, gate) && world_ready_ && gate_states_.size() < 64)
          gate_states_.push_back(gate);
      } else if (control_kind(msg.bytes) == kWorldWeatherTag) {
        WorldWeather weather;
        if (decode_world_weather(msg.bytes, weather) && world_ready_)
          world_weather_ = std::move(weather);
      } else if (control_kind(msg.bytes) == kMarkerShowTag) {
        MarkerOrder order;
        order.show = true;
        if (decode_marker_show(msg.bytes, order.marker) && marker_orders_.size() < kMaxMarkerOrders)
          marker_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kMarkerHideTag) {
        MarkerOrder order;
        if (decode_marker_hide(msg.bytes, order.marker.marker) &&
            marker_orders_.size() < kMaxMarkerOrders)
          marker_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kBlipShowTag) {
        BlipOrder order;
        order.show = true;
        if (decode_blip_show(msg.bytes, order.blip) && blip_orders_.size() < kMaxMarkerOrders)
          blip_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kBlipHideTag) {
        BlipOrder order;
        if (decode_blip_hide(msg.bytes, order.blip.blip) && blip_orders_.size() < kMaxMarkerOrders)
          blip_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kQuestShowTag) {
        QuestOrder order;
        if (decode_quest_show(msg.bytes, order.show) && quest_orders_.size() < kMaxQuestOrders)
          quest_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kQuestHideTag) {
        QuestOrder order;
        order.kind = QuestOrder::Kind::Hide;
        if (decode_quest_hide(msg.bytes, order.show.quest) &&
            quest_orders_.size() < kMaxQuestOrders)
          quest_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kQuestTrackTag) {
        QuestOrder order;
        order.kind = QuestOrder::Kind::Track;
        QuestTracking track;
        if (decode_quest_track(msg.bytes, track) && quest_orders_.size() < kMaxQuestOrders) {
          order.show.quest = track.quest;
          order.tracked = track.tracked;
          quest_orders_.push_back(std::move(order));
        }
      } else if (control_kind(msg.bytes) == kGraveTag) {
        Grave grave;
        if (decode_grave(msg.bytes, grave) &&
            (grave.kind == GraveKind::Set || grave.kind == GraveKind::Digging) && world_ready_ &&
            grave_sets_.size() < kMaxGraves)
          grave_sets_.push_back(grave);
      } else if (control_kind(msg.bytes) == kSmartObjectTag) {
        SmartObjectOrder order;
        if (decode_smart_object(msg.bytes, order) && world_ready_ && smart_objects_.size() < 256)
          smart_objects_.push_back(order);
      } else if (control_kind(msg.bytes) == kDoorSpawnTag) {
        DoorSpawn door;
        if (decode_door_spawn(msg.bytes, door) && world_ready_ && door_spawns_.size() < 256)
          door_spawns_.push_back(std::move(door));
      } else if (control_kind(msg.bytes) == kDoorSetTag) {
        DoorState state;
        if (decode_door_set(msg.bytes, state) && world_ready_ && door_sets_.size() < 256)
          door_sets_.push_back(state);
      } else if (control_kind(msg.bytes) == kDoorNearestTag) {
        std::uint32_t request = 0;
        if (decode_door_nearest(msg.bytes, request) && world_ready_ &&
            door_nearest_requests_.size() < 8)
          door_nearest_requests_.push_back(request);
      } else if (control_kind(msg.bytes) == kPropTag) {
        PropOrder order;
        if (decode_prop(msg.bytes, order) && world_ready_) {
          if (prop_orders_.size() < kMaxPlacedOrders) {
            prop_orders_.push_back(std::move(order));
          } else {
            ++dropped_placed_orders_;
          }
        }
      } else if (control_kind(msg.bytes) == kCartTag) {
        CartOrder order;
        if (decode_cart(msg.bytes, order) && world_ready_ && cart_orders_.size() < kMaxCartOrders)
          cart_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kVfxTag) {
        VfxOrder order;
        if (decode_vfx(msg.bytes, order) && world_ready_) {
          if (vfx_orders_.size() < kMaxPlacedOrders) {
            vfx_orders_.push_back(std::move(order));
          } else {
            ++dropped_placed_orders_;
          }
        }
      } else if (control_kind(msg.bytes) == kRecipeTableTag) {
        RecipeTable table;
        if (decode_recipe_table(msg.bytes, table) && world_ready_ &&
            recipe_tables_.size() < kMaxRecipeTables)
          recipe_tables_.push_back(std::move(table));
      } else if (control_kind(msg.bytes) == kRecipePictureTag) {
        RecipePicturePart part;
        RecipePicture picture;
        if (decode_recipe_picture_part(msg.bytes, part) && world_ready_ &&
            recipe_picture_parts_.add(std::move(part), picture) &&
            recipe_pictures_.size() < kMaxRecipePictures)
          recipe_pictures_.push_back(std::move(picture));
      } else if (control_kind(msg.bytes) == kLightTag) {
        LightOrder order;
        if (decode_light(msg.bytes, order) && world_ready_ &&
            light_orders_.size() < kMaxLightOrders)
          light_orders_.push_back(order);
      } else if (control_kind(msg.bytes) == kLevelEditApplyTag) {
        LevelEditChange change;
        if (decode_level_edit(msg.bytes, change.edit) && world_ready_ &&
            level_edit_changes_.size() < kMaxLevelEditChanges)
          level_edit_changes_.push_back(std::move(change));
      } else if (control_kind(msg.bytes) == kLevelEditRemoveTag) {
        LevelEditChange change;
        change.removed = true;
        if (decode_level_edit_remove(msg.bytes, change.edit.id) && world_ready_ &&
            level_edit_changes_.size() < kMaxLevelEditChanges)
          level_edit_changes_.push_back(std::move(change));
      } else if (control_kind(msg.bytes) == kGhostTag) {
        GhostOrder order;
        if (decode_ghost(msg.bytes, order) && world_ready_)
          queue_ghost_order(ghost_orders_, std::move(order), kMaxPreviewOrders);
      } else if (control_kind(msg.bytes) == kPlacerTag) {
        PlacerOrder order;
        if (decode_placer(msg.bytes, order) && world_ready_ &&
            placer_orders_.size() < kMaxPreviewOrders)
          placer_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kGizmoTag) {
        GizmoOrder order;
        if (decode_gizmo(msg.bytes, order) && world_ready_ &&
            gizmo_orders_.size() < kMaxGizmoOrders)
          gizmo_orders_.push_back(std::move(order));
      } else if (control_kind(msg.bytes) == kGrimeOrderTag) {
        GrimeOrder order;
        if (decode_grime_order(msg.bytes, order) && grime_orders_.size() < 16)
          grime_orders_.push_back(order);
      } else if (control_kind(msg.bytes) == kBodyCarryTag) {
        BodyCarry carry;
        if (decode_body_carry(msg.bytes, carry) && body_carries_.size() < 64)
          body_carries_.push_back(carry);
      } else if (control_kind(msg.bytes) == kAnimationOrderTag) {
        AnimationOrder order;
        if (decode_animation_order(msg.bytes, order) && world_ready_) {
          if (animation_orders_.size() == kMaxAnimationOrders) {
            animation_orders_.erase(animation_orders_.begin());
          }
          animation_orders_.push_back(std::move(order));
        }
      } else if (control_kind(msg.bytes) == kFreecamTag) {
        bool allowed = true;
        if (decode_freecam(msg.bytes, allowed))
          freecam_ = allowed;
      } else if (control_kind(msg.bytes) == kNeedsTag) {
        bool enabled = true;
        if (decode_needs(msg.bytes, enabled))
          needs_ = enabled;
      } else if (control_kind(msg.bytes) == kPickpocketRuleTag) {
        PickpocketRule rule;
        if (decode_pickpocket_rule(msg.bytes, rule) && pickpocket_rules_.size() < 64)
          pickpocket_rules_.push_back(rule);
      } else if (control_kind(msg.bytes) == kItemNoticeTag) {
        ItemNotice notice;
        if (decode_item_notice(msg.bytes, notice) && item_notices_.size() < 64)
          item_notices_.push_back(notice);
      } else if (control_kind(msg.bytes) == kShotRelayTag) {
        std::uint32_t shooter = 0;
        Shot shot;
        if (decode_shot_relay(msg.bytes, shooter, shot) && shot_relays_.size() < 64)
          shot_relays_.emplace_back(shooter, shot);
      } else if (control_kind(msg.bytes) == kWorldItemsRemovedTag) {
        std::vector<RemovedItemArea> areas;
        if (decode_world_items_removed(msg.bytes, areas)) {
          world_items_removed_ = std::move(areas);
        }
      } else if (control_kind(msg.bytes) == kWorldItemsEnabledTag) {
        bool enabled = true;
        if (decode_world_items_enabled(msg.bytes, enabled)) {
          world_items_enabled_ = enabled;
        }
      } else if (control_kind(msg.bytes) == kWorldTakenTag) {
        WorldTaken taken;
        if (decode_world_taken(msg.bytes, taken) && world_taken_.size() < 256)
          world_taken_.push_back(std::move(taken));
      } else if (control_kind(msg.bytes) == kCarryOrderTag) {
        CarryOrder order;
        if (decode_carry_order(msg.bytes, order) && carry_orders_.size() < 16)
          carry_orders_.push_back(order);
      } else if (control_kind(msg.bytes) == kHitVerdictTag) {
        HitVerdict verdict;
        if (decode_hit_verdict(msg.bytes, verdict) && hit_verdicts_.size() < 64)
          hit_verdicts_.push_back(verdict);
      } else if (control_kind(msg.bytes) == kHitRulesTag) {
        bool enabled = false;
        if (decode_hit_rules(msg.bytes, enabled))
          hit_rules_ = enabled;
      } else if (control_kind(msg.bytes) == kKnockoutVisionTag) {
        std::uint8_t percent = 0;
        if (decode_knockout_vision(msg.bytes, percent))
          knockout_vision_ = percent;
      } else if (control_kind(msg.bytes) == kCombatMarkerTag) {
        bool shown = true;
        if (decode_combat_marker(msg.bytes, shown))
          combat_marker_ = shown;
      } else if (control_kind(msg.bytes) == kSessionEndTag) {
        SessionEnd end;
        if (decode_session_end(msg.bytes, end)) {
          session_end_ = std::move(end);
          player_engine_commands_closed_ = true;
          player_engine_commands_.invalidate_body();
        }
      } else if (control_kind(msg.bytes) == kRestrictedAreaMarkerTag) {
        bool shown = true;
        if (decode_restricted_area_marker(msg.bytes, shown))
          restricted_area_marker_ = shown;
      } else if (control_kind(msg.bytes) == kThirdPersonTag) {
        std::uint8_t mode = kThirdPersonAllowed;
        if (decode_third_person(msg.bytes, mode)) {
          third_person_ = mode;
        }
      } else if (control_kind(msg.bytes) == kWorldRaycastTag) {
        WorldRayRequest ray;
        if (decode_world_raycast(msg.bytes, ray) && world_ready_ && world_rays_.size() < 32)
          world_rays_.push_back(ray);
      } else if (control_kind(msg.bytes) == kCameraShotTag) {
        CameraShot shot;
        if (decode_camera_shot(msg.bytes, shot) && world_ready_ && camera_orders_.size() < 16)
          camera_orders_.emplace_back(shot);
      } else if (control_kind(msg.bytes) == kCameraClearTag) {
        if (decode_camera_clear(msg.bytes) && camera_orders_.size() < 16)
          camera_orders_.emplace_back(std::nullopt);
      } else if (control_kind(msg.bytes) == kDialoguePageTag) {
        DialoguePage page;
        if (decode_dialogue_page(msg.bytes, page) && world_ready_ &&
            dialogue_orders_.size() < kMaxDialogueOrdersWaiting) {
          dialogue_orders_.emplace_back(std::move(page));
        }
      } else if (control_kind(msg.bytes) == kDialogueCloseTag) {
        DialogueClose close;
        if (decode_dialogue_close(msg.bytes, close) &&
            dialogue_orders_.size() < kMaxDialogueOrdersWaiting) {
          dialogue_orders_.emplace_back(close);
        }
      } else if (control_kind(msg.bytes) == kVendorShowTag) {
        VendorShow show;
        if (decode_vendor_show(msg.bytes, show) && world_ready_ &&
            vendor_orders_.size() < kMaxVendorOrdersWaiting) {
          vendor_orders_.emplace_back(std::move(show));
        }
      } else if (control_kind(msg.bytes) == kVendorCloseTag) {
        VendorClose close;
        if (decode_vendor_close(msg.bytes, close) &&
            vendor_orders_.size() < kMaxVendorOrdersWaiting) {
          vendor_orders_.emplace_back(close);
        }
      } else if (control_kind(msg.bytes) == kVendorHaggleReplyTag) {
        VendorHaggleReply reply;
        if (decode_vendor_haggle_reply(msg.bytes, reply) && world_ready_ &&
            vendor_orders_.size() < kMaxVendorOrdersWaiting) {
          vendor_orders_.emplace_back(reply);
        }
      } else if (control_kind(msg.bytes) == kDismountOrderTag) {
        PullDownOrder pull_down;
        if (decode_pull_down_order(msg.bytes, pull_down)) {
          pull_down_orders_.push_back(pull_down);
        } else if (decode_rear_order(msg.bytes)) {
          rear_ordered_ = true;
        } else if (decode_dismount_order(msg.bytes)) {
          dismount_ordered_ = true;
        }
      } else if (control_kind(msg.bytes) == kMountOrderTag) {
        MountOrder order;
        if (decode_mount_order(msg.bytes, order) && mount_orders_.size() < 8) {
          mount_orders_.push_back(order);
        }
      } else if (control_kind(msg.bytes) == kCuffsTag) {
        bool cuffed = false;
        if (decode_cuffs(msg.bytes, cuffed)) {
          cuffs_ = cuffed;
        }
      } else if (control_kind(msg.bytes) == kPlayerAttachTag) {
        PlayerAttach attach;
        if (decode_player_attach(msg.bytes, attach)) {
          player_attaches_.push_back(attach);
        }
      } else if (control_kind(msg.bytes) == kFrozenTag) {
        bool frozen = false;
        if (decode_frozen(msg.bytes, frozen)) {
          frozen_ = frozen;
        }
      } else if (control_kind(msg.bytes) == kNoClipAllowedTag) {
        bool allowed = true;
        if (decode_noclip_allowed(msg.bytes, allowed)) {
          noclip_allowed_ = allowed;
        }
      } else if (control_kind(msg.bytes) == kVoiceMuffleTag) {
        std::uint32_t talker = 0;
        std::uint8_t amount = 0;
        if (decode_voice_muffle(msg.bytes, talker, amount) && voice_muffles_.size() < 1024) {
          voice_muffles_.emplace_back(talker, amount);
        }
      } else if (control_kind(msg.bytes) == kVoiceStateTag) {
        VoiceState state;
        if (decode_voice_state(msg.bytes, state)) {
          voice_state_ = state;
        }
      } else if (control_kind(msg.bytes) == kServerNameTag) {
        std::string name;
        if (decode_server_name(msg.bytes, name)) {
          server_name_ = std::move(name);
        }
      } else if (control_kind(msg.bytes) == kWhistleTag) {
        std::uint32_t whistler = 0;
        if (decode_whistle(msg.bytes, whistler) && whistles_.size() < 64) {
          whistles_.push_back(whistler);
        }
      } else if (control_kind(msg.bytes) == kInteractionRulesTag) {
        std::uint16_t allowed = 0;
        if (decode_interaction_rules(msg.bytes, allowed)) {
          interaction_rules_ = allowed;
        }
      } else if (control_kind(msg.bytes) == kInteractionsTag) {
        std::vector<Interaction> list;
        if (decode_interactions(msg.bytes, list)) {
          interactions_ = std::move(list);
        }
      } else if (control_kind(msg.bytes) == kSiegeEngineTag) {
        SiegeEngineOrder order;
        if (decode_siege_engine(msg.bytes, order) && world_ready_ &&
            siege_orders_.size() < kMaxSiegeOrders) {
          siege_orders_.push_back(order);
        }
      } else if (control_kind(msg.bytes) == kSiegeShotTag) {
        SiegeShot shot;
        if (decode_siege_shot(msg.bytes, shot) && world_ready_ &&
            siege_shots_.size() < kMaxSiegeOrders) {
          siege_shots_.push_back(shot);
        }
      } else if (control_kind(msg.bytes) == kSiegeLadderTag) {
        SiegeLadderOrder order;
        if (decode_siege_ladder(msg.bytes, order) && world_ready_ &&
            siege_ladder_orders_.size() < kMaxSiegeOrders) {
          siege_ladder_orders_.push_back(order);
        }
      } else if (control_kind(msg.bytes) == kStonePileTag) {
        StonePileOrder order;
        if (decode_stone_pile(msg.bytes, order) && world_ready_ &&
            stone_pile_orders_.size() < kMaxSiegeOrders) {
          stone_pile_orders_.push_back(order);
        }
      } else if (control_kind(msg.bytes) == kNpcBlowTag) {
        NpcBlow blow;
        if (decode_npc_blow(msg.bytes, blow) && world_ready_ && world_config_ &&
            blow.revision == world_config_->revision && npc_blows_.size() < kMaxNpcBlowsWaiting) {
          npc_blows_.push_back(blow);
        }
      } else if (control_kind(msg.bytes) == kPilloryTag) {
        PilloryOrder order;
        if (decode_pillory(msg.bytes, order) && world_ready_) {
          if (pillory_orders_.size() == kMaxPilloryOrders) {
            pillory_orders_.erase(pillory_orders_.begin());
          }
          pillory_orders_.push_back(std::move(order));
        }
      } else if (control_kind(msg.bytes) == kCtrlSetOwner) {
        core::EntityId e = 0;
        core::TypeId type = 0;
        std::uint8_t epoch = 0;
        if (decode_set_owner(msg.bytes, e, type, epoch)) {
          owned_[e] = Owned{type, epoch};
          combat_epochs_[e] = epoch;
        }
      } else if (control_kind(msg.bytes) == kCtrlForceState) {
        core::EntityId e = 0;
        std::uint8_t epoch = 0;
        if (decode_force_state(msg.bytes, e, epoch)) {
          combat_epochs_[e] = epoch;
          auto it = owned_.find(e);
          if (it != owned_.end())
            it->second.epoch = epoch;  // adopt the forced epoch
        }
      } else if (control_kind(msg.bytes) == kCtrlForceTransform) {
        core::EntityId e = 0;
        std::uint8_t epoch = 0;
        core::Vec3 pos{};
        if (decode_force_transform(msg.bytes, e, epoch, pos)) {
          // Authorisation fence, applied before the teleport is ever queued:
          //  * the entity must currently be known to this client, either as a replicated mirror
          //    entry (an observer) or an owned entity (the controller, which is not sent its own
          //    snapshot). Both maps are keyed by the EntityId, which is never reused, so a command
          //    replayed after a reconnect names the old id, is absent from both, and is dropped --
          //    a stale reconnect command can never mutate the new entity;
          //  * the epoch must not predate the one already adopted, so a teleport cannot be applied
          //    out of order behind a newer forced state. The epoch is a wrapping uint8, so the
          //    "not older" test is a signed difference (like core::seq_greater_equal), not a raw
          //    >=: that keeps ordering correct as the counter rolls over 255 -> 0.
          // decode_force_transform has already rejected non-finite coordinates. The accepted
          // command is queued for the engine tick (take_forced_transforms); it is never written to
          // a native CEntity here, on the network thread.
          const bool known_entity = mirror_.get(e) != nullptr || owned_.count(e) != 0;
          const auto known = combat_epochs_.find(e);
          const bool not_older =
              known == combat_epochs_.end() || static_cast<std::int8_t>(epoch - known->second) >= 0;
          if (known_entity && not_older) {
            combat_epochs_[e] = epoch;
            // The owned input epoch is deliberately NOT adopted here. Owner input captured on the
            // engine thread before the teleport is applied still holds the old pose; sending it
            // under the new epoch would let the server accept that pose and undo the teleport.
            // take_forced_transforms() adopts the epoch on the engine thread, which applies the
            // transform before it captures and sends the next input.
            // Bound the queue so a misbehaving or hostile server cannot grow it without limit
            // between engine drains; the engine applies latest-wins, so dropping the oldest pending
            // teleport is safe.
            if (forced_transforms_.size() >= kMaxForcedTransforms_)
              forced_transforms_.erase(forced_transforms_.begin());
            ForcedTransform command;
            command.entity = e;
            command.position = pos;
            command.epoch = epoch;
            command.revision = world_config_ ? world_config_->revision : 0;
            forced_transforms_.push_back(command);
          }
        }
      } else if (control_kind(msg.bytes) == kCtrlForceRotation) {
        core::EntityId e = 0;
        std::uint8_t epoch = 0;
        core::Quat rotation;
        // Same authorisation fence and engine-thread hand-off as FORCE_XFORM above.
        if (decode_force_rotation(msg.bytes, e, epoch, rotation)) {
          const bool known_entity = mirror_.get(e) != nullptr || owned_.count(e) != 0;
          const auto known = combat_epochs_.find(e);
          const bool not_older =
              known == combat_epochs_.end() || static_cast<std::int8_t>(epoch - known->second) >= 0;
          if (known_entity && not_older) {
            combat_epochs_[e] = epoch;
            if (forced_transforms_.size() >= kMaxForcedTransforms_)
              forced_transforms_.erase(forced_transforms_.begin());
            ForcedTransform command;
            command.entity = e;
            command.has_position = false;
            command.has_rotation = true;
            command.rotation = rotation;
            command.epoch = epoch;
            command.revision = world_config_ ? world_config_->revision : 0;
            forced_transforms_.push_back(command);
          }
        }
      }
    }
    if (msg.channel == kCombatChannel) {
      MeleeEvent e;
      if (!decode_melee(msg.bytes, e) || !current_combat_context(e))
        continue;
      // Sequences count per sending player: a reaction is sent by its target (the victim).
      const auto author = e.authority_is_target ? e.target : e.source;
      const auto author_epoch = e.authority_is_target ? e.target_epoch : e.source_epoch;
      auto& seen = melee_received_[author];
      if (seen.first == author_epoch && e.sequence <= seen.second)
        continue;
      seen = {author_epoch, e.sequence};
      const bool observer = ((e.contact && e.attack_generation) || e.reaction) &&
                            !owned_.count(e.source) && !owned_.count(e.target);
      if (observer) {
        if (observer_contacts_.size() < kMaxObserverContacts) {
          observer_contacts_.push_back(e);
        }
      } else if (!e.attack_generation_marker || melee_inbox_.size() < kMaxObserverContacts) {
        melee_inbox_.push_back(e);
      }
    }
  }
  // Report the newest baseline we have applied so the server can shrink future deltas.
  for (auto it = player_input_publications_.begin(); it != player_input_publications_.end();) {
    if (!owned_.count(it->first) || !mirror_.get(it->first)) {
      it = player_input_publications_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = body_publications_.begin(); it != body_publications_.end();) {
    if (!mirror_.get(it->first)) {
      it = body_publications_.erase(it);
    } else {
      ++it;
    }
  }
  // A new applied snapshot is acknowledged immediately, including tick zero after wrap.
  // Repeat an unchanged ACK at a bounded cadence so a lost ACK still recovers without sending
  // one identical replication message on every idle engine/probe tick.
  if (mirror_.has_applied() &&
      (!last_replication_ack_ || *last_replication_ack_ != mirror_.last_applied() ||
       now < replication_ack_at_ || now - replication_ack_at_ >= kReplicationAckRepeatSeconds)) {
    conn_.queue(kChannelReplAck, encode_tick(mirror_.last_applied()));
    last_replication_ack_ = mirror_.last_applied();
    replication_ack_at_ = now;
  }
  conn_.flush(now);
}

bool ClientPeer::send_script(const sdk::Value& value) {
  if (!world_config_)
    return false;
  const auto op = script_operation(value);
  if (op != "catalogRequest" && op != "catalogChunk" && op != "fileRequest" && op != "event" &&
      op != "resourcesReady" && op != "entityAck" && op != "entityTransform" &&
      op != "groundItemRest")
    return false;
  return queue_script(conn_, world_config_->revision, script_sequence_, value);
}

bool ClientPeer::send_horse_state(HorseState state) {
  if (!world_config_ || !world_ready_ || !owns(state.source))
    return false;
  const auto* horse = mirror_.get(state.horse);
  if (!horse || horse->type != core::type_id("NetworkHorse") ||
      horse->fields.size() <= HorseNativeState ||
      std::get<std::int64_t>(horse->fields[HorseSimulator]) != state.source)
    return false;
  state.revision = world_config_->revision;
  state.epoch = std::uint32_t(std::get<std::int64_t>(horse->fields[HorseEpoch]));
  if (!valid_horse(state))
    return false;
  const bool ridden = std::get<std::int64_t>(horse->fields[HorseRider]) != 0;
  if (body_publications_[state.horse].publish(state, body_state_clock_, true, ridden)) {
    conn_.queue(kHorseMotionChannel, encode_horse(state));
  }
  return true;
}
bool ClientPeer::send_horse_mount(core::EntityId player, core::EntityId id, bool mount) {
  if (!world_config_ || !world_ready_ || !owns(player))
    return false;
  const auto* horse = mirror_.get(id);
  if (!horse || horse->type != core::type_id("NetworkHorse") ||
      horse->fields.size() <= HorseNativeState)
    return false;
  conn_.queue(kHorseMountChannel,
              encode_horse_mount({world_config_->revision,
                                  std::uint32_t(std::get<std::int64_t>(horse->fields[HorseEpoch])),
                                  player, id, mount}));
  return true;
}

bool ClientPeer::send_npc_state(NpcState state) {
  if (!world_config_ || !world_ready_ || !owns(state.source)) {
    return false;
  }
  const auto* npc = mirror_.get(state.npc);
  if (!npc || npc->type != core::type_id("NetworkNpc") || npc->fields.size() <= NpcAlive) {
    return false;
  }
  if (std::get<std::int64_t>(npc->fields[NpcSimulator]) != std::int64_t(state.source)) {
    return false;
  }
  state.revision = world_config_->revision;
  state.epoch = std::uint32_t(std::get<std::int64_t>(npc->fields[NpcEpoch]));
  if (!valid_npc(state)) {
    return false;
  }
  if (body_publications_[state.npc].publish(state, body_state_clock_)) {
    conn_.queue(kNpcMotionChannel, encode_npc_state(state));
  }
  return true;
}

bool ClientPeer::send_npc_look(core::EntityId npc, const std::string& look) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(npc);
  if (!entity || entity->type != core::type_id("NetworkNpc") ||
      entity->fields.size() <= NpcAppearance) {
    return false;
  }
  NpcLookReport report;
  report.npc = npc;
  report.epoch = std::uint32_t(std::get<std::int64_t>(entity->fields[NpcEpoch]));
  report.revision = world_config_->revision;
  report.look = look;
  auto message = encode_npc_look(report);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_outfit(core::EntityId npc, const std::string& outfit) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(npc);
  if (!entity || entity->type != core::type_id("NetworkNpc") ||
      entity->fields.size() <= NpcOutfit) {
    return false;
  }
  NpcOutfitReport report;
  report.npc = npc;
  report.epoch = std::uint32_t(std::get<std::int64_t>(entity->fields[NpcEpoch]));
  report.revision = world_config_->revision;
  report.outfit = outfit;
  auto message = encode_npc_outfit(report);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_pose(core::EntityId npc, const std::string& pose) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(npc);
  if (!entity || entity->type != core::type_id("NetworkNpc") || entity->fields.size() <= NpcEpoch) {
    return false;
  }
  NpcPoseReport report;
  report.npc = npc;
  report.epoch = std::uint32_t(std::get<std::int64_t>(entity->fields[NpcEpoch]));
  report.revision = world_config_->revision;
  report.pose = pose;
  auto message = encode_npc_pose(report);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_hit(NpcHit hit) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(hit.npc);
  if (!entity || entity->type != core::type_id("NetworkNpc")) {
    return false;
  }
  hit.revision = world_config_->revision;
  auto message = encode_npc_hit(hit);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_blow(NpcBlow blow) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(blow.npc);
  if (!entity || entity->type != core::type_id("NetworkNpc")) {
    return false;
  }
  blow.revision = world_config_->revision;
  auto message = encode_npc_blow(blow);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_engage(core::EntityId npc) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(npc);
  if (!entity || entity->type != core::type_id("NetworkNpc")) {
    return false;
  }
  NpcEngage engage;
  engage.npc = npc;
  engage.revision = world_config_->revision;
  auto message = encode_npc_engage(engage);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_horse_stamina(core::EntityId horse, std::uint32_t epoch, float stamina) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(horse);
  if (!entity || entity->type != core::type_id("NetworkHorse")) {
    return false;
  }
  HorseStaminaReport report;
  report.horse = horse;
  report.epoch = epoch;
  report.stamina = stamina;
  auto message = encode_horse_stamina(report);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_horse_health(core::EntityId horse, std::uint32_t epoch, float health,
                                   std::uint8_t state) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(horse);
  if (!entity || entity->type != core::type_id("NetworkHorse")) {
    return false;
  }
  HorseHealthReport report;
  report.horse = horse;
  report.epoch = epoch;
  report.health = health;
  report.state = state;
  auto message = encode_horse_health(report);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_intent_done(core::EntityId npc, std::uint32_t epoch,
                                      std::uint32_t sequence, std::uint8_t status) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  // An NPC's order, or a horse's (its orders walk the same way).
  const auto* entity = mirror_.get(npc);
  if (!entity || (entity->type != core::type_id("NetworkNpc") &&
                  entity->type != core::type_id("NetworkHorse"))) {
    return false;
  }
  NpcIntentDone done;
  done.npc = npc;
  done.epoch = epoch;
  done.sequence = sequence;
  done.status = status;
  auto message = encode_npc_intent_done(done);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_use(core::EntityId npc) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(npc);
  if (!entity || entity->type != core::type_id("NetworkNpc")) {
    return false;
  }
  auto message = encode_npc_use(npc);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_npc_animation_end(core::EntityId npc, std::uint32_t epoch,
                                        std::uint32_t sequence, std::uint8_t reason) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  const auto* entity = mirror_.get(npc);
  if (!entity || entity->type != core::type_id("NetworkNpc")) {
    return false;
  }
  NpcAnimationEnd end;
  end.npc = npc;
  end.epoch = epoch;
  end.sequence = sequence;
  end.reason = reason;
  auto message = encode_npc_animation_end(end);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_voice_key(const std::array<std::uint8_t, 32>& key) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  conn_.queue(0, encode_voice_key(key));
  return true;
}

bool ClientPeer::send_voice_tier(std::uint8_t tier) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  conn_.queue(0, encode_voice_tier(tier));
  return true;
}

bool ClientPeer::send_whistle_report() {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  conn_.queue(0, encode_whistle_report());
  return true;
}

bool ClientPeer::send_travel_end(float x, float y, float z) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  conn_.queue(0, encode_travel_end(TravelEnd{x, y, z}));
  return true;
}

bool ClientPeer::send_interaction_fired(std::uint32_t id) {
  if (!world_config_ || !world_ready_ || id == 0) {
    return false;
  }
  conn_.queue(0, encode_interaction_fired(id));
  return true;
}

bool ClientPeer::send_dialogue_choice(const DialogueChoice& choice) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  auto message = encode_dialogue_choice(choice);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_dialogue_cancel(const DialogueCancel& cancel) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  auto message = encode_dialogue_cancel(cancel);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_vendor_trade(const VendorTrade& trade) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  auto message = encode_vendor_trade(trade);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_vendor_haggle(const VendorHaggle& haggle) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  auto message = encode_vendor_haggle(haggle);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_vendor_closed(const VendorClosed& closed) {
  if (!world_config_) {
    return false;
  }
  auto message = encode_vendor_closed(closed);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_placer_report(const PlacerReport& report) {
  if (!world_config_ || !world_ready_) {
    return false;
  }
  auto message = encode_placer_report(report);
  if (message.empty()) {
    return false;
  }
  conn_.queue(0, std::move(message));
  return true;
}

bool ClientPeer::send_chat(core::EntityId player, const std::string& text) {
  if (!world_config_ || !world_ready_ || !owns(player) || !valid_chat_text(text))
    return false;
  conn_.queue(kChatChannel, encode_chat_submission(world_config_->revision, {player, text}));
  return true;
}

void ClientPeer::send_input(
    core::EntityId e,
    const std::vector<std::pair<core::FieldId, replication::FieldValue>>& updates) {
  if (world_config_ && !world_ready_)
    return;
  // Determine the entity's type + forced-state epoch: from a SET_OWNER grant, else from the mirror
  // (a client can *try* to author input for any entity it can see — the server rejects non-owner
  // and stale-epoch writes).
  core::TypeId type = 0;
  std::uint8_t epoch = 0;
  auto it = owned_.find(e);
  if (it != owned_.end()) {
    type = it->second.type;
    epoch = it->second.epoch;
  } else if (const replication::Mirror::Entity* me = mirror_.get(e)) {
    type = me->type;
    epoch = me->epoch;  // best-effort from the last snapshot
  } else {
    return;  // unknown type -> cannot encode
  }
  const replication::TypeDef* td = schema_.get(type);
  if (!td)
    return;

  // Validate before writing the count or changing the publication cache. An invalid field id
  // used to leave the encoded count larger than the body, creating a malformed partial input.
  if (updates.size() > 65535 ||
      std::any_of(updates.begin(), updates.end(),
                  [&](const auto& update) { return update.first >= td->fields.size(); })) {
    return;
  }
  if (it != owned_.end() && type == core::type_id("NetworkPlayer") &&
      !player_input_publications_[e].publish(
          *td, updates, {e, 0, epoch, world_config_ ? world_config_->revision : 0},
          player_input_clock_)) {
    return;
  }

  core::BitWriter w;
  w.write_varuint(e);
  w.write_bits(epoch, 8);
  w.write_bits(updates.size(), 16);
  for (const auto& [fid, val] : updates) {
    w.write_bits(fid, 16);
    replication::encode_field(w, td->fields[fid], val);
  }
  conn_.queue(static_cast<std::uint8_t>(wire::Channel::Upstream), w.bytes());
}

bool ClientPeer::send_native_effects(core::EntityId source, core::EntityId target,
                                     const NativeMeleeData& cause, const NativeEffects& effects,
                                     bool executor_is_target) {
  if (!world_config_ || !world_ready_ || !valid_committed_melee_data(cause) ||
      !valid_native_effects(effects))
    return false;
  auto grant = owned_.find(target);
  auto other = mirror_.get(source);
  if (grant == owned_.end() || !other || source == target)
    return false;
  MeleeEvent e;
  e.revision = world_config_->revision;
  e.source = source;
  e.target = target;
  e.authority_is_target = true;
  e.executor_is_target = executor_is_target;
  e.source_epoch = combat_epochs_.count(source) ? combat_epochs_.at(source) : other->epoch;
  e.target_epoch = grant->second.epoch;
  e.sequence = next_melee_sequence_++;
  e.effects = true;
  e.data = cause;
  e.effect_data = effects;
  auto bytes = encode_melee(e);
  if (bytes.empty())
    return false;
  conn_.queue(kCombatChannel, bytes);
  return true;
}
bool ClientPeer::send_native_contact(core::EntityId source, core::EntityId target,
                                     const NativeContactData& data,
                                     std::uint64_t attack_generation) {
  if (!world_config_ || !world_ready_ || !valid_contact_data(data))
    return false;
  auto grant = owned_.find(source);
  auto other = mirror_.get(target);
  if (grant == owned_.end() || !other || source == target)
    return false;
  MeleeEvent e;
  e.revision = world_config_->revision;
  e.source = source;
  e.target = target;
  e.source_epoch = grant->second.epoch;
  e.target_epoch = combat_epochs_.count(target) ? combat_epochs_.at(target) : other->epoch;
  e.sequence = next_melee_sequence_++;
  e.contact = true;
  e.contact_data = data;
  e.attack_generation = attack_generation;
  auto bytes = encode_melee(e);
  if (bytes.empty())
    return false;
  conn_.queue(kCombatChannel, bytes);
  return true;
}

bool ClientPeer::send_attack_generation(core::EntityId source, core::EntityId recipient,
                                        std::uint64_t generation, std::uint8_t native_kind) {
  if (!world_config_ || !world_ready_ || conn_.outstanding_reliable() >= kObserverReliableLimit) {
    return false;
  }
  const auto grant = owned_.find(source);
  const auto* other = mirror_.get(recipient);
  if (grant == owned_.end() || !other || source == recipient) {
    return false;
  }
  MeleeEvent event;
  event.revision = world_config_->revision;
  event.source = source;
  event.target = recipient;
  event.source_epoch = grant->second.epoch;
  event.target_epoch =
      combat_epochs_.count(recipient) ? combat_epochs_.at(recipient) : other->epoch;
  event.sequence = next_melee_sequence_++;
  event.attack_generation_marker = true;
  event.attack_generation = generation;
  event.native_attack_kind = native_kind;
  const auto bytes = encode_melee(event);
  if (bytes.empty()) {
    return false;
  }
  conn_.queue(kCombatChannel, bytes);
  return true;
}

bool ClientPeer::send_melee(core::EntityId source, core::EntityId target,
                            const NativeMeleeData& data, bool executor_is_target,
                            bool authority_is_target) {
  if (!world_config_ || !world_ready_)
    return false;
  auto grant = owned_.find(authority_is_target ? target : source);
  auto victim = mirror_.get(authority_is_target ? source : target);
  if (grant == owned_.end() || !victim || source == target || !valid_melee_data(data))
    return false;
  MeleeEvent e;
  e.revision = world_config_->revision;
  e.source = source;
  e.target = target;
  e.authority_is_target = authority_is_target;
  const auto other_epoch = combat_epochs_.count(authority_is_target ? source : target)
                               ? combat_epochs_.at(authority_is_target ? source : target)
                               : victim->epoch;
  e.source_epoch = authority_is_target ? other_epoch : grant->second.epoch;
  e.target_epoch = authority_is_target ? grant->second.epoch : other_epoch;
  e.sequence = next_melee_sequence_++;
  e.data = data;
  e.executor_is_target = executor_is_target;
  conn_.queue(kCombatChannel, encode_melee(e));
  return true;
}

bool ClientPeer::send_combat_action(core::EntityId source, core::EntityId target,
                                    const CombatActionData& data) {
  if (!world_config_ || !world_ready_ || !valid_combat_action(data))
    return false;
  auto grant = owned_.find(source);
  auto other = mirror_.get(target);
  if (grant == owned_.end() || !other || source == target)
    return false;
  MeleeEvent e;
  e.revision = world_config_->revision;
  e.source = source;
  e.target = target;
  e.source_epoch = grant->second.epoch;
  e.target_epoch = combat_epochs_.count(target) ? combat_epochs_.at(target) : other->epoch;
  e.sequence = next_melee_sequence_++;
  e.input = true;
  e.input_data = data;
  conn_.queue(kCombatChannel, encode_melee(e));
  return true;
}

bool ClientPeer::send_sync_hit(core::EntityId source, core::EntityId target,
                               const NativeSyncHitData& data) {
  if (!world_config_ || !world_ready_ || !valid_sync_hit_data(data))
    return false;
  auto grant = owned_.find(source);
  auto victim = mirror_.get(target);
  if (grant == owned_.end() || !victim || source == target)
    return false;
  MeleeEvent e;
  e.revision = world_config_->revision;
  e.source = source;
  e.target = target;
  e.source_epoch = grant->second.epoch;
  e.target_epoch = combat_epochs_.count(target) ? combat_epochs_.at(target) : victim->epoch;
  e.sequence = next_melee_sequence_++;
  e.sync_hit = true;
  e.sync_hit_data = data;
  conn_.queue(kCombatChannel, encode_melee(e));
  return true;
}

bool ClientPeer::send_reaction(core::EntityId source, core::EntityId target,
                               const NativeReactionData& data, bool authority_is_target) {
  if (!world_config_ || !world_ready_)
    return false;
  auto grant = owned_.find(authority_is_target ? target : source);
  auto victim = mirror_.get(authority_is_target ? source : target);
  if (grant == owned_.end() || !victim || source == target || !valid_reaction_data(data))
    return false;
  MeleeEvent e;
  e.revision = world_config_->revision;
  e.source = source;
  e.target = target;
  e.authority_is_target = authority_is_target;
  const auto other_epoch = combat_epochs_.count(authority_is_target ? source : target)
                               ? combat_epochs_.at(authority_is_target ? source : target)
                               : victim->epoch;
  e.source_epoch = authority_is_target ? other_epoch : grant->second.epoch;
  e.target_epoch = authority_is_target ? grant->second.epoch : other_epoch;
  e.sequence = next_melee_sequence_++;
  e.reaction = true;
  e.reaction_data = data;
  conn_.queue(kCombatChannel, encode_melee(e));
  return true;
}
}  // namespace kcd2s::integration
