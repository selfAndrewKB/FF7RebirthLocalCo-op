#pragma once

// The engine facts the HUD layout relies on, as IDA addresses of ff7rebirth_.exe (image base
// 0x140000000). Research: Rebirth/Documentation/research/RE-p2-hud.md (passes 1-3); the bytes
// below were read from the running game on 2026-09-25. They are checked at startup; if any fails,
// only the HUD layout stays out (the rest of the mod does not depend on it).

#include <cstdint>

namespace lss::hud::engine {

// SEndRootOverlay__RenderToCompositeLayer (0x140C95CC0) asks for the rect to lay the HUD out in:
// `call EndUI__GetCompositeLayerRect` at 0x140C95F57 (7 bytes into its 16-byte block).
inline constexpr std::uintptr_t kOverlayRectCall        = 0x140C95F57;
inline constexpr std::uintptr_t kGetCompositeLayerRect  = 0x140C958B8;  // FIntRect* (FIntRect* out)
inline constexpr std::uint8_t   kGetCompositeLayerRectPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};

// The same function then takes the frame's layer entry, after storing the host scale at +0x330
// (0x140C95FA0) and before the widget tree ticks (0x140C961C7):
//   mov rdx, [FName "SEndRootOverlay"] ; lea r8, [rsp+40h] (&rect) ; mov rcx, rsi (slot) ;
//   call FEndMenuRenderer__FindOrAddNamedLayerEntry at 0x140C960D9 (9 bytes into its block).
// Three register arguments, none on the stack; the entry comes back in rax.
inline constexpr std::uintptr_t kEntryCall                = 0x140C960D9;
inline constexpr std::uintptr_t kFindOrAddNamedLayerEntry = 0x140FC8A98;  // Entry* (Slot*, FName, FIntRect*)
inline constexpr std::uint8_t   kFindOrAddNamedLayerEntryPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,   // mov [rsp+10h], rbx
    0x48, 0x89, 0x74, 0x24, 0x18,   // mov [rsp+18h], rsi
    0x48, 0x89, 0x7C, 0x24, 0x20};  // mov [rsp+20h], rdi

// FEndMenuLayerEntry: the FMatrix at +0x50..+0x8F that DrawEntryBatchToTarget projects the entry
// with, rebuilt from the rect's size only when the rect changes (FEndMenuLayerEntry__SetRect_
// RebuildMatrix 0x140FC8CE8). The x/y terms the placement writes:
inline constexpr std::uint32_t kEntryMatrixScaleX     = 0x50;  // row 0 x
inline constexpr std::uint32_t kEntryMatrixScaleY     = 0x64;  // row 1 y
inline constexpr std::uint32_t kEntryMatrixTranslateX = 0x80;  // row 3 x
inline constexpr std::uint32_t kEntryMatrixTranslateY = 0x84;  // row 3 y

// The overlay singleton (SEndRootOverlay__GetOrCreate 0x1410EAB8C): the player-0 view size it
// caches before asking for the rect (written at 0x140C95F3C), and the host scale the marker
// helpers read (EndUI__ProjectWorldToRootOverlay and its twin are its only readers found).
inline constexpr std::uintptr_t kRootOverlaySingleton = 0x1491D1948;
inline constexpr std::uint32_t  kOverlayViewWidth     = 0x328;
inline constexpr std::uint32_t  kOverlayViewHeight    = 0x32C;
inline constexpr std::uint32_t  kOverlayHostScale     = 0x330;

}  // namespace lss::hud::engine
