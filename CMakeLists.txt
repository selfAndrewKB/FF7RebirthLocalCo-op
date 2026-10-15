cmake_minimum_required(VERSION 3.18)

# LocalSplitScreen — two-player split-screen for FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.
#
# Design:   Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md
# Rules:    Rebirth/Documentation/SPLITSCREEN-REBUILD-PHILOSOPHY.md
#
# This is a standalone CMake root. The one external dependency is RE-UE4SS, which provides the
# `UE4SS` target the mod links against. It is referenced where it already lives rather than copied.
project(LocalSplitScreen CXX)

set(UE4SS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../_tools/UE4SS_CppMods/RE-UE4SS"
    CACHE PATH "Path to the RE-UE4SS checkout that provides the UE4SS target")

if(NOT EXISTS "${UE4SS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "RE-UE4SS not found at UE4SS_ROOT='${UE4SS_ROOT}'. "
                        "Pass -DUE4SS_ROOT=<path to the RE-UE4SS checkout>.")
endif()

# The source lives outside this tree, so CMake needs an explicit build directory for it.
add_subdirectory("${UE4SS_ROOT}" "${CMAKE_BINARY_DIR}/RE-UE4SS")

# ------------------------------------------------------------------------------------------------
# The mod
# ------------------------------------------------------------------------------------------------
add_library(LocalSplitScreen SHARED
    src/mod.cpp

    src/hud/hud_placement.cpp
    src/hud/hud_roots.cpp
    src/hud/hud_updates.cpp
    src/hud/root_overlay_render.cpp
    src/hud/second_image.cpp
    src/hud/minimap_once.cpp
    src/hud/player_two_pass.cpp
    src/hud/prompt_rows.cpp
    src/hud/field_markers.cpp
    src/hud/action_icon_base.cpp
    src/hud/area_icons_two.cpp
    src/hud/target_views.cpp
    src/hud/widget_opacity.cpp
    src/hud/enemy_gauges.cpp
    src/hud/marker_timers.cpp
    src/hud/popups.cpp
    src/hud/user_widgets.cpp
    src/hud/overlay_band.cpp
    src/hud/overlay_layout.cpp
    src/hud/viewport_placement.cpp
    src/hud/layout_scale.cpp

    src/platform/call_redirect.cpp
    src/platform/code_redirect.cpp
    src/platform/entry_redirect.cpp
    src/platform/guarded.cpp
    src/platform/frame_watch.cpp
    src/platform/image.cpp
    src/platform/log.cpp
    src/platform/memory_watch.cpp
    src/platform/thread_times.cpp
    src/platform/binned_pools.cpp
    src/platform/build_translation.cpp
    src/platform/pool_bits.cpp
    src/platform/module_folder.cpp
    src/platform/module_pin.cpp
    src/platform/code_memory.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp

    src/player_two/describe.cpp
    src/player_two/field_camera.cpp
    src/player_two/field_exit.cpp
    src/player_two/generation.cpp
    src/player_two/leave_sequence.cpp
    src/player_two/leftover_actors.cpp
    src/player_two/member_movement.cpp
    src/player_two/nav_strand.cpp
    src/player_two/camera_repair.cpp
    src/player_two/ride_framing_watch.cpp
    src/player_two/climb_context.cpp
    src/player_two/near_fade.cpp
    src/player_two/rumble_list_two.cpp
    src/player_two/ffb_manager_two.cpp
    src/player_two/shared_ride_rumble.cpp
    src/player_two/audio_listener_two.cpp
    src/player_two/approach_two.cpp
    src/player_two/move_type_two.cpp
    src/player_two/anim_two_views.cpp
    src/player_two/traversal_camera.cpp
    src/player_two/traversal_watch.cpp
    src/player_two/trigger_two.cpp
    src/player_two/vault_camera.cpp
    src/player_two/warp_camera_watch.cpp
    src/player_two/framing_watch.cpp
    src/player_two/location_rows.cpp
    src/player_two/shoulder_scale.cpp
    src/player_two/walk_limits.cpp
    src/player_two/handover_watch.cpp
    src/player_two/field_pickup.cpp
    src/player_two/rumble_watch.cpp
    src/player_two/pad_rumble.cpp
    src/player_two/combat_rumble.cpp
    src/player_two/chest_watch.cpp
    src/player_two/vending.cpp
    src/player_two/interact_watch.cpp
    src/player_two/charapop_watch.cpp
    src/player_two/charapop_facts.cpp
    src/player_two/charapop_near.cpp
    src/player_two/charapop_centre.cpp
    src/player_two/world_actor_reach.cpp
    src/player_two/ride_watch.cpp
    src/player_two/chocobo_ride.cpp
    src/player_two/chocobo_climb.cpp
    src/player_two/shared_ride.cpp
    src/player_two/wheelie_ride.cpp
    src/player_two/wheelie_watch.cpp
    src/player_two/ride_probe.cpp
    src/hud/navimap_watch.cpp
    src/hud/minimap.cpp
    src/hud/minimap_labels.cpp
    src/hud/label_relayouts.cpp
    src/hud/minimap_icons.cpp
    src/hud/label_states.cpp
    src/player_two/party_warp.cpp
    src/player_two/strand_watch.cpp
    src/player_two/battle_input.cpp
    src/player_two/battle_camera.cpp
    src/player_two/camera_override.cpp
    src/player_two/camera_distance.cpp
    src/player_two/main_camera_shape.cpp
    src/player_two/command_locks.cpp
    src/player_two/battle_warp_two.cpp
    src/player_two/target_census.cpp
    src/player_two/equal_targeting.cpp
    src/player_two/player_hits.cpp
    src/player_two/lock_group_coin.cpp
    src/player_two/target_coin.cpp
    src/player_two/leave_watch.cpp
    src/player_two/command_rules.cpp
    src/player_two/menu_focus.cpp
    src/player_two/focus_scope.cpp
    src/player_two/command_menu_two.cpp
    src/player_two/command_menu_tick.cpp
    src/player_two/leader_scope.cpp
    src/player_two/lock_on.cpp
    src/player_two/own_controller.cpp
    src/player_two/party_events.cpp
    src/player_two/party_lease.cpp
    src/player_two/party_member.cpp
    src/player_two/party_switch.cpp
    src/player_two/primary_controller.cpp
    src/player_two/scope_leaks.cpp
    src/player_two/move_state.cpp
    src/player_two/pad_buttons.cpp
    src/player_two/pad_sticks.cpp
    src/player_two/session.cpp
    src/player_two/session_join.cpp
    src/player_two/stick_select.cpp
    src/player_two/swap_window.cpp
    src/player_two/tag_counts.cpp
    src/player_two/view_target.cpp
    src/player_two/widget_owner.cpp

    src/players/join_keys.cpp
    src/players/local_players.cpp
    src/players/single_view.cpp
    src/players/split_layout.cpp
    src/players/split_screen_settings.cpp
    src/players/unreal_access.cpp

    src/renderer/additional_family_output.cpp
    src/renderer/family_boundary.cpp
    src/renderer/me_feedback_once.cpp
    src/renderer/submit_hints.cpp
    src/renderer/submission_census.cpp
    src/renderer/async_compute.cpp
    src/renderer/before_device.cpp
    src/renderer/residency_off.cpp
    src/renderer/one_family_per_view.cpp
    src/renderer/view_family_submission.cpp

    src/views/player_view_point.cpp
    src/views/stream_view_math.cpp
    src/views/stream_views.cpp
    src/views/view_shape.cpp
    src/views/whole_screen.cpp
    src/views/whole_screen_reason.cpp
)

