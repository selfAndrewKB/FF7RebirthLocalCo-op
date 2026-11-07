cmake_minimum_required(VERSION 3.18)

# LocalSplitScreen — two-player split-screen for FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.
#
# Design:   Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md
# Rules:    Rebirth/Documentation/SPLITSCREEN-REBUILD-PHILOSOPHY.md
#
# This is a standalone CMake root. The one external dependency is RE-UE4SS, which provides the
# `UE4SS` target the mod links against. It is referenced where it already lives rather than copied.
project(LocalSplitScreen CXX)

# The UE4SS build the mod is built against is the one players get from Nexus Mods: UE4SS v3.0.1 Beta, Git SHA
# c0a8d4c (upstream main of 2025-02-06), so a player installs the mod next to that loader and nothing else. That
# commit built with xmake; the checkout is a worktree of the RE-UE4SS clone at that commit with the upstream CMake
# build added back (tools/ue4ss_nexus/README.md makes it). The mod's imports are checked against the Nexus DLL's
# exports after each build (tools/ue4ss_nexus/check_exports.py).
set(UE4SS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../_tools/UE4SS_CppMods/RE-UE4SS-c0a8d4c"
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
    src/modules.cpp
    src/engine_tick.cpp

    src/hud/layout/hud_placement.cpp
    src/hud/hud_roots.cpp
    src/hud/hud_updates.cpp
    src/hud/passes/root_overlay_render.cpp
    src/hud/passes/column_hud_images.cpp
    src/hud/passes/shared_slot.cpp
    src/hud/minimap/minimap_once.cpp
    src/hud/passes/seat_hud_content.cpp
    src/hud/field/action_rows.cpp
    src/hud/passes/pass_opacity.cpp
    src/hud/field/prompt_rows.cpp
    src/hud/field/field_markers.cpp
    src/hud/field/search_pins.cpp
    src/hud/field/wrangle_targets.cpp
    src/hud/field/action_icon_base.cpp
    src/hud/minimap/seat_area_icons.cpp
    src/hud/battle/target_views.cpp
    src/hud/battle/enemy_gauges.cpp
    src/hud/field/marker_timers.cpp
    src/hud/battle/popups.cpp
    src/hud/user_widgets.cpp
    src/hud/layout/overlay_band.cpp
    src/hud/layout/overlay_layout.cpp
    src/hud/layout/viewport_placement.cpp
    src/hud/layout/layout_scale.cpp

    src/platform/block_patch.cpp
    src/platform/byte_scope.cpp
    src/platform/call_redirect.cpp
    src/platform/code_redirect.cpp
    src/platform/deferred_sites.cpp
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
    src/platform/settings.cpp
    src/platform/module_pin.cpp
    src/platform/code_memory.cpp
    src/platform/verified_code.cpp
    src/platform/vtable_entry.cpp

    src/players/describe.cpp
    src/camera/field_camera.cpp
    src/session/field_exit.cpp
    src/players/generation.cpp
    src/session/leave_sequence.cpp
    src/session/leftover_actors.cpp
    src/input/member_movement.cpp
    src/scopes/leader_answer.cpp
    src/world/nav_strand.cpp
    src/camera/camera_repair.cpp
    src/input/climb_context.cpp
    src/camera/near_fade.cpp
    src/feedback/traversal_rumble.cpp
    src/feedback/traversal_rumble_copy.cpp
    src/feedback/traversal_rumble_sources.cpp
    src/feedback/seat_ffb_managers.cpp
    src/feedback/sound_listeners.cpp
    src/input/approach_own_camera.cpp
    src/input/seat_walk_cap.cpp
    src/world/anim_views.cpp
    src/traversal/traversal_camera.cpp
    src/traversal/traversal_exec.cpp
    src/traversal/traversal_holders.cpp
    src/traversal/traversal_watch.cpp
    src/world/shop_talk.cpp
    src/world/seat_switch_volumes.cpp
    src/world/seat_triggers.cpp
    src/camera/vault_camera.cpp
    src/camera/location_rows.cpp
    src/camera/location_volumes.cpp
    src/camera/shoulder_scale.cpp
    src/input/walk_limits.cpp
    src/world/field_pickup.cpp
    src/feedback/pad_rumble.cpp
    src/feedback/combat_rumble.cpp
    src/world/vending.cpp
    src/world/charapop_facts.cpp
    src/world/charapop_near.cpp
    src/world/charapop_centre.cpp
    src/world/world_actor_reach.cpp
    src/rides/chocobo_ride.cpp
    src/traversal/chocobo_climb.cpp
    src/rides/shared_ride.cpp
    src/rides/wheelie_ride.cpp
    src/hud/minimap/minimap.cpp
    src/hud/minimap/minimap_labels.cpp
    src/hud/minimap/minimap_icons.cpp
    src/hud/minimap/label_states.cpp
    src/world/party_warp.cpp
    src/world/strand_watch.cpp
    src/battle/battle_camera.cpp
    src/camera/camera_override.cpp
    src/camera/attack_cameras.cpp
    src/camera/attack_framing.cpp
    src/battle/synergy_partner.cpp
    src/camera/seat_camera_effects.cpp
    src/camera/camera_access.cpp
    src/camera/camera_distance.cpp
    src/camera/main_camera_shape.cpp
    src/battle/command_locks.cpp
    src/battle/battle_warp_skip.cpp
    src/battle/equal_targeting.cpp
    src/battle/player_hits.cpp
    src/battle/leader_reads.cpp
    src/battle/lock_group_coin.cpp
    src/battle/target_coin.cpp
    src/session/leave_watch.cpp
    src/battle/command_rules.cpp
    src/battle/focus_scope.cpp
    src/battle/command_menu.cpp
    src/battle/command_menu_view.cpp
    src/battle/shortcut_follow.cpp
    src/battle/command_menu_tick.cpp
    src/battle/menu_input_route.cpp
    src/battle/menu_tint.cpp
    src/scopes/leader_scope.cpp
    src/battle/lock_on.cpp
    src/session/own_controller.cpp
    src/session/party_events.cpp
    src/session/party_lease.cpp
    src/facts/in_battle.cpp
    src/players/weak_object.cpp
    src/session/party_member.cpp
    src/session/party_switch.cpp
    src/scopes/primary_controller.cpp
    src/scopes/scope_leaks.cpp
    src/input/move_state.cpp
    src/input/pad_buttons.cpp
    src/input/pad_sticks.cpp
    src/session/outside_join.cpp
    src/rides/own_ride.cpp
    src/session/session.cpp
    src/session/session_join.cpp
    src/session/seat_out.cpp
    src/input/stick_select.cpp
    src/scopes/swap_window.cpp
    src/camera/view_target.cpp
    src/battle/widget_owner.cpp

    src/players/console_access.cpp
    src/players/held_setting.cpp
    src/players/join_keys.cpp
    src/players/local_players.cpp
    src/players/single_view.cpp
    src/players/split_layout.cpp
    src/players/split_screen_settings.cpp
    src/players/split_settings_access.cpp
    src/players/unreal_access.cpp
    src/players/widget_opacity.cpp

    src/renderer/additional_family_output.cpp
    src/renderer/family_boundary.cpp
    src/renderer/me_feedback_once.cpp
    src/renderer/submit_hints.cpp
    src/renderer/async_compute.cpp
    src/renderer/before_device.cpp
    src/renderer/residency_off.cpp
    src/renderer/one_family_per_view.cpp
    src/renderer/view_family_submission.cpp

    src/views/player_view_point.cpp
    src/views/stream_view_math.cpp
    src/views/stream_views.cpp
    src/views/display_align.cpp
    src/views/display_config.cpp
    src/views/display_move.cpp
    src/views/game_window.cpp
    src/views/mirror_player.cpp
    src/views/monitor_list.cpp
    src/views/monitor_picks.cpp
    src/views/monitor_row.cpp
    src/views/record_file.cpp
    src/views/span_desktop.cpp
    src/views/view_shape.cpp
    src/views/column_layout.cpp
    src/views/window_span.cpp
    src/views/whole_screen.cpp
    src/views/screen_widgets.cpp
    src/views/whole_screen_reason.cpp
    src/views/column_source.cpp
)

