#pragma once

// The dev-tools switch (CMake option LSS_DEV_TOOLS, dev/dev_tools.h) as one macro for `#if`: 1 with the dev tools,
// 0 in a player's build.
//
// Two shapes use it. A dev-only module's header declares its functions under `#if LSS_DEV` and gives each an empty
// inline stub under `#else`, so a shipped module may still call it. A shipped module keeps its logging half (the
// readers, the snapshot, the lines; a redirect that exists only to log) under `#if LSS_DEV`, with a stub for the one
// function the feature's flow calls, so that flow reads the same in both builds.

#if defined(LSS_DEV_TOOLS) && LSS_DEV_TOOLS
#define LSS_DEV 1
#else
#define LSS_DEV 0
#endif