# Includes are rooted at src/ so every #include names its module ("platform/log.h").
target_include_directories(LocalSplitScreen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LocalSplitScreen PRIVATE cxx_std_20)
target_link_libraries(LocalSplitScreen PUBLIC UE4SS)

# Development tools (src/dev/dev_tools.h): the control-script channel, the HUD trace, the position
# probe and the dump commands. Off for a player's build: -DLSS_DEV_TOOLS=OFF.
option(LSS_DEV_TOOLS "Build the development tools into the mod" ON)
if(LSS_DEV_TOOLS)
    target_sources(LocalSplitScreen PRIVATE
        src/dev/dev_tools.cpp
        src/control/command_channel.cpp
        src/control/mod_target.cpp
        src/control/runner.cpp
        src/control/script.cpp
        src/hud/hud_players_dump.cpp
        src/hud/hud_trace.cpp
        src/hud/position_probe.cpp
        src/hud/widget_dump.cpp
    )
    target_compile_definitions(LocalSplitScreen PRIVATE LSS_DEV_TOOLS=1)
else()
    target_compile_definitions(LocalSplitScreen PRIVATE LSS_DEV_TOOLS=0)
endif()

# ------------------------------------------------------------------------------------------------
# Host-side tests: the logic that needs neither the game nor UE4SS
# ------------------------------------------------------------------------------------------------
enable_testing()

