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

// The entry's list of per-child view-projections (pass 5): widgets SEndCanvas paints in 3D push a
// matrix each (FEndElementList__PushViewProjection 0x142B36284), and their elements are drawn with
// it instead of the entry matrix. A TArray<FMatrix>: data at +224, count at +232. It is emptied as
// the entry is reused each frame (0x140FC9120) and copied to the render batch when the slot is
// submitted (0x140FC87B1), so it is placed once per frame just before that.
inline constexpr std::uint32_t kEntryMatrixData     = 224;
inline constexpr std::uint32_t kEntryMatrixCount    = 232;
inline constexpr std::int32_t  kEntryMatrixMaxCount = 4096;  // a sanity bound; the HUD pushes a handful

// The render ends by submitting the slot: `mov r8, rsi (slot) ; mov dl, 2 (pool) ; mov rcx, rdi ;
// call FEndMenuRenderer__SubmitLayerSlot` at 0x140C96258 (8 bytes into its block).
inline constexpr std::uintptr_t kSubmitCall                 = 0x140C96258;
inline constexpr std::uintptr_t kSubmitLayerSlot            = 0x140FC873C;
inline constexpr std::uint8_t   kSubmitLayerSlotPrologue[]  = {
    0x48, 0x8B, 0xC4,               // mov rax, rsp
    0x48, 0x89, 0x58, 0x10,         // mov [rax+10h], rbx
    0x48, 0x89, 0x70, 0x18,         // mov [rax+18h], rsi
    0x48, 0x89, 0x78, 0x20, 0x55};  // mov [rax+20h], rdi ; push rbp

// The overlay singleton (SEndRootOverlay__GetOrCreate 0x1410EAB8C): the player-0 view size it
// caches before asking for the rect (written at 0x140C95F3C), and the host scale the marker
// helpers read (EndUI__ProjectWorldToRootOverlay and its twin are its only readers found).
inline constexpr std::uintptr_t kRootOverlaySingleton = 0x1491D1948;
inline constexpr std::uint32_t  kOverlayViewWidth     = 0x328;
inline constexpr std::uint32_t  kOverlayViewHeight    = 0x32C;
inline constexpr std::uint32_t  kOverlayHostScale     = 0x330;

// UUserWidget's viewport layout (the stock ViewportAnchors, ViewportOffsets, ViewportAlignment),
// bound to each HUD root's SEndCanvas (RE-p2-hud.md P6.1; read live in pass 7). Read only: changes
// go through the reflected Set*InViewport setters.
inline constexpr std::uint32_t kUserWidgetViewportAnchors   = 0x210;  // FAnchors, 16 bytes
inline constexpr std::uint32_t kUserWidgetViewportOffsets   = 0x220;  // FMargin, 16 bytes
inline constexpr std::uint32_t kUserWidgetViewportAlignment = 0x230;  // FVector2D, 8 bytes
inline constexpr std::uint32_t kUserWidgetViewportLayoutEnd = 0x238;

}  // namespace lss::hud::engine
