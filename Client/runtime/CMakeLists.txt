# Native multiplayer client and launchers. Shared contracts come from Backend/integration.
option(KCD2S_BUILD_GAME_CLIENT "Build native experimental multiplayer adapter" OFF)
if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME runtime.material_capacity TARGET test_runtime_material_capacity
    SOURCES characters/tests/material_capacity_test.cpp)
  kcd2s_add_test(NAME runtime.feature_capacity TARGET test_runtime_feature_capacity
    SOURCES characters/tests/feature_capacity_test.cpp)
  kcd2s_add_test(NAME runtime.pipeline_metrics TARGET test_runtime_pipeline_metrics
    SOURCES characters/tests/pipeline_metrics_test.cpp)
  kcd2s_add_test(NAME runtime.horse_riders TARGET test_runtime_horse_riders
    SOURCES replicas/horse_riders_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.npc_combat_mode TARGET test_runtime_npc_combat_mode
    SOURCES npcs/combat_mode_test.cpp)
  kcd2s_add_test(NAME runtime.distance_lod TARGET test_runtime_distance_lod
    SOURCES replicas/distance_lod_policy_test.cpp)
  kcd2s_add_test(NAME runtime.replica_drift TARGET test_runtime_replica_drift
    SOURCES replicas/replica_drift_test.cpp)
  kcd2s_add_test(NAME runtime.ladder_hold TARGET test_runtime_ladder_hold
    SOURCES movement/tests/ladder_hold_test.cpp)
  kcd2s_add_test(NAME runtime.arm_ik TARGET test_runtime_arm_ik
    SOURCES animation/tests/arm_ik_test.cpp)
  kcd2s_add_test(NAME runtime.model_slots TARGET test_runtime_model_slots
    SOURCES architect/tests/model_slots_test.cpp)
  kcd2s_add_test(NAME runtime.model_split TARGET test_runtime_model_split
    SOURCES architect/tests/model_split_test.cpp)
  kcd2s_add_test(NAME runtime.snow_cover TARGET test_runtime_snow_cover
    SOURCES winter/tests/snow_cover_test.cpp)
  kcd2s_add_test(NAME runtime.character_stats TARGET test_runtime_character_stats
    SOURCES hud/character_stats/tests/stat_rows_test.cpp)
  kcd2s_add_test(NAME runtime.base64 TARGET test_runtime_base64
    SOURCES files/tests/base64_test.cpp)
  # The client's logs stay small on disk (logging).
  kcd2s_add_test(NAME runtime.log_files TARGET test_runtime_log_files
    SOURCES logging/tests/log_files_test.cpp logging/log_files.cpp)
  kcd2s_add_test(NAME runtime.layer_catalog TARGET test_runtime_layer_catalog
    SOURCES level/tests/layer_catalog_test.cpp level/layer_catalog.cpp)
  kcd2s_add_test(NAME runtime.zip_read TARGET test_runtime_zip_read
    SOURCES files/tests/zip_read_test.cpp files/zip_read.cpp files/stored_zip.cpp)
  kcd2s_add_test(NAME runtime.studio_level TARGET test_runtime_studio_level
    SOURCES studio/studio_level_test.cpp studio/studio_level.cpp files/zip_read.cpp
            files/stored_zip.cpp)
  kcd2s_add_test(NAME runtime.animation_samples TARGET test_runtime_animation_samples
    SOURCES characters/tests/animation_samples_test.cpp)
  kcd2s_add_test(NAME runtime.skinning_capacity TARGET test_runtime_skinning_capacity
    SOURCES characters/tests/skinning_capacity_test.cpp)
  kcd2s_add_test(NAME runtime.skinning_metrics TARGET test_runtime_skinning_metrics
    SOURCES characters/tests/skinning_metrics_test.cpp)
  kcd2s_add_test(NAME runtime.motion_index TARGET test_runtime_motion_index
    SOURCES replicas/motion_index_test.cpp)
  kcd2s_add_test(NAME runtime.remote_ui_role TARGET test_runtime_remote_ui_role
    SOURCES replicas/remote_ui_role_test.cpp replicas/remote_interactor_scope_test.cpp)
  kcd2s_add_test(NAME runtime.retirement_fence TARGET test_runtime_retirement_fence
    SOURCES replicas/retirement_fence_test.cpp)
  kcd2s_add_test(NAME runtime.damage_reports TARGET test_runtime_damage_reports
    SOURCES combat/damage_reports_test.cpp combat/damage_reports.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.replica_wake TARGET test_runtime_replica_wake
    SOURCES knockout/replica_wake_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.faint TARGET test_runtime_faint
    SOURCES knockout/faint_test.cpp knockout/faint.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.fall_knockout TARGET test_fall_knockout
    SOURCES knockout/fall_knockout_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.npc_health TARGET test_runtime_npc_health
    SOURCES npcs/npc_health_test.cpp npcs/npc_health.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.replica_contact TARGET test_runtime_replica_contact
    SOURCES combat/replica_contact_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.look_pose_layer TARGET test_runtime_look_pose_layer
    SOURCES look/look_pose_layer_test.cpp)
  kcd2s_add_test(NAME runtime.clinch_handshake TARGET test_runtime_clinch_handshake
    SOURCES combat/clinch_handshake_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.refused_pair TARGET test_runtime_refused_pair
    SOURCES combat/refused_pair_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.owned_horse TARGET test_runtime_owned_horse
    SOURCES riding/owned_horse_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.dodges TARGET test_runtime_dodges
    SOURCES combat/dodges_test.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.priced_hits TARGET test_runtime_priced_hits
    SOURCES combat/priced_hits_test.cpp combat/priced_hits.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.correction_blend TARGET test_runtime_correction_blend
    SOURCES local_player/correction_blend_test.cpp)
  kcd2s_add_test(NAME runtime.max_column_movie TARGET test_runtime_max_column_movie
    SOURCES inventory/max_column_movie_test.cpp inventory/max_column_movie.cpp)
  kcd2s_add_test(NAME runtime.web_icons TARGET test_runtime_web_icons
    SOURCES web/icons/tests/icons_test.cpp web/icons/dds_picture.cpp web/icons/png_file.cpp)
  kcd2s_add_test(NAME runtime.hint_width_movie TARGET test_runtime_hint_width_movie
    SOURCES action_hints/hint_width_movie_test.cpp action_hints/hint_width_movie.cpp)
  kcd2s_add_test(NAME runtime.discord_activity TARGET test_runtime_discord_activity
    SOURCES discord/discord_activity_test.cpp)
  kcd2s_add_test(NAME runtime.discord_login TARGET test_runtime_discord_login
    SOURCES identity/discord_callback_test.cpp)