add_executable(LocalSplitScreenTests
    tests/one_family_per_view_test.cpp
    src/renderer/one_family_per_view.cpp
)
target_include_directories(LocalSplitScreenTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LocalSplitScreenTests PRIVATE cxx_std_20)
add_test(NAME one_family_per_view COMMAND LocalSplitScreenTests)

add_executable(OutputLoadActionTests
    tests/output_load_action_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(OutputLoadActionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(OutputLoadActionTests PRIVATE cxx_std_20)
add_test(NAME output_load_action COMMAND OutputLoadActionTests)

add_executable(SplitScreenSettingsTests
    tests/split_screen_settings_test.cpp
    src/players/split_screen_settings.cpp
)
target_include_directories(SplitScreenSettingsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SplitScreenSettingsTests PRIVATE cxx_std_20)
add_test(NAME split_screen_settings COMMAND SplitScreenSettingsTests)

add_executable(ViewShapeTests
    tests/view_shape_test.cpp
    src/views/view_shape.cpp
)
target_include_directories(ViewShapeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ViewShapeTests PRIVATE cxx_std_20)
add_test(NAME view_shape COMMAND ViewShapeTests)

add_executable(SplitLayoutTests
    tests/split_layout_test.cpp
    src/players/split_layout.cpp
)
target_include_directories(SplitLayoutTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SplitLayoutTests PRIVATE cxx_std_20)
add_test(NAME split_layout COMMAND SplitLayoutTests)

add_executable(SingleViewTests
    tests/single_view_test.cpp
    src/players/single_view.cpp
)
target_include_directories(SingleViewTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SingleViewTests PRIVATE cxx_std_20)
add_test(NAME single_view COMMAND SingleViewTests)

add_executable(WholeScreenTests
    tests/whole_screen_test.cpp
    src/views/whole_screen_reason.cpp
)
target_include_directories(WholeScreenTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(WholeScreenTests PRIVATE cxx_std_20)
add_test(NAME whole_screen COMMAND WholeScreenTests)

add_executable(StreamViewMathTests
    tests/stream_view_math_test.cpp
    src/views/stream_view_math.cpp
)
target_include_directories(StreamViewMathTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StreamViewMathTests PRIVATE cxx_std_20)
add_test(NAME stream_view_math COMMAND StreamViewMathTests)

add_executable(PoolBitsTests
    tests/pool_bits_test.cpp
    src/platform/pool_bits.cpp
)
target_include_directories(PoolBitsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PoolBitsTests PRIVATE cxx_std_20)
add_test(NAME pool_bits COMMAND PoolBitsTests)

add_executable(StrandWatchTests
    tests/strand_watch_test.cpp
    src/player_two/strand_watch.cpp
)
target_include_directories(StrandWatchTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StrandWatchTests PRIVATE cxx_std_20)
add_test(NAME strand_watch COMMAND StrandWatchTests)

add_executable(SwapWindowTests
    tests/swap_window_test.cpp
    src/player_two/swap_window.cpp
)
target_include_directories(SwapWindowTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SwapWindowTests PRIVATE cxx_std_20)
add_test(NAME swap_window COMMAND SwapWindowTests)

add_executable(PartyLeaseTests
    tests/party_lease_test.cpp
    src/player_two/party_lease.cpp
)
target_include_directories(PartyLeaseTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PartyLeaseTests PRIVATE cxx_std_20)
add_test(NAME party_lease COMMAND PartyLeaseTests)

add_executable(LeaveSequenceTests
    tests/leave_sequence_test.cpp
    src/player_two/leave_sequence.cpp
)
target_include_directories(LeaveSequenceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LeaveSequenceTests PRIVATE cxx_std_20)
add_test(NAME leave_sequence COMMAND LeaveSequenceTests)

add_executable(ControlScriptTests
    tests/control_script_test.cpp
    src/control/script.cpp
)
target_include_directories(ControlScriptTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ControlScriptTests PRIVATE cxx_std_20)
add_test(NAME control_script COMMAND ControlScriptTests)

add_executable(ControlRunnerTests
    tests/control_runner_test.cpp
    src/control/runner.cpp
    src/control/script.cpp
)
target_include_directories(ControlRunnerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ControlRunnerTests PRIVATE cxx_std_20)
add_test(NAME control_runner COMMAND ControlRunnerTests)

add_executable(StickSelectTests
    tests/stick_select_test.cpp
    src/player_two/stick_select.cpp
)
target_include_directories(StickSelectTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(StickSelectTests PRIVATE cxx_std_20)
add_test(NAME stick_select COMMAND StickSelectTests)

add_executable(CallRedirectTests
    tests/call_redirect_test.cpp
    src/platform/call_redirect.cpp
    src/platform/module_pin.cpp
    src/platform/code_memory.cpp
)
target_include_directories(CallRedirectTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CallRedirectTests PRIVATE cxx_std_20)
add_test(NAME call_redirect COMMAND CallRedirectTests)

add_executable(EntryRedirectTests
    tests/entry_redirect_test.cpp
    src/platform/entry_redirect.cpp
    src/platform/module_pin.cpp
    src/platform/code_memory.cpp
)
target_include_directories(EntryRedirectTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(EntryRedirectTests PRIVATE cxx_std_20)
add_test(NAME entry_redirect COMMAND EntryRedirectTests)

add_executable(CodeMemoryTests
    tests/code_memory_test.cpp
    src/platform/code_memory.cpp
)
target_include_directories(CodeMemoryTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CodeMemoryTests PRIVATE cxx_std_20)
add_test(NAME code_memory COMMAND CodeMemoryTests)

add_executable(SparseArrayTests
    tests/sparse_array_test.cpp
    src/platform/guarded.cpp
)
target_include_directories(SparseArrayTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SparseArrayTests PRIVATE cxx_std_20)
add_test(NAME sparse_array COMMAND SparseArrayTests)

add_executable(OverlayBandTests
    tests/overlay_band_test.cpp
    src/hud/overlay_band.cpp
)
target_include_directories(OverlayBandTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(OverlayBandTests PRIVATE cxx_std_20)
add_test(NAME overlay_band COMMAND OverlayBandTests)

add_executable(ViewportPlacementTests
    tests/viewport_placement_test.cpp
    src/hud/viewport_placement.cpp
)
target_include_directories(ViewportPlacementTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ViewportPlacementTests PRIVATE cxx_std_20)
add_test(NAME viewport_placement COMMAND ViewportPlacementTests)

add_executable(EntryFilterCodeTests
    tests/entry_filter_code_test.cpp
)
target_include_directories(EntryFilterCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(EntryFilterCodeTests PRIVATE cxx_std_20)
add_test(NAME entry_filter_code COMMAND EntryFilterCodeTests)

add_executable(MinimapOnceCodeTests
    tests/minimap_once_code_test.cpp
)
target_include_directories(MinimapOnceCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(MinimapOnceCodeTests PRIVATE cxx_std_20)
add_test(NAME minimap_once_code COMMAND MinimapOnceCodeTests)

add_executable(FirstPlayerCodeTests
    tests/first_player_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(FirstPlayerCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FirstPlayerCodeTests PRIVATE cxx_std_20)
add_test(NAME first_player_code COMMAND FirstPlayerCodeTests)

add_executable(EnemyGaugesCodeTests
    tests/enemy_gauges_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(EnemyGaugesCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(EnemyGaugesCodeTests PRIVATE cxx_std_20)
add_test(NAME enemy_gauges_code COMMAND EnemyGaugesCodeTests)

add_executable(CommandRulesCodeTests
    tests/command_rules_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(CommandRulesCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CommandRulesCodeTests PRIVATE cxx_std_20)
add_test(NAME command_rules_code COMMAND CommandRulesCodeTests)

add_executable(PanelGateCodeTests
    tests/panel_gate_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(PanelGateCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PanelGateCodeTests PRIVATE cxx_std_20)
add_test(NAME panel_gate_code COMMAND PanelGateCodeTests)

add_executable(CenteredFrameCodeTests
    tests/centered_frame_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(CenteredFrameCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CenteredFrameCodeTests PRIVATE cxx_std_20)
add_test(NAME centered_frame_code COMMAND CenteredFrameCodeTests)

add_executable(PopupsCodeTests
    tests/popups_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(PopupsCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PopupsCodeTests PRIVATE cxx_std_20)
add_test(NAME popups_code COMMAND PopupsCodeTests)

add_executable(ArrowEdgeCodeTests
    tests/arrow_edge_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/code_memory.cpp
)
target_include_directories(ArrowEdgeCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ArrowEdgeCodeTests PRIVATE cxx_std_20)
add_test(NAME arrow_edge_code COMMAND ArrowEdgeCodeTests)

add_executable(FamilyBoundaryListTests
    tests/family_boundary_list_test.cpp
)
target_include_directories(FamilyBoundaryListTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FamilyBoundaryListTests PRIVATE cxx_std_20)
add_test(NAME family_boundary_list COMMAND FamilyBoundaryListTests)

add_executable(ButtonViewTests
    tests/button_view_test.cpp
)
target_include_directories(ButtonViewTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ButtonViewTests PRIVATE cxx_std_20)
add_test(NAME button_view COMMAND ButtonViewTests)

add_executable(SettledLogTests
    tests/settled_log_test.cpp
)
target_include_directories(SettledLogTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SettledLogTests PRIVATE cxx_std_20)
add_test(NAME settled_log COMMAND SettledLogTests)

add_executable(UcpcChoiceTests
    tests/ucpc_choice_test.cpp
)
target_include_directories(UcpcChoiceTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(UcpcChoiceTests PRIVATE cxx_std_20)
add_test(NAME ucpc_choice COMMAND UcpcChoiceTests)

add_executable(CharaPopRuleTests
    tests/charapop_rule_test.cpp
)
target_include_directories(CharaPopRuleTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CharaPopRuleTests PRIVATE cxx_std_20)
add_test(NAME charapop_rule COMMAND CharaPopRuleTests)

add_executable(LabelStatesTests
    tests/label_states_test.cpp
    src/hud/label_states.cpp
)
target_include_directories(LabelStatesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LabelStatesTests PRIVATE cxx_std_20)
add_test(NAME label_states COMMAND LabelStatesTests)

add_executable(BuildTranslationTests
    tests/build_translation_test.cpp
    src/platform/build_translation.cpp
)
target_include_directories(BuildTranslationTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(BuildTranslationTests PRIVATE cxx_std_20)
add_test(NAME build_translation COMMAND BuildTranslationTests)
