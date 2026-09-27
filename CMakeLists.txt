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
    src/hud/player_two_pass.cpp
    src/hud/prompt_rows.cpp
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
    src/platform/guarded.cpp
    src/platform/frame_watch.cpp
    src/platform/image.cpp
    src/platform/log.cpp
    src/platform/module_folder.cpp
    src/platform/module_pin.cpp
    src/platform/near_code.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp

    src/player_two/describe.cpp
    src/player_two/field_camera.cpp
    src/player_two/field_exit.cpp
    src/player_two/generation.cpp
    src/player_two/leave_sequence.cpp
    src/player_two/leftover_actors.cpp
    src/player_two/member_movement.cpp
    src/player_two/battle_input.cpp
    src/player_two/battle_camera.cpp
    src/player_two/camera_override.cpp
    src/player_two/command_rules.cpp
    src/player_two/menu_focus.cpp
    src/player_two/focus_scope.cpp
    src/player_two/command_menu_two.cpp
    src/player_two/command_menu_tick.cpp
    src/player_two/leader_scope.cpp
    src/player_two/lock_on.cpp
    src/player_two/target_cursor.cpp
    src/player_two/own_controller.cpp
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

    src/players/join_keys.cpp
    src/players/local_players.cpp
    src/players/split_layout.cpp
    src/players/split_screen_settings.cpp
    src/players/unreal_access.cpp

    src/renderer/additional_family_output.cpp
    src/renderer/one_family_per_view.cpp
    src/renderer/view_family_submission.cpp

    src/views/player_view_point.cpp
    src/views/view_shape.cpp
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
        src/dev/terrain_watch.cpp
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
    src/platform/near_code.cpp
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
    src/platform/near_code.cpp
)
target_include_directories(CallRedirectTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CallRedirectTests PRIVATE cxx_std_20)
add_test(NAME call_redirect COMMAND CallRedirectTests)

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

add_executable(FirstPlayerCodeTests
    tests/first_player_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/near_code.cpp
)
target_include_directories(FirstPlayerCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FirstPlayerCodeTests PRIVATE cxx_std_20)
add_test(NAME first_player_code COMMAND FirstPlayerCodeTests)

add_executable(EnemyGaugesCodeTests
    tests/enemy_gauges_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/near_code.cpp
)
target_include_directories(EnemyGaugesCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(EnemyGaugesCodeTests PRIVATE cxx_std_20)
add_test(NAME enemy_gauges_code COMMAND EnemyGaugesCodeTests)

add_executable(CommandRulesCodeTests
    tests/command_rules_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/near_code.cpp
)
target_include_directories(CommandRulesCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CommandRulesCodeTests PRIVATE cxx_std_20)
add_test(NAME command_rules_code COMMAND CommandRulesCodeTests)

add_executable(CenteredFrameCodeTests
    tests/centered_frame_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/near_code.cpp
)
target_include_directories(CenteredFrameCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(CenteredFrameCodeTests PRIVATE cxx_std_20)
add_test(NAME centered_frame_code COMMAND CenteredFrameCodeTests)

add_executable(PopupsCodeTests
    tests/popups_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/near_code.cpp
)
target_include_directories(PopupsCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(PopupsCodeTests PRIVATE cxx_std_20)
add_test(NAME popups_code COMMAND PopupsCodeTests)

add_executable(ArrowEdgeCodeTests
    tests/arrow_edge_code_test.cpp
    src/platform/code_redirect.cpp
    src/platform/near_code.cpp
)
target_include_directories(ArrowEdgeCodeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ArrowEdgeCodeTests PRIVATE cxx_std_20)
add_test(NAME arrow_edge_code COMMAND ArrowEdgeCodeTests)




add_executable(ButtonViewTests
    tests/button_view_test.cpp
)
target_include_directories(ButtonViewTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ButtonViewTests PRIVATE cxx_std_20)
add_test(NAME button_view COMMAND ButtonViewTests)
