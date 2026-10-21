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
    src/hud/search_pins.cpp
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

    src/platform/block_patch.cpp
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
    src/player_two/climb_context.cpp
    src/player_two/near_fade.cpp
    src/player_two/rumble_list_two.cpp
    src/player_two/ffb_manager_two.cpp
    src/player_two/audio_listener_two.cpp
    src/player_two/approach_two.cpp
    src/player_two/move_type_two.cpp
    src/player_two/anim_two_views.cpp
    src/player_two/traversal_camera.cpp
    src/player_two/traversal_watch.cpp
    src/player_two/shop_talk.cpp
    src/player_two/switch_volume_two.cpp
    src/player_two/trigger_two.cpp
    src/player_two/vault_camera.cpp
    src/player_two/location_rows.cpp
    src/player_two/shoulder_scale.cpp
    src/player_two/walk_limits.cpp
    src/player_two/field_pickup.cpp
    src/player_two/pad_rumble.cpp
    src/player_two/combat_rumble.cpp
    src/player_two/vending.cpp
    src/player_two/charapop_facts.cpp
    src/player_two/charapop_near.cpp
    src/player_two/charapop_centre.cpp
    src/player_two/world_actor_reach.cpp
    src/player_two/chocobo_ride.cpp
    src/player_two/chocobo_climb.cpp
    src/player_two/shared_ride.cpp
    src/player_two/wheelie_ride.cpp
    src/hud/minimap.cpp
    src/hud/minimap_labels.cpp
    src/hud/minimap_icons.cpp
    src/hud/label_states.cpp
    src/player_two/party_warp.cpp
    src/player_two/strand_watch.cpp
    src/player_two/battle_camera.cpp
    src/player_two/camera_override.cpp
    src/player_two/attack_cameras.cpp
    src/player_two/attack_framing.cpp
    src/player_two/synergy_partner.cpp
    src/player_two/camera_effects_two.cpp
    src/player_two/camera_distance.cpp
    src/player_two/main_camera_shape.cpp
    src/player_two/command_locks.cpp
    src/player_two/battle_warp_two.cpp
    src/player_two/equal_targeting.cpp
    src/player_two/player_hits.cpp
    src/player_two/leader_reads.cpp
    src/player_two/lock_group_coin.cpp
    src/player_two/target_coin.cpp
    src/player_two/leave_watch.cpp
    src/player_two/command_rules.cpp
    src/player_two/focus_scope.cpp
    src/player_two/command_menu_two.cpp
    src/player_two/command_menu_tick.cpp
    src/player_two/leader_scope.cpp
    src/player_two/lock_on.cpp
    src/player_two/own_controller.cpp
    src/player_two/party_events.cpp
    src/player_two/party_lease.cpp
    src/player_two/in_battle.cpp
    src/player_two/weak_object.cpp
    src/player_two/party_member.cpp
    src/player_two/party_switch.cpp
    src/player_two/primary_controller.cpp
    src/player_two/scope_leaks.cpp
    src/player_two/move_state.cpp
    src/player_two/pad_buttons.cpp
    src/player_two/pad_sticks.cpp
    src/player_two/outside_join.cpp
    src/player_two/session.cpp
    src/player_two/session_join.cpp
    src/player_two/stick_select.cpp
    src/player_two/swap_window.cpp
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
    src/renderer/async_compute.cpp
    src/renderer/before_device.cpp
    src/renderer/residency_off.cpp
    src/renderer/one_family_per_view.cpp
    src/renderer/view_family_submission.cpp

    src/views/player_view_point.cpp
    src/views/stream_view_math.cpp
    src/views/stream_views.cpp
    src/views/monitor_row.cpp
    src/views/third_view.cpp
    src/views/view_shape.cpp
    src/views/window_span.cpp
    src/views/whole_screen.cpp
    src/views/screen_widgets.cpp
    src/views/whole_screen_reason.cpp
)

