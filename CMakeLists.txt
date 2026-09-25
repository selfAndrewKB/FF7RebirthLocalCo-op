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

    src/control/command_channel.cpp
    src/control/mod_target.cpp
    src/control/runner.cpp
    src/control/script.cpp

    src/hud/hud_placement.cpp
    src/hud/overlay_band.cpp
    src/hud/overlay_layout.cpp
    src/hud/viewport_placement.cpp
    src/hud/position_probe.cpp
    src/hud/widget_dump.cpp
    src/hud/layout_scale.cpp

    src/platform/call_redirect.cpp
    src/platform/code_redirect.cpp
    src/platform/image.cpp
    src/platform/log.cpp
    src/platform/module_pin.cpp
    src/platform/near_code.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp

    src/player_two/describe.cpp
    src/player_two/field_camera.cpp
    src/player_two/generation.cpp
    src/player_two/leave_sequence.cpp
    src/player_two/leftover_actors.cpp
    src/player_two/member_movement.cpp
    src/player_two/own_controller.cpp
    src/player_two/party_lease.cpp
    src/player_two/party_member.cpp
    src/player_two/primary_controller.cpp
    src/player_two/move_state.cpp
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