# Includes are rooted at src/ so every #include names its module ("platform/log.h"). src/ue4ss_compat maps the
# UE4SS header names the modules use (the current upstream layout) onto the c0a8d4c tree's.
target_include_directories(LocalSplitScreen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
                                                    "${CMAKE_CURRENT_SOURCE_DIR}/src/ue4ss_compat")
target_compile_features(LocalSplitScreen PRIVATE cxx_std_20)
target_link_libraries(LocalSplitScreen PUBLIC UE4SS)

# Development tools (src/dev/dev_tools.h): the console, the HUD trace, the position
# probe, the dump commands, and the watches that only log (installed and ticked by dev_tools.cpp; a
# header a shipped module includes carries empty stubs for the build without them). Off for a player's
# build: -DLSS_DEV_TOOLS=OFF.
option(LSS_DEV_TOOLS "Build the development tools into the mod" ON)
if(LSS_DEV_TOOLS)
    target_sources(LocalSplitScreen PRIVATE
        src/dev/dev_tools.cpp
        src/control/remote.cpp
        src/platform/command_registry.cpp
        src/control/runner.cpp
        src/control/script.cpp
        src/control/server.cpp
        src/control/stick_holds.cpp
        src/hud/hud_trace.cpp
        src/hud/position_probe.cpp
        src/hud/widget_dump.cpp
        src/hud/minimap/label_relayouts.cpp
        src/battle/battle_input.cpp
        src/world/charapop_watch.cpp
        src/camera/framing_watch.cpp
        src/session/handover_watch.cpp
        src/world/interact_watch.cpp
        src/battle/menu_focus.cpp
        src/input/move_census.cpp
        src/rides/ride_watch.cpp
        src/feedback/rumble_watch.cpp
        src/session/tag_counts.cpp
        src/battle/target_census.cpp
        src/traversal/traversal_collision.cpp
        src/rides/wheelie_watch.cpp
        src/renderer/submission_census.cpp
    )
    target_compile_definitions(LocalSplitScreen PRIVATE LSS_DEV_TOOLS=1)