endif()
if(WIN32 AND KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME runtime.player_stats TARGET test_runtime_player_stats
    SOURCES stats/player_stats_test.cpp stats/player_stats.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.human_overlay TARGET test_runtime_human_overlay
    SOURCES animation/human_overlay_test.cpp animation/human_overlay.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.paired_start TARGET test_runtime_paired_start
    SOURCES animation/paired_start_test.cpp animation/paired_start.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.pe_sections TARGET test_runtime_pe_sections
    SOURCES core/pe_sections_test.cpp)
  kcd2s_add_test(NAME runtime.game_layouts TARGET test_runtime_game_layouts
    SOURCES core/game_layouts_test.cpp)
  kcd2s_add_test(NAME runtime.body_state TARGET test_runtime_body_state
    SOURCES knockout/body_state_test.cpp knockout/body_state.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.player_commands TARGET test_runtime_player_commands
    SOURCES stats/player_commands_test.cpp stats/player_commands.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.needs TARGET test_runtime_needs
    SOURCES needs/needs_test.cpp needs/needs.cpp LIBS kcd2s::integration)
  kcd2s_add_test(NAME runtime.bug_report TARGET test_runtime_bug_report
    SOURCES bugreport/bug_report_test.cpp bugreport/bug_report.cpp LIBS kcd2s::masterclient)
  kcd2s_add_test(NAME runtime.asset_container TARGET test_runtime_asset_container
    SOURCES assets/asset_container_test.cpp assets/asset_container.cpp LIBS bcrypt)
  kcd2s_add_test(NAME runtime.texture_dds TARGET test_runtime_texture_dds
    SOURCES studio/texture_dds_test.cpp studio/texture_dds.cpp)
  kcd2s_add_test(NAME runtime.cgf_patch TARGET test_runtime_cgf_patch
    SOURCES studio/cgf_patch_test.cpp studio/cgf_patch.cpp)
  kcd2s_add_test(NAME runtime.cgf_mesh TARGET test_runtime_cgf_mesh
    SOURCES studio/cgf_mesh_test.cpp studio/cgf_mesh.cpp)
  kcd2s_add_test(NAME runtime.pack_index TARGET test_runtime_pack_index
    SOURCES studio/pack_index_test.cpp studio/pack_index.cpp)
  kcd2s_add_test(NAME runtime.whip_lash TARGET test_runtime_whip_lash
    SOURCES physical_props/whip_lash_test.cpp physical_props/whip_lash.cpp)
endif()
if(WIN32)
  # Seals a zip pak into a .kcdsasset (assets).
  add_executable(kcds-asset-seal assets/seal_tool.cpp assets/asset_container.cpp
                                 identity/hardware_id.cpp identity/sha256.cpp)
  target_link_libraries(kcds-asset-seal PRIVATE bcrypt advapi32)
