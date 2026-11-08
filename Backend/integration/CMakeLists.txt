# Backend/integration/ — headless server tick pipeline + client peer (build-order step 6). The Windows
# engine bridge (Client/bridge) is separate and built only in the client target.
add_library(kcd2s_integration STATIC server.cpp client_peer.cpp client_bridge_driver.cpp netbus.cpp
            shared_state_sync.cpp session.cpp demo.cpp player_slots.cpp)
add_library(kcd2s::integration ALIAS kcd2s_integration)

target_include_directories(kcd2s_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_integration
  PUBLIC kcd2s::voice kcd2s::core kcd2s::wire kcd2s::transport kcd2s::replication kcd2s::bridge kcd2s::sdk
         kcd2s::flags)

if(KCD2S_BUILD_TESTS)
  # Explicit production-path diagnostic benchmark, never a CI timing threshold.
  add_executable(bench_integration_snapshot_pipeline tests/snapshot_pipeline_bench.cpp)
  target_link_libraries(bench_integration_snapshot_pipeline PRIVATE kcd2s::integration)
  add_executable(test_integration_script_child tests/script_process_child.cpp)
  target_link_libraries(test_integration_script_child PRIVATE kcd2s::sdk kcd2s::flags)
  kcd2s_add_test(NAME integration.script_inbox TARGET test_integration_script_inbox
                SOURCES tests/script_inbox_test.cpp LIBS kcd2s::integration)
  add_dependencies(test_integration_script_inbox test_integration_script_child)
  target_compile_definitions(test_integration_script_inbox PRIVATE
    KCDS_SCRIPT_TEST_CHILD="$<TARGET_FILE:test_integration_script_child>")
  set_tests_properties(integration.script_inbox PROPERTIES TIMEOUT 60)
  kcd2s_add_test(NAME integration.player_input_publication TARGET test_integration_player_input_publication
                SOURCES tests/player_input_publication_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.body_state_publication TARGET test_integration_body_state_publication
                SOURCES tests/body_state_publication_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.visibility TARGET test_integration_visibility
                SOURCES tests/visibility_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.script_channel TARGET test_integration_script_channel
                SOURCES tests/script_channel_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.horse TARGET test_integration_horse SOURCES tests/horse_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.npc TARGET test_integration_npc
                 SOURCES tests/npc_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.hud_messages TARGET test_integration_hud_messages
                 SOURCES tests/hud_messages_test.cpp LIBS kcd2s::integration)
  # No two control message tags in include/ share a value.
  kcd2s_add_test(NAME integration.wire_tags TARGET test_integration_wire_tags
                 SOURCES tests/wire_tags_test.cpp)
  target_compile_definitions(test_integration_wire_tags
    PRIVATE KCD2S_INTEGRATION_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include")
  kcd2s_add_test(NAME integration.pickpocket TARGET test_integration_pickpocket
                 SOURCES tests/pickpocket_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.grime TARGET test_integration_grime
                 SOURCES tests/grime_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.world_time TARGET test_integration_world_time
                 SOURCES tests/world_time_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.world_weather TARGET test_integration_world_weather
                 SOURCES tests/world_weather_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.world_markers TARGET test_integration_world_markers
                 SOURCES tests/world_markers_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.doors TARGET test_integration_doors
                 SOURCES tests/doors_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.placed_objects TARGET test_integration_placed_objects
                 SOURCES tests/placed_objects_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.prop_orders TARGET test_integration_prop_orders
                 SOURCES tests/prop_orders_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.smart_objects TARGET test_integration_smart_objects
                 SOURCES tests/smart_objects_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.graves TARGET test_integration_graves
                 SOURCES tests/graves_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.recipe_tables TARGET test_integration_recipe_tables
                 SOURCES tests/recipe_tables_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.carts TARGET test_integration_carts
                 SOURCES tests/carts_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.previews TARGET test_integration_previews
                 SOURCES tests/previews_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.body_carry TARGET test_integration_body_carry
                 SOURCES tests/body_carry_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.player_animation TARGET test_integration_player_animation
                 SOURCES tests/player_animation_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.freecam TARGET test_integration_freecam
                 SOURCES tests/freecam_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.needs TARGET test_integration_needs
                 SOURCES tests/needs_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.chat TARGET test_integration_chat SOURCES tests/chat_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.combat TARGET test_integration_combat
                 SOURCES tests/combat_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.combat_capabilities TARGET test_integration_combat_capabilities
                 SOURCES tests/combat_capabilities_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.animation TARGET test_integration_animation
                 SOURCES tests/animation_test.cpp LIBS kcd2s::integration)
  # Opt-in benchmark over actual captured animation payloads; no timing CI threshold.
  add_executable(bench_integration_animation_codec tests/animation_codec_bench.cpp)
  target_link_libraries(bench_integration_animation_codec PRIVATE kcd2s::integration)
  kcd2s_add_test(NAME integration.human_overlay TARGET test_integration_human_overlay
                 SOURCES tests/human_overlay_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.human_overlay_transport TARGET test_integration_human_overlay_transport
                 SOURCES tests/human_overlay_transport_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.convergence TARGET test_integration_convergence
                 SOURCES tests/convergence_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.client_bridge_driver TARGET test_integration_client_bridge
                 SOURCES tests/client_bridge_driver_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.netbus TARGET test_integration_netbus
                 SOURCES tests/netbus_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.deltas TARGET test_integration_deltas
                 SOURCES tests/delta_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.interest_budget TARGET test_integration_interest_budget
                 SOURCES tests/interest_budget_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.character_admission TARGET test_integration_character_admission
                 SOURCES tests/character_admission_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.upstream TARGET test_integration_upstream
                 SOURCES tests/upstream_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.demo TARGET test_integration_demo
                 SOURCES tests/demo_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.decoder_fuzz TARGET test_integration_decoder_fuzz
                 SOURCES tests/decoder_fuzz_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.shared_state_sync TARGET test_integration_shared_state_sync
                 SOURCES tests/shared_state_sync_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.player_slots TARGET test_integration_player_slots
                 SOURCES tests/player_slots_test.cpp
                 LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.session TARGET test_integration_session
                 SOURCES tests/session_test.cpp
                 LIBS kcd2s::integration)
endif()

# kcd2s-demo CLI: run the headless end-to-end scenario.
add_executable(kcd2s-demo demo_main.cpp)
target_link_libraries(kcd2s-demo PRIVATE kcd2s::integration)
if(KCD2S_BUILD_SERVER AND NOT WIN32)
  # The dedicated server on Linux: POSIX sockets, fork/exec resource host, libcurl
  # for the master-server listing.
  find_package(Threads REQUIRED)
  find_package(CURL REQUIRED)
  add_executable(kcd2s-server local/server_main.cpp)
  target_link_libraries(kcd2s-server PRIVATE kcd2s::integration kcd2s::masterclient
                                             Threads::Threads CURL::libcurl)
  target_compile_definitions(kcd2s-server PRIVATE KCDS_SERVER_VERSION="${PROJECT_VERSION}")
  # One binary for every distribution and container: the C++ runtime is linked in, so the host
  # needs only glibc and libcurl.
  target_link_options(kcd2s-server PRIVATE -static-libstdc++ -static-libgcc)
endif()
if(WIN32 AND KCD2S_BUILD_SERVER)
  add_executable(kcd2s-server local/server_main.cpp)
  target_link_libraries(kcd2s-server PRIVATE kcd2s::integration kcd2s::masterclient ws2_32 psapi
                                             winhttp)
  target_compile_definitions(kcd2s-server PRIVATE KCDS_SERVER_VERSION="${PROJECT_VERSION}")
  add_executable(kcd2s-client-resource-probe local/client_resource_probe.cpp)
  target_link_libraries(kcd2s-client-resource-probe PRIVATE kcd2s::integration ws2_32)
  add_executable(kcd2s-resource-probe local/resource_probe.cpp)
  target_link_libraries(kcd2s-resource-probe PRIVATE kcd2s::integration ws2_32)
  add_executable(kcd2s-network-probe local/probe_main.cpp)
  target_link_libraries(kcd2s-network-probe PRIVATE kcd2s::integration ws2_32)
  # Headless KSTP load generator for the measurement tooling (not a game client).
  add_executable(kcd2s-load-probe local/load/load_probe_main.cpp)
  target_link_libraries(kcd2s-load-probe PRIVATE kcd2s::integration ws2_32 psapi)
endif()
if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME integration.world_config TARGET test_integration_world_config
                SOURCES tests/world_config_test.cpp LIBS kcd2s::integration)
  # Knock-out instead of death: report, revive and vision messages.
  kcd2s_add_test(NAME integration.knockout TARGET test_integration_knockout
                 SOURCES tests/knockout_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.player_engine_commands TARGET test_integration_player_engine_commands
                 SOURCES tests/player_engine_commands_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.combat_marker TARGET test_integration_combat_marker
                 SOURCES tests/combat_marker_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.session_end TARGET test_integration_session_end
                 SOURCES tests/session_end_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.restricted_area_marker
                 TARGET test_integration_restricted_area_marker
                 SOURCES tests/restricted_area_marker_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.third_person TARGET test_integration_third_person
                 SOURCES tests/third_person_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.raycast_camera TARGET test_integration_raycast_camera
                 SOURCES tests/raycast_camera_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.pillory TARGET test_integration_pillory
                 SOURCES tests/pillory_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.level_edits TARGET test_integration_level_edits
                 SOURCES tests/level_edits_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.stash_unlock TARGET test_integration_stash_unlock
                 SOURCES tests/stash_unlock_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.quests TARGET test_integration_quests
                 SOURCES tests/quests_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.station_action TARGET test_integration_station_action
                 SOURCES tests/station_action_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.crafting TARGET test_integration_crafting
                 SOURCES tests/crafting_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.dice TARGET test_integration_dice
                 SOURCES tests/dice_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.gathering TARGET test_integration_gathering
                 SOURCES tests/gathering_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.cookpot TARGET test_integration_cookpot
                 SOURCES tests/cookpot_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.lights TARGET test_integration_lights
                 SOURCES tests/lights_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.crafting_table TARGET test_integration_crafting_table
                 SOURCES tests/crafting_table_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.dice_view TARGET test_integration_dice_view
                 SOURCES tests/dice_view_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.dialogue TARGET test_integration_dialogue
                 SOURCES tests/dialogue_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.vendor TARGET test_integration_vendor
                 SOURCES tests/vendor_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.appearance TARGET test_integration_appearance
                 SOURCES tests/appearance_test.cpp LIBS kcd2s::integration)
  # The seats a player holds: the "seats" field.
  kcd2s_add_test(NAME integration.seats TARGET test_integration_seats
                 SOURCES tests/seats_test.cpp LIBS kcd2s::integration)
  # What a player holds in its hands: the "hands" field.
  kcd2s_add_test(NAME integration.hands TARGET test_integration_hands
                 SOURCES tests/hands_test.cpp LIBS kcd2s::integration)
  # Player names: NICKNAME and its cleaning.
  kcd2s_add_test(NAME integration.player_identity TARGET test_integration_player_identity
                 SOURCES tests/player_identity_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.nameplates TARGET test_integration_nameplates
                 SOURCES tests/nameplates_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.physical_prop TARGET test_integration_physical_prop
                 SOURCES tests/physical_prop_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.pose_animation TARGET test_integration_pose_animation
                 SOURCES tests/pose_animation_test.cpp LIBS kcd2s::integration)
  # Player stats: the "vitals" and "stats" fields and SET_STAT.
  kcd2s_add_test(NAME integration.player_stats TARGET test_integration_player_stats
                 SOURCES tests/player_stats_test.cpp LIBS kcd2s::integration)
  # The pace rule a server puts on a player: MOVEMENT_MODE.
  kcd2s_add_test(NAME integration.movement_mode TARGET test_integration_movement_mode
                 SOURCES tests/movement_mode_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.posture TARGET test_integration_posture
                 SOURCES tests/posture_test.cpp LIBS kcd2s::integration)
  # A player's progression: PROGRESSION.
  kcd2s_add_test(NAME integration.progression TARGET test_integration_progression
                 SOURCES tests/progression_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.siege TARGET test_integration_siege
                 SOURCES tests/siege_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.ladders_stones TARGET test_integration_ladders_stones
                 SOURCES tests/ladders_stones_test.cpp LIBS kcd2s::integration)
  # The owner's RPG soul on its replicas: the replicaSoul field.
  kcd2s_add_test(NAME integration.replica_soul TARGET test_integration_replica_soul
                 SOURCES tests/replica_soul_test.cpp LIBS kcd2s::integration)
  # The owner's fighting buffs on its replicas: the replicaBuffs field.
  kcd2s_add_test(NAME integration.replica_buffs TARGET test_integration_replica_buffs
                 SOURCES tests/replica_buffs_test.cpp LIBS kcd2s::integration)
  # The status effects a server put on a horse: the HorseBuffs field.
  kcd2s_add_test(NAME integration.horse_buffs TARGET test_integration_horse_buffs
                 SOURCES tests/horse_buffs_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.buffs TARGET test_integration_buffs
                 SOURCES tests/buffs_test.cpp LIBS kcd2s::integration)
  # The resources' own buffs: CUSTOM_BUFF.
  kcd2s_add_test(NAME integration.custom_buffs TARGET test_integration_custom_buffs
                 SOURCES tests/custom_buffs_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME integration.inventory TARGET test_integration_inventory
                 SOURCES tests/inventory_test.cpp LIBS kcd2s::integration)
  # Portable load-measurement helpers (histogram, counters, JSON lines, probe options/workload).
  kcd2s_add_test(NAME integration.load_metrics TARGET test_integration_load_metrics
                 SOURCES tests/load_metrics_test.cpp LIBS kcd2s::integration)
  # Scaling settings of the local host (portable parser and derived budgets).
  kcd2s_add_test(NAME integration.host_settings TARGET test_integration_host_settings
                 SOURCES tests/host_settings_test.cpp LIBS kcd2s::integration)
  # A player held out of the world (LOBBY, Player.setLobby).
  kcd2s_add_test(NAME integration.lobby TARGET test_integration_lobby
                 SOURCES tests/lobby_test.cpp LIBS kcd2s::integration)
  # Master-server registration settings of the local server (portable parser).
  kcd2s_add_test(NAME integration.master_registration TARGET test_integration_master_registration
                 SOURCES tests/master_registration_test.cpp LIBS kcd2s::integration kcds::launcher)
  # The game client's resource download cache (portable).
  kcd2s_add_test(NAME integration.download_cache TARGET test_integration_download_cache
                 SOURCES tests/download_cache_test.cpp LIBS kcd2s::integration)
  # Fixed-rate tick schedule of the local server loop (portable bookkeeping).
  kcd2s_add_test(NAME integration.tick_schedule TARGET test_integration_tick_schedule
                 SOURCES tests/tick_schedule_test.cpp LIBS kcd2s::integration)
  # Resource host view and player reports as changed-state deltas (portable bookkeeping).
  kcd2s_add_test(NAME integration.view_reports TARGET test_integration_view_reports
                 SOURCES tests/view_reports_test.cpp LIBS kcd2s::integration)
  # Remote players shown smoothly: the replica timeline.
  kcd2s_add_test(NAME integration.replica_timeline TARGET test_integration_replica_timeline
                 SOURCES tests/replica_timeline_test.cpp LIBS kcd2s::integration)