else()
    target_compile_definitions(LocalSplitScreen PRIVATE LSS_DEV_TOOLS=0)
endif()

# ------------------------------------------------------------------------------------------------
# The setup app: picks the monitors and writes LocalSplitScreen.ini; shares the mod's row rules and settings table
# ------------------------------------------------------------------------------------------------
enable_language(RC)

# The app's version is the mod's: read from the one version string, and read again whenever that file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/mod.cpp)
file(STRINGS src/mod.cpp LSS_VERSION_LINE REGEX "kVersion = L\"" LIMIT_COUNT 1)
# Plain numbers, none starting with a zero: the resource compiler reads a leading zero as octal.
if(NOT LSS_VERSION_LINE MATCHES "L\"((0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)[^\"]*)\"")
    message(FATAL_ERROR "src/mod.cpp: kVersion not found as L\"major.minor.patch...\"")
endif()
set(LSS_VERSION "${CMAKE_MATCH_1}")
set(LSS_VERSION_MAJOR "${CMAKE_MATCH_2}")
set(LSS_VERSION_MINOR "${CMAKE_MATCH_3}")
set(LSS_VERSION_PATCH "${CMAKE_MATCH_4}")

# The app's file name is the one the mod starts its guard by: read from that line, and read again whenever its file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/views/display_move.h)
file(STRINGS src/views/display_move.h LSS_SETUP_LINE REGEX "kSetupExe = L\"" LIMIT_COUNT 1)
if(NOT LSS_SETUP_LINE MATCHES "L\"([A-Za-z0-9_]+)\\.exe\"")
    message(FATAL_ERROR "src/views/display_move.h: kSetupExe not found as L\"Name.exe\"")
