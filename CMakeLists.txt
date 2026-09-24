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

    src/platform/code_redirect.cpp
    src/platform/image.cpp
    src/platform/log.cpp
    src/platform/vtable_entry.cpp

    src/players/join_keys.cpp
    src/players/local_players.cpp
    src/players/shared_camera.cpp
    src/players/split_screen_settings.cpp
    src/players/unreal_access.cpp

    src/renderer/additional_family_output.cpp
    src/renderer/one_family_per_view.cpp
    src/renderer/view_family_submission.cpp

    src/views/player_view_point.cpp
    src/views/view_turn.cpp
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

add_executable(SharedCameraTests
    tests/shared_camera_test.cpp
    src/players/shared_camera.cpp
)
target_include_directories(SharedCameraTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(SharedCameraTests PRIVATE cxx_std_20)
add_test(NAME shared_camera COMMAND SharedCameraTests)

add_executable(OutputLoadActionTests
    tests/output_load_action_test.cpp
    src/platform/code_redirect.cpp
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

add_executable(ViewTurnTests
    tests/view_turn_test.cpp
    src/views/view_turn.cpp
)
target_include_directories(ViewTurnTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(ViewTurnTests PRIVATE cxx_std_20)
add_test(NAME view_turn COMMAND ViewTurnTests)
