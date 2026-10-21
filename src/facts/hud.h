#pragma once

// The HUD: the UI system (g_EndUISystem) and the slots two or more modules read, the End menu renderer's layer
// slots and entries, the root overlay, and the user widget (its viewport layout, SetPositionInViewport, the
// field-action rows' layout).
//
// Addresses are as IDA shows them in ff7rebirth_.exe (image base 0x140000000); each function's prologue is checked
// (platform/verified_code.h) by the module that installs on it, and that module alone stands aside when it differs.
//
// Research: Rebirth/Documentation/research/RE-p2-hud.md (passes 1-3, 5, 7; P6.1), RE-p2-hud-view-levers.md §1, §2,
// RE-p2-field-prompts.md §1.2, §4, RE-p2-field-interact.md §12.14, RE-p2-command-menu-input.md §2, §10,
// RE-p2-enemy-markers.md §7, RE-p2-own-minimap.md §8.1. The layer facts' bytes were read from the running game on
// 2026-09-25.

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

// ---------------------------------------------------------------------------------------------
// The UI system
// ---------------------------------------------------------------------------------------------

/// g_EndUISystem (data): the pointer to the UI system object, the HUD's root. Its slots are 8-byte pointers to the
/// managers and holders below (EndUISystem[n] = slot n).
inline constexpr std::uintptr_t kEndUISystem = 0x1493496E0;

inline constexpr std::uint32_t kUIInput              = 0x10;   // EndUISystem[2]: Square's input object
inline constexpr std::uint32_t kUIFieldMarkers       = 0x98;   // -> a TSparseArray of the field-target markers
inline constexpr std::uint32_t kFieldMarkerEntry           = 140;    // its element size (the widgets are per module)
inline constexpr std::uint32_t kUICommandController  = 0xB8;   // EndUISystem[23]: the field command controller
inline constexpr std::uint32_t kUIEnemyStatus = 0xF8;   // the enemy-status manager (gauges, lock-on bars)
inline constexpr std::uint32_t kUIAreaMap     = 0x108;  // the area-map manager (the compass, the navimap)
inline constexpr std::uint32_t kUIKeyGuide           = 0x1A8;  // EndUISystem[53]: the key guide
inline constexpr std::uint32_t kUIActionRows         = 0x208;  // -> the field-action icon list (TArray<FWeakObjectPtr>)
/// Its byte marking a command-controller Setup deferred behind a fade. 1.005: MasterHUD_Tick 0x140F75A05 cmp byte
/// [rbx+2D9h],0 (1.004 0x140F37E5D [rbx+2D1h]); EndUISystem__ClearFieldCommandSetupPending 0x1413FBB7A and
/// SetupFieldCommandOrDefer 0x1413FBBE2 mov byte [..+2D9h],0 (a member was inserted after +0x2A8).
inline constexpr platform::PerBuild<std::uint32_t> kUISetupDeferred = {0x2D1, 0x2D9};

// ---------------------------------------------------------------------------------------------
// The root overlay's render (SEndRootOverlay__RenderToCompositeLayer 0x140C95CC0)
// ---------------------------------------------------------------------------------------------

// It asks for the rect to lay the HUD out in: `call EndUI__GetCompositeLayerRect` at 0x140C95F57 (7 bytes into its
// 16-byte block).
inline constexpr std::uintptr_t kOverlayRectCall = 0x140C95F57;
inline constexpr std::uint8_t   kGetCompositeLayerRectPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact   kGetCompositeLayerRect{L"EndUI::GetCompositeLayerRect", 0x140C958B8,  // FIntRect* (out)
                                                     kGetCompositeLayerRectPrologue};

// The same function then takes the frame's layer entry, after storing the host scale at +0x330
// (0x140C95FA0) and before the widget tree ticks (0x140C961C7):
//   mov rdx, [FName "SEndRootOverlay"] ; lea r8, [rsp+40h] (&rect) ; mov rcx, rsi (slot) ;
//   call FEndMenuRenderer__FindOrAddNamedLayerEntry at 0x140C960D9 (9 bytes into its block).
// Three register arguments, none on the stack; the entry comes back in rax.
inline constexpr std::uintptr_t kEntryCall = 0x140C960D9;
inline constexpr std::uint8_t   kFindOrAddNamedLayerEntryPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,   // mov [rsp+10h], rbx
    0x48, 0x89, 0x74, 0x24, 0x18,   // mov [rsp+18h], rsi
    0x48, 0x89, 0x7C, 0x24, 0x20};  // mov [rsp+20h], rdi
/// Entry* (Slot*, FName, FIntRect*).
inline constexpr FunctionFact kFindOrAddNamedLayerEntry{L"FEndMenuRenderer::FindOrAddNamedLayerEntry", 0x140FC8A98,
                                                        kFindOrAddNamedLayerEntryPrologue};

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
inline constexpr std::uintptr_t kSubmitCall = 0x140C96258;
inline constexpr std::uint8_t   kSubmitLayerSlotPrologue[] = {
    0x48, 0x8B, 0xC4,               // mov rax, rsp
    0x48, 0x89, 0x58, 0x10,         // mov [rax+10h], rbx
    0x48, 0x89, 0x70, 0x18,         // mov [rax+18h], rsi
    0x48, 0x89, 0x78, 0x20, 0x55};  // mov [rax+20h], rdi ; push rbp
inline constexpr FunctionFact kSubmitLayerSlot{L"FEndMenuRenderer::SubmitLayerSlot", 0x140FC873C,
                                               kSubmitLayerSlotPrologue};