# Includes are rooted at src/ so every #include names its module ("platform/log.h").
target_include_directories(LocalSplitScreen PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(LocalSplitScreen PRIVATE cxx_std_20)
target_link_libraries(LocalSplitScreen PUBLIC UE4SS)

# Development tools (src/dev/dev_tools.h): the control-script channel, the HUD trace, the position
# probe, the dump commands, and the watches that only log (installed and ticked by dev_tools.cpp; a
# header a shipped module includes carries empty stubs for the build without them). Off for a player's
# build: -DLSS_DEV_TOOLS=OFF.
option(LSS_DEV_TOOLS "Build the development tools into the mod" ON)
if(LSS_DEV_TOOLS)
    target_sources(LocalSplitScreen PRIVATE
        src/dev/dev_tools.cpp
        src/control/command_channel.cpp
        src/control/mod_target.cpp
        src/control/runner.cpp
        src/control/script.cpp
        src/hud/hud_trace.cpp
        src/hud/position_probe.cpp
        src/hud/widget_dump.cpp
        src/hud/label_relayouts.cpp
        src/player_two/battle_input.cpp
        src/player_two/charapop_watch.cpp
        src/player_two/framing_watch.cpp
        src/player_two/handover_watch.cpp
        src/player_two/interact_watch.cpp
        src/player_two/menu_focus.cpp
        src/player_two/move_census.cpp
        src/player_two/ride_watch.cpp
        src/player_two/rumble_watch.cpp
        src/player_two/tag_counts.cpp
        src/player_two/target_census.cpp
        src/player_two/wheelie_watch.cpp
        src/renderer/submission_census.cpp
    )
    target_compile_definitions(LocalSplitScreen PRIVATE LSS_DEV_TOOLS=1)
else()
    target_compile_definitions(LocalSplitScreen PRIVATE LSS_DEV_TOOLS=0)
endif()

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
lss_test(split_screen_settings src/players/split_screen_settings.cpp)
lss_test(view_shape src/views/view_shape.cpp)
lss_test(split_layout src/players/split_layout.cpp)
lss_test(monitor_row src/views/monitor_row.cpp)
lss_test(single_view src/players/single_view.cpp)
lss_test(whole_screen src/views/whole_screen_reason.cpp)
lss_test(stream_view_math src/views/stream_view_math.cpp)
lss_test(pool_bits src/platform/pool_bits.cpp)
lss_test(strand_watch src/player_two/strand_watch.cpp)
lss_test(swap_window src/player_two/swap_window.cpp)
lss_test(party_lease src/player_two/party_lease.cpp)
lss_test(leave_sequence src/player_two/leave_sequence.cpp)
lss_test(control_script src/control/script.cpp)
lss_test(control_runner src/control/runner.cpp src/control/script.cpp)
lss_test(stick_select src/player_two/stick_select.cpp)
lss_test(call_redirect src/platform/call_redirect.cpp src/platform/block_patch.cpp src/platform/module_pin.cpp
         src/platform/code_memory.cpp src/platform/image.cpp src/platform/build_translation.cpp)
lss_test(entry_redirect src/platform/entry_redirect.cpp src/platform/block_patch.cpp src/platform/module_pin.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(code_memory src/platform/code_memory.cpp)
lss_test(block_patch src/platform/block_patch.cpp src/platform/code_memory.cpp)
lss_test(sparse_array src/platform/guarded.cpp)
lss_test(overlay_band src/hud/overlay_band.cpp)
lss_test(viewport_placement src/hud/viewport_placement.cpp)
lss_test(entry_filter_code)
lss_test(minimap_once_code)
lss_test(first_player_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(enemy_gauges_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(command_rules_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(panel_gate_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(centered_frame_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(popups_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(arrow_edge_code src/platform/code_redirect.cpp src/platform/block_patch.cpp src/platform/code_memory.cpp tests/log_console.cpp)
lss_test(family_boundary_list)
lss_test(button_view)
lss_test(settled_log)
lss_test(ucpc_choice)
lss_test(charapop_rule)
lss_test(label_states src/hud/label_states.cpp)
lss_test(build_translation src/platform/build_translation.cpp)
lss_test(vtable_entry src/platform/vtable_entry.cpp src/platform/module_pin.cpp src/platform/image.cpp
         src/platform/build_translation.cpp tests/log_console.cpp)
lss_test(guarded src/platform/guarded.cpp)
lss_test(call_site src/platform/guarded.cpp)
lss_test(game_thread_sites)
lss_test(thread_hold)
lss_test(angles)
lss_test(every)
lss_test(frame_watch_clock src/platform/frame_watch.cpp tests/log_console.cpp)
lss_test(deferred_sites src/platform/deferred_sites.cpp src/platform/image.cpp src/platform/build_translation.cpp)
lss_test(game_image src/platform/image.cpp src/platform/build_translation.cpp)
lss_test(layout_scale_code)
lss_test(shadow_feedback_code)
lss_test(rumble_list_code)
lss_test(outside_join src/player_two/outside_join.cpp)
lss_test(session_rules)
lss_test(coin_table)
lss_test(lock_group_rules)
lss_test(synergy_rules src/player_two/party_lease.cpp)
lss_test(menu_rules)
lss_test(seqlock)
lss_test(listener_choice)
lss_test(rumble_rules)
lss_test(ride_rules)
lss_test(hud_rules)
lss_test(world_rules)
lss_test(platform_rules)
lss_test(axis_records)
lss_test(join_keys)
lss_test(field_rules)

get_property(LSS_TEST_TARGETS GLOBAL PROPERTY LSS_TEST_TARGETS)
add_custom_target(lss_tests DEPENDS ${LSS_TEST_TARGETS})