endif()
set(LSS_SETUP_NAME "${CMAKE_MATCH_1}")
configure_file(setup/setup_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/setup_version.h" @ONLY)

add_executable(SetupApp WIN32
    setup/main.cpp
    setup/app.cpp
    setup/guard.cpp
    setup/tips.cpp
    setup/view.cpp
    setup/app.rc
    setup/app.manifest
    src/views/display_config.cpp
    src/views/display_move.cpp
    src/views/monitor_list.cpp
    src/views/monitor_picks.cpp
    src/views/monitor_row.cpp
    src/views/record_file.cpp
)
set_property(TARGET SetupApp PROPERTY OUTPUT_NAME "${LSS_SETUP_NAME}")
target_include_directories(SetupApp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}"
                                            "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(SetupApp PRIVATE cxx_std_20)
target_compile_definitions(SetupApp PRIVATE NOMINMAX)
target_link_libraries(SetupApp PRIVATE user32 gdi32 msimg32 comctl32 shell32 dwmapi uxtheme)
set_source_files_properties(setup/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/setup/app.ico")  # a new icon rebuilds it
set_property(TARGET SetupApp PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # one file: no runtime to install

# ------------------------------------------------------------------------------------------------
# Host-side tests: the logic that needs neither the game nor UE4SS
# ------------------------------------------------------------------------------------------------
enable_testing()

# One test executable per test file: tests/<name>_test.cpp with the sources it needs (headers alone
# need none). Every test is a dependency of `lss_tests`, so `cmake --build build --target lss_tests`
# builds them all before `ctest` runs them (tests/expect.h: the check and the verdict).
function(lss_test name)
    set(target ${name}_test)
    add_executable(${target} tests/${name}_test.cpp ${ARGN})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_features(${target} PRIVATE cxx_std_20)
    add_test(NAME ${name} COMMAND ${target})
    set_property(GLOBAL APPEND PROPERTY LSS_TEST_TARGETS ${target})
endfunction()

lss_test(one_family_per_view src/renderer/one_family_per_view.cpp)
lss_test(output_load_action src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(held_setting src/players/held_setting.cpp)
lss_test(view_shape src/views/view_shape.cpp)
lss_test(split_layout src/players/split_layout.cpp src/players/held_setting.cpp src/platform/settings.cpp
         src/platform/module_folder.cpp tests/log_console.cpp)
lss_test(split_screen_settings src/players/split_screen_settings.cpp src/players/held_setting.cpp)
lss_test(monitor_row src/views/monitor_row.cpp)
lss_test(monitor_picks src/views/monitor_picks.cpp src/views/monitor_row.cpp)
lss_test(row_journal)
lss_test(setup_rules src/views/monitor_picks.cpp src/views/monitor_row.cpp)
target_include_directories(setup_rules_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")  # the app's own folder (setup/)
lss_test(single_view src/players/single_view.cpp)
lss_test(whole_screen src/views/whole_screen_reason.cpp)
lss_test(column_source src/views/column_source.cpp)
lss_test(column_rules)
lss_test(split_fov_rules)
lss_test(stream_view_math src/views/stream_view_math.cpp)
lss_test(pool_bits src/platform/pool_bits.cpp)
lss_test(strand_watch src/world/strand_watch.cpp)
lss_test(swap_window src/scopes/swap_window.cpp)
lss_test(party_lease src/session/party_lease.cpp)
lss_test(leave_sequence src/session/leave_sequence.cpp)
lss_test(leader_answer)
lss_test(settings)
lss_test(window_span_rules)
lss_test(control_registry src/platform/command_registry.cpp)
lss_test(control_script src/control/script.cpp src/platform/command_registry.cpp)
lss_test(control_runner src/control/runner.cpp src/control/script.cpp src/platform/command_registry.cpp)
lss_test(control_server src/control/server.cpp)
lss_test(stick_select src/input/stick_select.cpp)
lss_test(call_redirect src/platform/call_redirect.cpp src/platform/block_patch.cpp src/platform/module_pin.cpp
         src/platform/code_memory.cpp src/platform/image.cpp src/platform/build_translation.cpp)
lss_test(entry_redirect src/platform/entry_redirect.cpp src/platform/block_patch.cpp src/platform/module_pin.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(code_memory src/platform/code_memory.cpp)
lss_test(block_patch src/platform/block_patch.cpp src/platform/code_memory.cpp)
lss_test(sparse_array src/platform/guarded.cpp)
lss_test(tset_find src/platform/guarded.cpp)
lss_test(overlay_band src/hud/layout/overlay_band.cpp)
lss_test(viewport_placement src/hud/layout/viewport_placement.cpp)
lss_test(entry_filter_code)
lss_test(minimap_once_code)
lss_test(first_player_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(enemy_gauges_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(command_rules_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(panel_gate_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(centered_frame_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(popups_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(placement_record)
lss_test(arrow_edge_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(family_boundary_list)
lss_test(button_view)
lss_test(settled_log)
lss_test(ucpc_choice)
lss_test(charapop_rule)
lss_test(label_states src/hud/minimap/label_states.cpp)
lss_test(build_translation src/platform/build_translation.cpp)
lss_test(vtable_entry src/platform/vtable_entry.cpp src/platform/module_pin.cpp src/platform/image.cpp
         src/platform/build_translation.cpp tests/log_console.cpp)
lss_test(guarded src/platform/guarded.cpp)
lss_test(call_site src/platform/guarded.cpp)
lss_test(game_thread_sites)
lss_test(generation src/players/generation.cpp src/players/weak_object.cpp src/platform/verified_code.cpp
         src/platform/vtable_entry.cpp src/platform/module_pin.cpp src/platform/guarded.cpp src/platform/image.cpp
         src/platform/build_translation.cpp tests/log_console.cpp)
lss_test(thread_hold)
lss_test(byte_scope src/platform/byte_scope.cpp src/platform/guarded.cpp)
lss_test(scope_ladder)
lss_test(angles)
lss_test(every)
lss_test(frame_watch_clock src/platform/frame_watch.cpp tests/log_console.cpp)
lss_test(deferred_sites src/platform/deferred_sites.cpp src/platform/image.cpp src/platform/build_translation.cpp)
lss_test(game_image src/platform/image.cpp src/platform/build_translation.cpp)
lss_test(layout_scale_code)
lss_test(shadow_feedback_code)
lss_test(traversal_rumble_code)
lss_test(outside_join src/session/outside_join.cpp)
lss_test(session_rules)
lss_test(coin_table)
lss_test(lock_group_rules)
lss_test(synergy_rules src/session/party_lease.cpp)
lss_test(menu_rules)
lss_test(seqlock)
lss_test(listener_rules)
lss_test(context_owners)
lss_test(traversal_rumble_rules)
lss_test(ride_rules)
lss_test(hud_rules)
lss_test(world_rules)
lss_test(platform_rules)
lss_test(axis_records)
lss_test(join_keys)
lss_test(field_rules)
lss_test(seats)
lss_test(camera_access)
lss_test(attack_framing)
lss_test(seat_walk_cap)
lss_test(seat_state)
lss_test(columns)
# The facts rule (tests/facts_once.py): no engine address declared in two files, no per-build pair with equal halves.
add_test(NAME facts_once COMMAND python "${CMAKE_CURRENT_SOURCE_DIR}/tests/facts_once.py" "${CMAKE_CURRENT_SOURCE_DIR}/src")

get_property(LSS_TEST_TARGETS GLOBAL PROPERTY LSS_TEST_TARGETS)
add_custom_target(lss_tests DEPENDS ${LSS_TEST_TARGETS})