// FEndMenuRenderer__AcquireLayerSlot(renderer, pool): rcx = the renderer, dl = the pool, the slot in rax. The root
// overlay's render takes pool 2 (its call at 0x140C960A2, hud/second_image), the navimap circle's paint pool 0.
inline constexpr std::uint8_t kAcquireLayerSlotPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00,
                                                             0x00};
inline constexpr FunctionFact kAcquireLayerSlot{L"FEndMenuRenderer::AcquireLayerSlot", 0x140FC8694,
                                                kAcquireLayerSlotPrologue};

// SEndNaviMapCircle__OnPaint_Pool0's pool-0 block: the acquire (rcx renderer, dl 0; its call
// crosses a 16-byte block, so it is installed from the game thread while that thread is outside the paint) and the
// submit (rcx renderer, dl 0, r8 slot). hud/minimap redirects them; hud/minimap_once skips the block
// (RE-p2-own-minimap.md §8.1).
inline constexpr std::uintptr_t kNaviMapCircleAcquireCall = 0x140FC6CAE;
inline constexpr std::uintptr_t kNaviMapCircleSubmitCall  = 0x140FC6F13;

// A layer slot's pool buffers are FVertexBuffer and FIndexBuffer resources (their vtables, data), initialised through
// BeginInitResource_OrDirectIfRenderThread (render thread: vt+0x28 directly).
inline constexpr std::uintptr_t kVertexBufferVtable = 0x146621CF8;
inline constexpr std::uintptr_t kIndexBufferVtable  = 0x146621D50;
inline constexpr std::uint8_t   kBeginInitResourcePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                                              0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0xB8,
                                                              0x90, 0x00, 0x00, 0x00};
inline constexpr FunctionFact   kBeginInitResource{L"BeginInitResource", 0x1409E7EF8,  // void (FRenderResource*)
                                                 kBeginInitResourcePrologue};

// EndUI__ContainScaleFor1920x1080: the UI scale of a rect relative to 1920x1080 (RE-p2-hud-view-levers.md
// §2). float(FVector2D) with the size packed in rcx (X low, Y high), the result in xmm0; a leaf that
// reads only its argument and .rdata constants, so any thread may call it.
inline constexpr std::uint8_t kContainScaleBytes[] = {
    0xC5, 0xFA, 0x6F, 0x1D, 0x48, 0xD4, 0x6A, 0x05,  // vmovdqu xmm3, [abs mask]
    0xC5, 0xFA, 0x10, 0x25, 0xC4, 0xD3, 0x6A, 0x05,  // vmovss xmm4, [1e-8]
    0x48, 0x89, 0x4C, 0x24, 0x10,                    // mov [rsp+10h], rcx
    0xC5, 0xFA, 0x10, 0x54, 0x24, 0x10};             // vmovss xmm2, [rsp+10h]
inline constexpr FunctionFact kContainScale{L"EndUI::ContainScaleFor1920x1080", 0x1409F52B0, kContainScaleBytes};

// The overlay singleton (SEndRootOverlay__GetOrCreate 0x1410EAB8C): the player-0 view size it
// caches before asking for the rect (written at 0x140C95F3C), and the host scale the marker
// helpers read (EndUI__ProjectWorldToRootOverlay and its twin are its only readers found).
inline constexpr std::uintptr_t kRootOverlaySingleton = 0x1491D1948;
inline constexpr std::uint32_t  kOverlayViewWidth     = 0x328;
inline constexpr std::uint32_t  kOverlayViewHeight    = 0x32C;
inline constexpr std::uint32_t  kOverlayHostScale     = 0x330;

// ---------------------------------------------------------------------------------------------
// The user widget (UUserWidget)
// ---------------------------------------------------------------------------------------------

// Its viewport layout (the stock ViewportAnchors, ViewportOffsets, ViewportAlignment),
// bound to each HUD root's SEndCanvas (RE-p2-hud.md P6.1; read live in pass 7). Read only: changes
// go through the reflected Set*InViewport setters.
inline constexpr std::uint32_t kUserWidgetViewportAnchors   = 0x210;  // FAnchors, 16 bytes
inline constexpr std::uint32_t kUserWidgetViewportOffsets   = 0x220;  // FMargin, 16 bytes
inline constexpr std::uint32_t kUserWidgetViewportAlignment = 0x230;  // FVector2D, 8 bytes
inline constexpr std::uint32_t kUserWidgetViewportLayoutEnd = 0x238;

/// UUserWidget::SetPositionInViewport(widget, FVector2D, bRemoveDPIScale): the stock setter the game's Blueprints
/// place widgets with (hud/position_probe) and the enemy gauges' placement calls.
inline constexpr std::uint8_t kSetPositionInViewportPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kSetPositionInViewport{L"UUserWidget::SetPositionInViewport", 0x140CC39E8,
                                                     kSetPositionInViewportPrologue};

/// FieldActionIconList__SortAndLayout(TArray<FWeakObjectPtr>* list, UObject* placeNow): sorts the given array by
/// priority and gives each live row the next slot in it (RE-p2-field-prompts.md §4). Called by the game only when a
/// row is added or removed.
inline constexpr std::uint8_t kSortAndLayoutBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55,
                                                       0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8,
                                                       0x20, 0x00};
inline constexpr FunctionFact kSortAndLayout{L"FieldActionIconList::SortAndLayout", 0x1414A9B14, kSortAndLayoutBytes};

}  // namespace lss::facts