endif()
if(WIN32 AND KCD2S_BUILD_GAME_CLIENT)
  enable_language(C)
  enable_language(RC)
  # The version block of a client binary (branding/assets/kcds-version.rc.in), as the launcher has:
  # its file version has four numbers, major, minor, patch and the hotfix (0 for a release). Each
  # target gets its own copy with its values (any generator; no resource compiler defines).
  set(kcds_version_hotfix "${PROJECT_VERSION_TWEAK}")
  if(kcds_version_hotfix STREQUAL "")
    set(kcds_version_hotfix 0)
  endif()
  set(KCDS_VERSION_RC_NUMBERS
      "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${kcds_version_hotfix}")
  function(kcds_version_info target description file_name file_type)
    set(KCDS_FILE_DESCRIPTION "${description}")
    set(KCDS_ORIGINAL_FILENAME "${file_name}")
    set(KCDS_FILE_TYPE "${file_type}")
    set(version_rc "${CMAKE_CURRENT_BINARY_DIR}/version/${target}-version.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/branding/assets/kcds-version.rc.in" "${version_rc}"
                   @ONLY)
    target_sources(${target} PRIVATE "${version_rc}")
  endfunction()
  set(KCD2S_MINHOOK_DIR "" CACHE PATH "MinHook source root")
  if(NOT EXISTS "${KCD2S_MINHOOK_DIR}/include/MinHook.h")
    message(FATAL_ERROR "KCD2S_MINHOOK_DIR must contain MinHook source")
  endif()
  add_library(kcd2s_runtime_minhook STATIC
    ${KCD2S_MINHOOK_DIR}/src/buffer.c ${KCD2S_MINHOOK_DIR}/src/hook.c
    ${KCD2S_MINHOOK_DIR}/src/trampoline.c ${KCD2S_MINHOOK_DIR}/src/hde/hde64.c)
  target_include_directories(kcd2s_runtime_minhook PUBLIC ${KCD2S_MINHOOK_DIR}/include)
  # MinHook's trampoline.c uses the __movsb intrinsic for MSVC (_MSC_VER) without including
  # intrin.h, which MSVC forgives and clang-cl (the Linux cross build) does not.
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(kcd2s_runtime_minhook PRIVATE /FIintrin.h)
  endif()
  set(KCDS_WEBVIEW2_ROOT "" CACHE PATH "Pinned WebView2 SDK root for client resource runtime")
  if(NOT EXISTS "${KCDS_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
    message(FATAL_ERROR "KCDS_WEBVIEW2_ROOT must contain the pinned WebView2 SDK")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(resource_runtime_header "${CMAKE_CURRENT_BINARY_DIR}/generated/resource_runtime_html.hpp")
  add_custom_command(OUTPUT "${resource_runtime_header}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/tools/build_resource_runtime.py" "${resource_runtime_header}"
    DEPENDS "${PROJECT_SOURCE_DIR}/Dev/tools/build_resource_runtime.py"
            "${PROJECT_SOURCE_DIR}/Client/resources/worker.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/runtime.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/entities.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/entity_transfer.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/web.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/loading_screen.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/input.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/screen_effects.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/hud_rects.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/hud.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/chat.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/action_hints.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/action_menu.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/world_map.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/book.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/vendor_screen.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/sync_channel.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/native_ui.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/native_host_movie.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/character_creator.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/creator_panel.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/tutorial_hint.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/noclip.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/local_voice.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/keybinds.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/camera.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/render.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/vfx.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/lights.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/prefabs.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/collision.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/terrain.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/vegetation.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/catalogue.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/theme.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/glyphs.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/icons.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/marks.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/shell.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/group_copy.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/story_states.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/collision_layer.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/house_fronts.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/ai_connect.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/ai_commands.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/materials.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/material_work.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/interiors.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/opening.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/arranging.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/tags.cjs"
            "${PROJECT_SOURCE_DIR}/Dev/tools/architect-mcp/models/bytes.cjs"
            "${PROJECT_SOURCE_DIR}/Dev/tools/architect-mcp/models/cgf_writer.cjs"
            "${PROJECT_SOURCE_DIR}/Dev/tools/architect-mcp/models/open_core.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/blender_export.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/server_export.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/movers.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/architect/editor.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/glyphs.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/animation.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/clip_bake.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/animation_tools.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/lash_keys.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/lash_motion.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/lash_design.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/timeline.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/outfits.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/placing.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/shapes.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/camera.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/studio/editor.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/world.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/previews.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/props/catalog.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/props/catalog.json"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/props/collision.json"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/props/prefabs.json"
            "${PROJECT_SOURCE_DIR}/Client/resources/local_npcs.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/gizmos.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/nameplates.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/discord.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/game.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/audio.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/assets.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/follow.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/tabletop.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/emotes.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/blips.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/markers/blip_types.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/markers/decals.json"
            "${PROJECT_SOURCE_DIR}/Client/resources/inventory.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/progression.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/buffs.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/sealed_files.cjs"
            "${PROJECT_SOURCE_DIR}/Client/resources/bundle_cache.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/languages/lua_host.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/languages/csharp_host.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/languages/vendor/wasmoon/index.js"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/languages/vendor/wasmoon/glue.wasm"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/services.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/shared_state.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/math.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/camera/shots.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/vfx/vfx_values.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/vfx/catalog.json"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/world/mesh_categories.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/lights/light_values.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/appearance/appearances.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/npcs/npc_values.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/stats/player_stats.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/posture/player_posture.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/progression/catalog.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/progression/player_progression.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/progression/catalog.json"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/buffs/catalog.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/buffs/player_buffs.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/buffs/catalog.json"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/inventory/catalog.cjs"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/inventory/catalog.json"
            "${PROJECT_SOURCE_DIR}/Backend/scripting/runtimes/js/appearance/catalog.json")
  add_executable(kcd2s-resource-runtime ../resources/host_main.cpp
    ../resources/runtime_profiles.cpp ../resources/runtime_files.cpp "${resource_runtime_header}")
  kcds_version_info(kcd2s-resource-runtime "KCD2-Synced resource runtime" "kcd2s-resource-runtime.exe" VFT_APP)
  if(KCD2S_BUILD_TESTS)
    # The runtime's WebView2 profiles never pile up on disk (resources).
    kcd2s_add_test(NAME runtime.resource_profiles TARGET test_runtime_resource_profiles
      SOURCES ../resources/runtime_profiles_test.cpp ../resources/runtime_profiles.cpp
      LIBS kcd2s::integration)
    # The runtime page reads only its own .NET files.
    kcd2s_add_test(NAME runtime.resource_files TARGET test_runtime_resource_files
      SOURCES ../resources/runtime_files_test.cpp ../resources/runtime_files.cpp
      LIBS kcd2s::integration)
  endif()
  # The .NET runtime C# client resources run on, next to the program in dotnet/
  # (Client/resources/runtime_files.hpp). Built when the .NET SDK is installed; without it the
  # client runs JavaScript and Lua resources only.
  find_program(KCDS_DOTNET_EXECUTABLE NAMES dotnet)
  if(KCDS_RELEASE_BUILD AND NOT KCDS_DOTNET_EXECUTABLE)
    message(FATAL_ERROR "A release client needs the .NET SDK for C# resources")
  endif()
  if(KCDS_DOTNET_EXECUTABLE AND Python3_EXECUTABLE)
    add_custom_target(kcds-csharp-host ALL
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/tools/build_csharp_host.py"
              --out "$<TARGET_FILE_DIR:kcd2s-resource-runtime>/dotnet"
      COMMENT "C# resource host (.NET WebAssembly)"
      VERBATIM)
    add_dependencies(kcd2s-resource-runtime kcds-csharp-host)
  endif()
  target_include_directories(kcd2s-resource-runtime PRIVATE
    "${KCDS_WEBVIEW2_ROOT}/build/native/include" "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(kcd2s-resource-runtime PRIVATE kcd2s::sdk
    "${KCDS_WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib"
    ole32 oleaut32 shlwapi version advapi32 shell32 user32)
  if(KCD2S_BUILD_TESTS)
    find_program(KCDS_NODE_EXECUTABLE NAMES node)
    if(KCDS_NODE_EXECUTABLE)
      add_test(NAME integration.client_runtime_process
        COMMAND "${KCDS_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/tools/test_resource_runtime.cjs"
                "$<TARGET_FILE:kcd2s-resource-runtime>")
      set_tests_properties(integration.client_runtime_process PROPERTIES TIMEOUT 40)
      add_test(NAME integration.client_timer_contract
        COMMAND "${KCDS_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/tools/test_client_timer_contract.cjs"
                "$<TARGET_FILE:kcd2s-resource-runtime>")
      set_tests_properties(integration.client_timer_contract PROPERTIES TIMEOUT 45)
      if(TARGET kcds-csharp-host)
        # A C# client resource in the real runtime, on the .NET files it serves.
        add_test(NAME integration.client_csharp_runtime
          COMMAND "${KCDS_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/tools/test_resource_runtime_csharp.cjs"
                  "$<TARGET_FILE:kcd2s-resource-runtime>")
        set_tests_properties(integration.client_csharp_runtime PROPERTIES TIMEOUT 60)
      endif()
      if(TARGET kcd2s-server AND TARGET kcd2s-client-resource-probe)
        add_test(NAME integration.entity_resource_roundtrip
          COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/testkit/entity_resource_roundtrip.py"
                  "$<TARGET_FILE:kcd2s-server>" "$<TARGET_FILE:kcd2s-client-resource-probe>"
                  "$<TARGET_FILE:kcd2s-resource-runtime>" "${KCDS_NODE_EXECUTABLE}")
        set_tests_properties(integration.entity_resource_roundtrip PROPERTIES TIMEOUT 50)
        add_test(NAME integration.state_change_roundtrip
          COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/testkit/state_change_roundtrip.py"
                  "$<TARGET_FILE:kcd2s-server>" "$<TARGET_FILE:kcd2s-client-resource-probe>"
                  "$<TARGET_FILE:kcd2s-resource-runtime>" "${KCDS_NODE_EXECUTABLE}")
        set_tests_properties(integration.state_change_roundtrip PROPERTIES TIMEOUT 50)
        add_test(NAME integration.player_visibility_roundtrip
          COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/testkit/player_visibility_roundtrip.py"
                  "$<TARGET_FILE:kcd2s-server>" "$<TARGET_FILE:kcd2s-client-resource-probe>"
                  "$<TARGET_FILE:kcd2s-resource-runtime>" "${KCDS_NODE_EXECUTABLE}")
        set_tests_properties(integration.player_visibility_roundtrip PROPERTIES TIMEOUT 60)
        add_test(NAME integration.guide_scenarios_roundtrip
          COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/testkit/guide_scenarios_roundtrip.py"
                  "$<TARGET_FILE:kcd2s-server>" "$<TARGET_FILE:kcd2s-client-resource-probe>"
                  "$<TARGET_FILE:kcd2s-resource-runtime>" "${KCDS_NODE_EXECUTABLE}")
        set_tests_properties(integration.guide_scenarios_roundtrip PROPERTIES TIMEOUT 60)
      endif()
    endif()
  endif()
  add_executable(kcd2s-inject-pid dev_tools/inject_pid.cpp)
  add_executable(kcd2s-launch-client dev_tools/launch_client.cpp)
  kcds_version_info(kcd2s-launch-client "KCD2-Synced game starter" "kcd2s-launch-client.exe" VFT_APP)
  target_link_libraries(kcd2s-launch-client PRIVATE shell32 ole32)
  # The loading screen rendered into PNGs at the window sizes players have, to check a
  # design change without the game.
  add_executable(kcds-loading-preview dev_tools/loading_preview.cpp branding/loading_screen.cpp)
  target_link_libraries(kcds-loading-preview PRIVATE d2d1 dwrite windowscodecs ole32)
  # The developer launch arguments (a 960x540 window, the game's -devmode) for the local test
  # kit. Off by default: a release build starts the game as the player set it up.
  option(KCDS_DEV_BUILD "Developer build: windowed 960x540 games with -devmode for the test kit" OFF)
  if(KCDS_DEV_BUILD)
    target_compile_definitions(kcd2s-launch-client PRIVATE KCDS_DEV_BUILD=1)
  endif()
  add_library(kcd2s-game-client SHARED game_client.cpp chat/chat_overlay.cpp
              voice/voice_client.cpp voice/voice_settings_page.cpp
              voice/voice_settings_page_install.cpp voice/voice_indicator.cpp)
  kcds_version_info(kcd2s-game-client "KCD2-Synced game client" "kcd2s-game-client-v4.dll" VFT_DLL)
  # The test kit's background mode (testing/background_mode.hpp) exists in developer builds only: a
  # released client never gives the foreground away, whatever the environment holds.
  if(KCDS_DEV_BUILD)
    target_compile_definitions(kcd2s-game-client PRIVATE KCDS_DEV_BUILD=1)
  endif()
  # The Asset/Animation Studio's floor, a mesh embedded in the client (studio/studio_floor.hpp).
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(studio_floor_header "${CMAKE_CURRENT_BINARY_DIR}/generated/studio_floor_files.hpp")
  add_custom_command(OUTPUT "${studio_floor_header}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/Dev/tools/assets/kcds_studio/make_studio_floor.py" "${studio_floor_header}"
    DEPENDS "${PROJECT_SOURCE_DIR}/Dev/tools/assets/kcds_studio/make_studio_floor.py"
            "${PROJECT_SOURCE_DIR}/Dev/tools/assets/blender/kcds_cgf.py")
  target_sources(kcd2s-game-client PRIVATE "${studio_floor_header}")
  # What the features split out of game_client.cpp share (core).
  target_sources(kcd2s-game-client PRIVATE core/log.cpp logging/log_files.cpp core/natives.cpp riding/native_horses.cpp riding/lead_rope.cpp riding/horse_speed_scale.cpp
                                           npcs/native_npcs.cpp knockout/knockout_hooks.cpp
                                           pillory/pillory_hooks.cpp world/world_entity_hooks.cpp
                                           session/server_address.cpp session/server_browser.cpp
                                           session/server_pings.cpp session/address_cards.cpp
                                           inventory/inventory_tick.cpp carts/carts_tick.cpp
                                           crafting/crafting_tick.cpp props/previews_tick.cpp
                                           vfx/placed_objects_tick.cpp buffs/buffs_tick.cpp
                                           progression/progression_tick.cpp chat/chat_tick.cpp
                                           hud/hud_calls.cpp siege/siege_tick.cpp
                                           vendors/vendors_tick.cpp replicas/forced_transforms.cpp
                                           stats/stats_tick.cpp movement/movement_mode_tick.cpp
                                           body_state/body_state_tick.cpp input/input_gate.cpp
                                           world/single_player_screens.cpp
                                           session/profile_settings.cpp session/leave_world.cpp
                                           dialogue/dialogue_tick.cpp camera/view_camera.cpp
                                           world/world_queries.cpp weapons/held_weapon.cpp
                                           pickpocket/pickpocket_tick.cpp
                                           carry/body_carry_tick.cpp ranged/shots_tick.cpp
                                           appearance/appearance_tick.cpp markers/marker_hooks.cpp
                                           look/native_look.cpp look/local_look.cpp
                                           replicas/replica_state.cpp replicas/replica_look.cpp
                                           replicas/deferred_moves.cpp replicas/held_items.cpp
                                           knockout/knockout_tick.cpp
                                           stats/player_command_binding.cpp
                                           combat/dodge_relay.cpp testing/test_hooks.cpp
                                           testing/background_mode.cpp
                                           testing/bone_camera.cpp testing/stack_sampler.cpp
                                           testing/fake_movement_probe.cpp testing/nav_listener_probe.cpp testing/prefab_probe.cpp
                                           testing/navmesh_probe.cpp
                                           testing/terrain_probe.cpp
                                           prefabs/game_prefabs.cpp prefabs/local_prefabs.cpp
                                           terrain/terrain_edits.cpp terrain/terrain_ops.cpp
                                           vegetation/local_vegetation.cpp
                                           collision/local_collision.cpp
                                           props/local_props.cpp
                                           architect/local_mode.cpp
                                           architect/local_level_edits.cpp
                                           architect/level_doors.cpp
                                           architect/level_sounds.cpp
                                           architect/navmesh_bake.cpp
                                           architect/user_assets.cpp
                                           architect/asset_files.cpp
                                           architect/level_properties.cpp
                                           maps/custom_maps.cpp
                                           maps/custom_navmesh.cpp
                                           architect/editor_overlay.cpp
                                           architect/project_store.cpp
                                           architect/editor_pipe.cpp
                                           studio/studio_scene.cpp
                                           studio/studio_level.cpp
                                           files/zip_read.cpp
                                           studio/studio_rope.cpp
                                           studio/studio_whip.cpp
                                           studio/studio_export.cpp
                                           studio/studio_skeleton.cpp
                                           studio/studio_pose.cpp
                                           studio/studio_outfit.cpp
                                           studio/studio_sky.cpp
                                           studio/studio_mesh.cpp
                                           studio/studio_skin.cpp
                                           studio/studio_skin_install.cpp
                                           studio/studio_game_clip.cpp
                                           studio/game_files.cpp
                                           studio/pack_index.cpp
                                           studio/studio_view.cpp
                                           studio/studio_hold.cpp
                                           studio/studio_prop_gizmo.cpp
                                           studio/studio_texture.cpp
                                           studio/studio_saves.cpp
                                           studio/studio_event.cpp
                                           studio/studio_floor.cpp
                                           studio/studio_grid.cpp
                                           studio/texture_dds.cpp
                                           studio/cgf_patch.cpp
                                           studio/cgf_mesh.cpp
                                           pose_animation/joint_turns.cpp
                                           pose_animation/clip_in_place.cpp
                                           pose_animation/player_animations.cpp
                                           pose_animation/hand_grip.cpp
                                           physical_props/rope_prop.cpp
                                           physical_props/cloth.cpp
                                           physical_props/whip_prop.cpp
                                           physical_props/whip_lash.cpp
                                           physical_props/player_props.cpp
                                           assets/memory_paks.cpp
                                           assets/asset_container.cpp
                                           architect/world_look.cpp
                                           architect/level_plants.cpp
                                           architect/level_layers.cpp
                                           terrain/merged_grass.cpp
                                           architect/player_body.cpp
                                           architect/saves.cpp
                                           architect/ai_bridge.cpp
                                           architect/cursor_pick.cpp
                                           carry/carry_orders.cpp riding/rider_diagnostics.cpp
                                           replicas/replica_removal.cpp combat/hit_feedback.cpp
                                           riding/trample.cpp movement/ladder_clips.cpp
                                           combat/hit_reactions.cpp npcs/npc_blows.cpp
                                           replicas/replica_autonomy.cpp
                                           knockout/replica_wake_tick.cpp
                                           combat/reaction_window.cpp combat/defence_timing.cpp
                                           combat/clinch_relay.cpp combat/sync_attacks.cpp
                                           combat/attack_observers.cpp
                                           replicas/replica_fight.cpp body_state/body_snapshot.cpp
                                           hud/combat_hud_hooks.cpp input/input_mode.cpp
                                           combat/player_damage.cpp combat/native_effects.cpp
                                           combat/combat_identity.cpp combat/defence.cpp
                                           combat/combat_input.cpp combat/melee_contacts.cpp
                                           combat/targetable.cpp combat/takedowns.cpp
                                           combat/women_targets.cpp
                                           combat/held_hits.cpp combat/held_harm.cpp
                                           combat/native_actions.cpp
                                           combat/attack_factory.cpp
                                           animation/animation_capture.cpp
                                           replicas/replica_spawn.cpp replicas/remote_ui.cpp
                                           replicas/remote_interactor_lifetime.cpp
                                           replicas/character_retirement.cpp characters/body_collisions.cpp characters/rested_updates.cpp characters/remote_target_scan.cpp characters/skinning_capacity.cpp
                                           characters/skinning_metrics.cpp
                                           characters/frame_metrics.cpp
                                           characters/material_capacity.cpp
                                           characters/pipeline_metrics.cpp
                                           replicas/distance_lod.cpp
                                           session/world_reset.cpp
                                           testing/combat_tests.cpp combat/keep_lock.cpp
                                           replicas/remote_lifecycle.cpp core/engine_loop.cpp
                                           session/network_session.cpp session/resource_files.cpp session/resource_cache.cpp
                                           session/process_hooks.cpp core/client_state.cpp
                                           combat/combat_relay_tick.cpp replicas/replica_tick.cpp
                                           movement/ladders_tick.cpp camera/photo_mode_tick.cpp
                                           animation/animation_orders_tick.cpp
                                           pillory/pillory_tick.cpp doors/doors_tick.cpp
                                           graves/graves_tick.cpp
                                           smart_objects/smart_objects_tick.cpp
                                           stash/body_containers.cpp
                                           knockout/dropped_weapons_tick.cpp
                                           local_player/local_state_tick.cpp
                                           session/world_start.cpp world_map/map_reveal.cpp
                                           body_state/body_state_install.cpp
                                           animation/player_animation_install.cpp
                                           hud/resource_hud_install.cpp
                                           action_hints/action_hints_install.cpp
                                           world_map/world_map_install.cpp
                                           character_creator/creator_install.cpp
                                           replicas/replica_role_install.cpp
                                           seats/seats_install.cpp weapons/weapons_install.cpp
                                           noclip/noclip_install.cpp book/book_install.cpp
                                           keybinds/keybinds_install.cpp
                                           stash/stash_install.cpp vendors/vendors_install.cpp
                                           world/world_clock_install.cpp
                                           winter/snow_stage.cpp
                                           winter/shader_compile.cpp
                                           markers/markers_install.cpp quests/quests_install.cpp
                                           inventory/inventory_install.cpp
                                           appearance/appearance_install.cpp
                                           ground/ground_install.cpp hud/notices_install.cpp
                                           carry/body_carry_install.cpp
                                           replicas/replica_guards_install.cpp
                                           ranged/aim_install.cpp
                                           pickpocket/pickpocket_install.cpp
                                           physics/physics_install.cpp
                                           native_ui/native_ui_install.cpp
                                           props/props_install.cpp
                                           crafting/crafting_install.cpp
                                           gizmos/gizmos_install.cpp
                                           nameplates/nameplates_install.cpp
                                           combat/combat_natives_install.cpp
                                           riding/rider_install.cpp
                                           session/session_guards_install.cpp
                                           knockout/knockout_install.cpp
                                           progression/progression_install.cpp
                                           movement/movement_install.cpp
                                           world/single_player_screens_install.cpp
                                           stats/stats_install.cpp core/engine_natives.cpp
                                           core/startup_targets.cpp
                                           animation/animation_targets.cpp
                                           replicas/replica_targets.cpp
                                           combat/combat_targets.cpp look/look_install.cpp
                                           input/input_install.cpp chat/chat_install.cpp
                                           core/game_module.cpp core/signature_store.cpp core/hook_batch.cpp session/early_hooks.cpp
                                           game_mods/game_mods.cpp
                                           hud/hud_checks.cpp core/frame.cpp)
  add_dependencies(kcd2s-game-client kcd2s-resource-runtime)
  # MH_EnableHook joins the start's batch in all of the game client's own sources (hook_batch.hpp).
  target_compile_options(kcd2s-game-client PRIVATE
                         /FI${CMAKE_CURRENT_SOURCE_DIR}/core/hook_batch.hpp)
  set_target_properties(kcd2s-game-client PROPERTIES OUTPUT_NAME kcd2s-game-client-v4)
  target_link_libraries(kcd2s-game-client PRIVATE kcds::voice_engine)
  include(${CMAKE_SOURCE_DIR}/Client/voice/steamaudio.cmake)
  add_custom_command(TARGET kcd2s-game-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KCDS_STEAMAUDIO_DLL}"
            "$<TARGET_FILE_DIR:kcd2s-game-client>/phonon.dll")
  target_link_libraries(kcd2s-game-client PRIVATE kcd2s::integration kcd2s_runtime_minhook ws2_32 psapi d3d11 d3d12 dxgi d2d1 dwrite
                        windowscodecs shell32 ole32)
  # In-game server browser: native main-menu hooks, the in-frame page compositor and
  # the page protocol over the master server list.
  target_link_libraries(kcd2s-game-client PRIVATE kcds::menu_native kcds::menu_compositor
                        kcd2s::masterclient kcd2s::wire version)
  # The server browser page is loaded from ui/menu next to the DLL.
  add_custom_command(TARGET kcd2s-game-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/Client/ui/menu"
            "$<TARGET_FILE_DIR:kcd2s-game-client>/ui/menu")
  # Native crash reporting (crash/): settings handed over by the launcher; the
  # backend and crashpad_handler.exe only with KCDS_ENABLE_SENTRY.
  target_sources(kcd2s-game-client PRIVATE crash/crash_reporting.cpp crash/crash_context.cpp)
  # In-game bug reports (bugreport/): the ESC menu's Report a bug, signed, to the KCDS Discord bot.
  target_sources(kcd2s-game-client PRIVATE bugreport/bug_report.cpp)
  # The game's videos (video/): no frame skipping, whose catch-up wait could hang the game.
  target_sources(kcd2s-game-client PRIVATE video/bink_skip.cpp)
  # Runtime branding (branding/): our asset pak and startup settings, applied in memory only in a
  # process started by our launcher; the game installation is never written to.
  target_sources(kcd2s-game-client PRIVATE branding/runtime_branding.cpp branding/splash.cpp
                                           branding/watermark.cpp branding/window_icon.cpp
                                           branding/editor_chrome.cpp
                                           branding/loading_screen.cpp
                                           branding/loading_tail.cpp
                                           branding/assets/kcds-game-client.rc)
  target_link_libraries(kcd2s-game-client PRIVATE gdiplus windowscodecs ole32 crypt32)
  # The watermark's mark is built into the client: its PNG becomes a byte list.
  set(KCDS_WATERMARK_MARK "${CMAKE_CURRENT_SOURCE_DIR}/branding/assets/kcds-watermark-mark.png")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KCDS_WATERMARK_MARK}")
  file(READ "${KCDS_WATERMARK_MARK}" kcds_watermark_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," kcds_watermark_bytes "${kcds_watermark_hex}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/kcds_watermark_mark.inc"
       "${kcds_watermark_bytes}\n")
  target_include_directories(kcd2s-game-client PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  # World Architect's MCP server for AI assistants (architect/ai_bridge.hpp), next to the DLL: the
  # editor's AI tab shows its path.
  add_custom_command(TARGET kcd2s-game-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/Dev/tools/architect-mcp"
            "$<TARGET_FILE_DIR:kcd2s-game-client>/architect-mcp")
  # The start splash overlay, loaded from assets/ next to the DLL.
  add_custom_command(TARGET kcd2s-game-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/branding/assets/kcds-splash-overlay.png"
            "$<TARGET_FILE_DIR:kcd2s-game-client>/assets/kcds-splash-overlay.png")
  # Several game processes side by side (instances/): the exe's single-instance check is bypassed
  # in memory only, never in KingdomCome.exe on disk.
  target_sources(kcd2s-game-client PRIVATE instances/multiple_instances.cpp)
  # Resource web views (web): the client resources' Web global on CEF. The CEF
  # runtime and kcds-web-helper.exe are copied next to the DLL.
  target_sources(kcd2s-game-client PRIVATE web/resource_web.cpp web/icons/item_icons.cpp
    web/icons/dds_picture.cpp web/icons/png_file.cpp)
  # Key bindings and Controls of client resources (input); scripted game input for
  # live tests (KCDS_TEST_HOOKS only).
  target_sources(kcd2s-game-client PRIVATE input/resource_input.cpp
                                           input/bow_input_probe.cpp
                                           input/scripted_input.cpp
                                           input/focus_release.cpp
                                           input/cuffs.cpp
                                           input/frozen.cpp
                                           input/interaction_rules.cpp
                                           lobby/lobby.cpp)
  # The game's HUD for client resources (hud).
    target_sources(kcd2s-game-client PRIVATE hud/engine_script.cpp hud/resource_hud.cpp
                                           hud/monologue.cpp hud/story_cutscenes.cpp
                                           hud/character_stats/character_stats.cpp)
  # The server's conversations in the game's own dialogue list (dialogue).
  target_sources(kcd2s-game-client PRIVATE dialogue/dialogue_list.cpp dialogue/game_dialogs.cpp)
  # The server's vendors on the game's own shop screen and its haggle (vendors/, and
  # 0.4.1).
  target_sources(kcd2s-game-client PRIVATE vendors/vendor_screen.cpp vendors/vendor_haggle.cpp)
  # The Chat global and the reserved chat events of client resources (chat).
  target_sources(kcd2s-game-client PRIVATE chat/resource_chat.cpp)
  # ActionHint of client resources (action_hints).
  target_sources(kcd2s-game-client PRIVATE action_hints/resource_action_hints.cpp
                                           action_hints/hint_width.cpp
                                           action_hints/hint_width_movie.cpp)
  # Robbing another player with the game's own pickpocketing (pickpocket).
  target_sources(kcd2s-game-client PRIVATE pickpocket/pickpocket.cpp)
  # Whether solid geometry lies between two points, with the game's own ray test (physics/).
  target_sources(kcd2s-game-client PRIVATE physics/line_of_sight.cpp)
  # The rays the server asks this game to trace (physics).
  target_sources(kcd2s-game-client PRIVATE physics/world_raycast.cpp)
  # The client scripts' own rays, spheres and camera pose (physics).
  target_sources(kcd2s-game-client PRIVATE physics/local_world.cpp)
  # The use key on a server NPC (npcs).
  target_sources(kcd2s-game-client PRIVATE npcs/npc_use.cpp)
  # The client scripts' own prop previews (props).
  target_sources(kcd2s-game-client PRIVATE props/local_previews.cpp)
  # The tools the game puts in the local player's hands, for everyone (animation).
  target_sources(kcd2s-game-client PRIVATE animation/hand_props.cpp)
  target_sources(kcd2s-game-client PRIVATE lockpicking/lockpicking.cpp)
  # WorldMap, Compass and Travel of client resources (world_map).
  target_sources(kcd2s-game-client PRIVATE world_map/resource_world_map.cpp
                                           world_map/map_discover.cpp
                                           world_map/travel_check.cpp)
  # Book of client resources (book).
  target_sources(kcd2s-game-client PRIVATE book/resource_book.cpp)
  # Held props of remote players (props).
  target_sources(kcd2s-game-client PRIVATE props/held_props.cpp props/prop_entity.cpp)
  # Appearance of players: worn clothing and character components (appearance).
  target_sources(kcd2s-game-client PRIVATE appearance/brand.cpp appearance/components.cpp appearance/female_skeleton.cpp
                                           appearance/player_skeleton.cpp
                                           appearance/disguise.cpp
                                           appearance/script_gender.cpp
                                           appearance/first_person_head.cpp
                                           appearance/grime.cpp appearance/outfit.cpp
                                           appearance/unisex_clothing.cpp
                                           appearance/visibility.cpp appearance/worn_only.cpp)
  # Replicas leave the player role to the local player (replicas), stay out of the AI
  # LOD update and go without the game's combat queueing animations into them.
  target_sources(kcd2s-game-client PRIVATE replicas/local_player_role.cpp replicas/lod_guard.cpp
                                           replicas/removal_guard.cpp)
  # A seat a player sits on is taken on every client (seats).
  target_sources(kcd2s-game-client PRIVATE seats/seat_reservations.cpp)
  # In a bed the player stays lying until Get up (beds).
  target_sources(kcd2s-game-client PRIVATE beds/stay_in_bed.cpp)
  # Seated players do not push or block each other's bodies.
  target_sources(kcd2s-game-client PRIVATE seats/seat_collisions.cpp)
  # Weapons drawn on replicas as by their owners (weapons).
  target_sources(kcd2s-game-client PRIVATE weapons/drawn_weapons.cpp)
  # Portable identity of the observed human overlay assets (animation).
  target_sources(kcd2s-game-client PRIVATE animation/human_overlay.cpp)
  # The server's playAnimation on the local player's body, by the game's Mannequin.
  target_sources(kcd2s-game-client PRIVATE animation/player_animation.cpp
                                           animation/animation_props.cpp
                                           animation/upper_body.cpp
                                           animation/no_gravity.cpp
                                           animation/pointing.cpp
                                           animation/female_retarget.cpp
                                           animation/paired_start.cpp)
  # Shots with bows and crossbows seen by everyone (ranged).
  target_sources(kcd2s-game-client PRIVATE ranged/shots.cpp ranged/landings.cpp ranged/aim.cpp)
  # The local player's swing meeting something solid that is not a body (weapon_hits).
  target_sources(kcd2s-game-client PRIVATE weapon_hits/weapon_hits.cpp
                                           weapon_hits/weapon_hits_tick.cpp)
  # The name the local player connects under (identity).
  target_sources(kcd2s-game-client PRIVATE identity/nickname.cpp identity/licence_pass.cpp identity/discord_login.cpp
                                         identity/epic_licence.cpp identity/xbox_licence.cpp
                                         identity/hardware_id.cpp identity/sha256.cpp)
  target_link_libraries(kcd2s-game-client PRIVATE bcrypt)
  # Optional official Microsoft GDK headers; no static GDK imports. The provider
  # calls only the runtime already loaded by a Microsoft Store game. Non-GDK
  # builds explicitly report Xbox proof unavailable.
  set(KCDS_GDK_INCLUDE_DIR "" CACHE PATH "Official Microsoft GDK Windows include directory")
  if(KCDS_GDK_INCLUDE_DIR)
    if(NOT EXISTS "${KCDS_GDK_INCLUDE_DIR}/XStore.h")
      message(FATAL_ERROR "KCDS_GDK_INCLUDE_DIR must contain the official XStore.h")
    endif()
    target_include_directories(kcd2s-game-client PRIVATE "${KCDS_GDK_INCLUDE_DIR}")
    target_compile_definitions(kcd2s-game-client PRIVATE KCDS_WITH_GDK_PROOF=1)
  endif()
  # The local player's stats and the server's setStat (stats).
  target_sources(kcd2s-game-client PRIVATE stats/player_stats.cpp)
  # Native damage reports retain receipt order while projectile verdicts are pending.
  target_sources(kcd2s-game-client PRIVATE combat/damage_reports.cpp)
  # The game's own price of another player's blow and its re-price for a ruling.
  target_sources(kcd2s-game-client PRIVATE combat/priced_hits.cpp)
  target_sources(kcd2s-game-client PRIVATE stats/player_commands.cpp)
  # The game's sleep and food needs turned off by the server (needs).
  target_sources(kcd2s-game-client PRIVATE needs/needs.cpp)
  # The pace rule a server puts on the local player (movement).
  target_sources(kcd2s-game-client PRIVATE movement/movement_mode.cpp movement/walk_default.cpp
                                           movement/shared_ladders.cpp)
  # How the players' bodies stand and move: posture and Mannequin tags (body_state).
  target_sources(kcd2s-game-client PRIVATE body_state/body_readings.cpp)
  # The local player's progression, reported to the server (progression).
  target_sources(kcd2s-game-client PRIVATE progression/progression_report.cpp
                                           progression/progression_orders.cpp
                                           progression/xp_gate.cpp
                                           progression/perk_gate.cpp
                                           progression/replica_soul.cpp)
  # The status effects on the local player's body (buffs) and the owners' fighting
  # buffs on their replicas, the resources' own buffs and the server's
  # buffs on horses.
  target_sources(kcd2s-game-client PRIVATE buffs/player_buffs.cpp buffs/replica_buffs.cpp
                                           buffs/custom_buffs.cpp buffs/horse_buffs.cpp)
  # The game's NPCs kept out until they are synced (npcs/, 0.2.0).
  target_sources(kcd2s-game-client PRIVATE npcs/npc_suppression.cpp npcs/npc_body.cpp
                                         npcs/npc_speech.cpp npcs/local_npcs.cpp
                                         npcs/npc_health.cpp npcs/npc_walk.cpp npcs/real_movement.cpp
                                         npcs/carcass_prompts.cpp
                                         npcs/npc_attack.cpp npcs/npc_barks.cpp)
  # Resource actions in the game's key binding settings, kept per server (keybinds).
  target_sources(kcd2s-game-client PRIVATE keybinds/keybind_screen.cpp keybinds/keybind_store.cpp
                                           keybinds/keybinds.cpp)
  # NoClip's free camera (noclip).
  target_sources(kcd2s-game-client PRIVATE camera/anchor_view.cpp camera/photo_mode.cpp noclip/free_camera.cpp)
  # The game's third-person camera on F5, under the server's ThirdPerson mode (camera).
  target_sources(kcd2s-game-client PRIVATE camera/third_person.cpp)
  # The server's scripted camera shots (camera).
  target_sources(kcd2s-game-client PRIVATE camera/camera_shot.cpp)
  target_sources(kcd2s-game-client PRIVATE camera/view_shake.cpp)
  # Free look: the head and camera turn, the body does not (camera).
  target_sources(kcd2s-game-client PRIVATE camera/free_look.cpp)
  # Render.view: a picture from any camera, the player's view kept (render).
  target_sources(kcd2s-game-client PRIVATE render/render_view.cpp)
  # The game's render-resource release lists: no hang when a map unloads (render).
  target_sources(kcd2s-game-client PRIVATE render/release_flush_guard.cpp)
  # A player in the game's pillory, put there and taken out by the server (pillory).
  target_sources(kcd2s-game-client PRIVATE pillory/pillory.cpp)
  # The level's doors, kept in step with the server (doors).
  target_sources(kcd2s-game-client PRIVATE doors/doors.cpp)
  # The level's graves, dug out and filled in step with the server (graves).
  target_sources(kcd2s-game-client PRIVATE graves/graves.cpp)
  # The smart objects scripts place, the game's prefabs built natively (smart_objects).
  target_sources(kcd2s-game-client PRIVATE smart_objects/placed_smart_objects.cpp)
  # The server's level edits: level entities and brushes hidden or moved (level).
  target_sources(kcd2s-game-client PRIVATE level/level_edits.cpp level/story_layers.cpp
                                           level/layer_catalog.cpp)
  # The level's animated gates, moved as the server has them (gates).
  target_sources(kcd2s-game-client PRIVATE gates/gates.cpp gates/series.cpp)
  # The game's tutorial hint with a resource's content (hud).
  target_sources(kcd2s-game-client PRIVATE hud/tutorial_hint.cpp)
  # The game's notice of items the server gave (hud).
  target_sources(kcd2s-game-client PRIVATE hud/item_notice.cpp hud/level_up_notice.cpp
                                         hud/game_texts.cpp)
  # The game's speech bubbles over a head (hud/, Npc.say).
  target_sources(kcd2s-game-client PRIVATE hud/speech_bubbles.cpp)
  # The resources' interactions on things in the world (interactions).
  target_sources(kcd2s-game-client PRIVATE interactions/world_interactions.cpp
                                           interactions/talk_prompts.cpp
                                           interactions/station_prompts.cpp
                                           riding/whistle.cpp)
  # The server's siege engines: their props, cycle, use and shots (siege).
  target_sources(kcd2s-game-client PRIVATE siege/siege_engines.cpp)
  # The server's siege ladders and stone piles (siege).
  target_sources(kcd2s-game-client PRIVATE siege/siege_ladders.cpp siege/stone_piles.cpp)
  # The CharacterCreator's stage (character_creator).
  target_sources(kcd2s-game-client PRIVATE character_creator/creator_stage.cpp)
  # NativeUI of client resources (native_ui).
  target_sources(kcd2s-game-client PRIVATE native_ui/native_ui.cpp files/stored_zip.cpp)
  target_link_libraries(kcd2s-game-client PRIVATE kcds::web)
  add_dependencies(kcd2s-game-client kcds-web-helper)
  kcds_ship_cef(kcd2s-game-client)
  add_custom_command(TARGET kcd2s-game-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:kcds-web-helper>"
            "$<TARGET_FILE_DIR:kcd2s-game-client>")
  # Knock-out instead of death in a server world (knockout/): the local player is knocked out by
  # a lethal hit and stays down until a resource revives it. The game's exhaustion
  # faint is told apart from it, and replicas never faint by their own games.
  target_sources(kcd2s-game-client PRIVATE knockout/knockout.cpp knockout/body_state.cpp
                                           knockout/game_over.cpp
                                           knockout/dropped_weapons.cpp knockout/faint.cpp
                                           knockout/replica_faint.cpp)
  # Players fighting players call no guards (crime): a fight whose participants are
  # all players files no crime report.
  target_sources(kcd2s-game-client PRIVATE crime/pvp_crime.cpp)
  # A rider stays seated on its horse (riding/): the posture state keeps the horse body position
  # while the actor is mounted, and a rider's horse reservation survives the horse going out of
  # the game's reach. A horse the server gives the local player is its own horse.
  target_sources(kcd2s-game-client PRIVATE riding/seated_body.cpp riding/horse_reservation.cpp
                                           riding/player_horse.cpp riding/horse_capacity.cpp
                                           riding/horse_inspect.cpp riding/horse_saddlebags.cpp
                                          riding/horse_tack.cpp)
  # The inventory the server holds, shown by the game's own (inventory).
  target_sources(kcd2s-game-client PRIVATE inventory/stock_projection.cpp inventory/item_names.cpp inventory/item_types.cpp
                                           inventory/item_overrides.cpp inventory/equipped_weight.cpp
                                           inventory/max_column.cpp inventory/row_names.cpp
                                           inventory/max_column_movie.cpp
                                           inventory/inventory_use.cpp inventory/quickslots.cpp
                                           inventory/kettles.cpp
                                           ground/ground_items.cpp stash/stashes.cpp)
  # The world clock the server sets (world).
  target_sources(kcd2s-game-client PRIVATE world/world_clock.cpp world/world_weather.cpp
                                           session/load_progress.cpp
                                           world/trespass.cpp world/interior.cpp
                                           world/navmesh_holes.cpp)
  # The local player attached to another (attach).
  target_sources(kcd2s-game-client PRIVATE attach/player_attach.cpp)
  # The markers and blips the server shows (markers).
  target_sources(kcd2s-game-client PRIVATE markers/world_markers.cpp markers/marker_shapes.cpp
                                           markers/blips.cpp markers/map_quest_marks.cpp
                                           markers/local_decals.cpp)
  # The quests the server writes into the journal (quests).
  target_sources(kcd2s-game-client PRIVATE quests/quests.cpp quests/journal.cpp)
  target_sources(kcd2s-game-client PRIVATE crafting/recipe_tables.cpp)
  # A server's own perks, read with the game's tables at its next start (startup_tables).
  target_sources(kcd2s-game-client PRIVATE startup_tables/startup_tables.cpp)
  target_sources(kcd2s-game-client PRIVATE crafting/smithing.cpp crafting/alchemy.cpp
                                             crafting/station_clips.cpp
                                             crafting/emitters.cpp crafting/sounds.cpp
                                             crafting/grinding.cpp
                                             crafting/permits.cpp preparing/preparing.cpp)
  # Dice matches between two players at the game's own dice table (dice).
  target_sources(kcd2s-game-client PRIVATE dice/dice_table.cpp dice/dice_tick.cpp
                                             dice/dice_install.cpp dice/dice_badges.cpp
                                             dice/dice_sit.cpp)
  # Herb gathering through the server (gathering).
  target_sources(kcd2s-game-client PRIVATE gathering/herbs.cpp gathering/gathering_tick.cpp
                                             gathering/gathering_install.cpp)
  # How the stew pots look, as the server keeps them (cookpot).
  target_sources(kcd2s-game-client PRIVATE cookpot/pot_looks.cpp cookpot/cookpot_tick.cpp
                                             cookpot/cookpot_install.cpp
                                             cookpot/kettle_state.cpp)
  # The props and particle effects the server places (props/, vfx).
  target_sources(kcd2s-game-client PRIVATE props/placed_props.cpp props/prop_physics.cpp
                                           vfx/placed_effects.cpp
                                           vfx/local_effects.cpp)
  # The point lights the server and the client scripts place (lights).
  target_sources(kcd2s-game-client PRIVATE lights/lights.cpp lights/lights_tick.cpp)
  # The carts and wagons the server places (carts).
  target_sources(kcd2s-game-client PRIVATE carts/placed_carts.cpp carts/cart_horses.cpp)
  # The player's prop previews and placement, and the server's gizmos (props/, gizmos/, issues).
  target_sources(kcd2s-game-client PRIVATE props/ghosts.cpp props/placer.cpp gizmos/gizmos.cpp)
  target_sources(kcd2s-game-client PRIVATE nameplates/nameplates.cpp nameplates/nameplate_paint.cpp
                                           nameplates/presented_frame.cpp hud/hud_text.cpp)
  target_sources(kcd2s-game-client PRIVATE discord/discord_presence.cpp)
  target_sources(kcd2s-game-client PRIVATE audio/resource_audio.cpp audio/audio_install.cpp
                                          audio/voice_files.cpp)
  target_sources(kcd2s-game-client PRIVATE follow/follow.cpp follow/follow_install.cpp)
  # Items carried in the arms (carry).
  target_sources(kcd2s-game-client PRIVATE carry/carry_items.cpp carry/body_carry.cpp)
  target_link_libraries(kcd2s-game-client PRIVATE kcds::launcher)
  target_compile_definitions(kcd2s-game-client PRIVATE
    KCDS_GAME_CLIENT_VERSION="${PROJECT_VERSION}")
  if(TARGET kcds_launcher_sentry)
    target_link_libraries(kcd2s-game-client PRIVATE kcds::launcher_sentry)
    kcds_ship_crash_handler(kcd2s-game-client)
  endif()
endif()