endif()
if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME integration.departure TARGET test_integration_departure
                 SOURCES tests/departure_test.cpp LIBS kcd2s::integration)
  if(WIN32 AND KCD2S_BUILD_SERVER)
    kcd2s_add_test(NAME integration.licence_join TARGET test_integration_licence_join
                   SOURCES tests/licence_join_test.cpp LIBS kcd2s::integration ws2_32)
    find_program(KCDS_TEST_NODE_EXECUTABLE NAMES node)
    if(KCDS_TEST_NODE_EXECUTABLE)
      kcd2s_add_test(NAME integration.licence_admission_binding
                     TARGET test_integration_licence_admission_binding
                     SOURCES tests/licence_admission_binding_test.cpp
                     LIBS kcd2s::integration ws2_32)
      target_compile_definitions(test_integration_licence_admission_binding PRIVATE
        KCDS_TEST_NODE_PATH="${KCDS_TEST_NODE_EXECUTABLE}"
        KCDS_TEST_CODEC_PATH="${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/codec.cjs")
    endif()
    # Real loopback UDP through the local Host/Client runtime (Windows sockets).
    kcd2s_add_test(NAME integration.graceful_disconnect TARGET test_integration_graceful_disconnect
                   SOURCES tests/graceful_disconnect_test.cpp LIBS kcd2s::integration ws2_32)
    # The server's socket reader thread.
    kcd2s_add_test(NAME integration.udp_reader TARGET test_integration_udp_reader
                   SOURCES tests/udp_reader_test.cpp LIBS kcd2s::integration ws2_32)
  endif()
endif()
