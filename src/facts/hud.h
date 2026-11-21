#pragma once

// The HUD: the UI system (g_EndUISystem) and the slots two or more modules read, the End menu renderer's layer
// slots and entries, the root overlay, and the user widget (its viewport layout, SetPositionInViewport, the
// field-action rows' layout).
//
// Addresses are as IDA shows them in ff7rebirth_.exe (image base 0x140000000); each function's prologue is checked
// (platform/verified_code.h) by the module that installs on it, and that module alone stands aside when it differs.
//
// Research: Rebirth/Documentation/research/RE-p2-hud.md, RE-p2-hud-view-levers.md, RE-p2-field-prompts.md,
// RE-p2-field-interact.md, RE-p2-command-menu-input.md, RE-p2-enemy-markers.md, RE-p2-own-minimap.md. The layer
// facts' bytes were read from the running game.

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <array>
#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;

// ---------------------------------------------------------------------------------------------
// The UI system
// ---------------------------------------------------------------------------------------------

/// g_EndUISystem (data): the UI system object itself, the HUD's root, at this address. Its slots are 8-byte pointers
/// to the managers and holders below (EndUISystem[n] = slot n, at the address plus 8n).
inline constexpr std::uintptr_t kEndUISystem = 0x149186150;

inline constexpr std::uint32_t kUIInput              = 0x10;   // EndUISystem[2]: Square's input object
inline constexpr std::uint32_t kUIFieldMarkers       = 0x98;   // -> a TSparseArray of the field-target markers
inline constexpr std::uint32_t kFieldMarkerEntry           = 140;    // its element size (the widgets are per module)
inline constexpr std::uint32_t kUICommandController  = 0xB8;   // EndUISystem[23]: the field command controller
inline constexpr std::uint32_t kUIEnemyStatus = 0xF8;   // the enemy-status manager (gauges, lock-on bars)
/// The UI fonts (RE-per-player-icons.md): EndUISystem+0x28 points at the font holder, whose first array {weak UEndFont*
/// data, int32 num} lists the four system fonts (0 Normal, 1 Small, 2 Large, 3 XLarge; num 0 before they load). The
/// game's device change writes the button bytes of Small, Normal and Large only; a text's button tags read its own
/// font's two bytes.
inline constexpr std::uint32_t kUIFontHolder   = 0x28;
inline constexpr std::uint32_t kFontHolderNum  = 0x08;
inline constexpr std::array<std::uint32_t, 3> kButtonFonts{1, 0, 2};
inline constexpr std::uint32_t kFontPadFamily  = 0x558;  // u8: which pad's art
inline constexpr std::uint32_t kFontKeyboard   = 0x559;  // u8: keycaps where a tag allows them
inline constexpr std::uint32_t kUIAreaMap     = 0x108;  // the area-map manager (the compass, the navimap)
inline constexpr std::uint32_t kUIFilterManager      = 0x148;  // EndUISystem[41]: the UI filter manager (the
                                                                 // tactical tint's latch and count; menu_tint)
inline constexpr std::uint32_t kUIKeyGuide           = 0x1A8;  // EndUISystem[53]: the key guide
inline constexpr std::uint32_t kUIActionRows         = 0x208;  // -> the field-action icon list (TArray<FWeakObjectPtr>)
inline constexpr std::uint32_t kUIMenuLocks          = 0x2D8;  // u64: the command menu's lock bits (bit 0: the on/off
                                                                 // table's action lock; any other keeps it shut too)
inline constexpr std::uint64_t kUIMenuActionLock     = 0x01;
/// The save-data notice, the game's top-right box ("Checking saved data..."; EndUISystem[35], RE-title-notice.md):
/// the manager holds the widget's weak at +0 and a shown byte; the widget (SaveIcon_C) has one text block. The
/// game's show takes a world and a kind (0: the check's look, no later text swap), its hide the next kind (0: none;
/// an icon up for two seconds or more collapses at once), and the text block's setter takes an FString whose buffer
/// it frees (FMemory's). 1.005 through replaced-title-notice.json.
inline constexpr std::uint32_t kUISaveIcon    = 0x118;
inline constexpr std::uint32_t kSaveIconShown = 16;
inline constexpr std::uint32_t kSaveIconText  = 0x430;
inline constexpr std::uint8_t  kShowSaveIconBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                                       0x00, 0x00, 0x00, 0xE8, 0x30, 0x09, 0x90, 0x00};
inline constexpr FunctionFact  kShowSaveIcon{L"EndUI_ShowSaveIcon_Static", 0x14186B240, kShowSaveIconBytes};
inline constexpr std::uint8_t  kHideSaveIconBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                       0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact  kHideSaveIcon{L"EndUI_HideSaveIcon_Static", 0x140FE4168, kHideSaveIconBytes};
inline constexpr std::uint8_t  kTextBlockSetTextBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                           0x24, 0x10, 0x57, 0xB8, 0x50, 0x00, 0x00, 0x00};
inline constexpr FunctionFact  kTextBlockSetText{L"UEndTextBlock_SetText_FStringPtr", 0x140E8B948, kTextBlockSetTextBytes};
/// Its byte marking a command-controller Setup deferred behind a fade. 1.005: MasterHUD_Tick 0x140F75A05 cmp byte
/// [rbx+2D9h],0 (1.004 0x140F37E5D [rbx+2D1h]); EndUISystem__ClearFieldCommandSetupPending 0x1413FBB7A and
/// SetupFieldCommandOrDefer 0x1413FBBE2 mov byte [..+2D9h],0 (a member was inserted after +0x2A8).
inline constexpr platform::PerBuild<std::uint32_t> kUISetupDeferred = {0x2D9, 0x2D1};

/// UWidget's render transform applied from its fields (+0x90/+0x94 the translation, +0x98/+0x9C the scale, +0xA8 the
/// angle): the game's own call after it writes them (the enemy gauges' scale, RE-p2-enemy-markers.md; the chocobo
/// wrangle's targets, RE-chocobo-wrangle-targets.md). Its 1.004 prologue differs (the translation table carries
/// that build's bytes).
inline constexpr std::uint8_t kApplyRenderTransformBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                              0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55,
                                                              0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr FunctionFact  kApplyRenderTransform{L"UWidget's render transform applied", 0x140B12FA8,
                                                    kApplyRenderTransformBytes};

// ---------------------------------------------------------------------------------------------
// The root overlay's render (SEndRootOverlay__RenderToCompositeLayer 0x140B0ED18)
// ---------------------------------------------------------------------------------------------

// It asks for the rect to lay the HUD out in: `call EndUI__GetCompositeLayerRect` at 0x140B0EFAD (7 bytes into its
// 16-byte block).
inline constexpr std::uintptr_t kOverlayRectCall = 0x140B0EFAD;
inline constexpr std::uint8_t   kGetCompositeLayerRectPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact   kGetCompositeLayerRect{L"EndUI::GetCompositeLayerRect", 0x140B0E8CC,  // FIntRect* (out)
                                                     kGetCompositeLayerRectPrologue};

// The same function then takes the frame's layer entry, after storing the host scale at +0x330
// (0x140B0EFF6) and before the widget tree ticks (0x140C961C7):
//   mov rdx, [FName "SEndRootOverlay"] ; lea r8, [rsp+40h] (&rect) ; mov rcx, rsi (slot) ;
//   call FEndMenuRenderer__FindOrAddNamedLayerEntry at 0x140B0F140 (9 bytes into its block).
// Three register arguments, none on the stack; the entry comes back in rax.
inline constexpr std::uintptr_t kEntryCall = 0x140B0F140;
inline constexpr std::uint8_t   kFindOrAddNamedLayerEntryPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,   // mov [rsp+10h], rbx
    0x4C, 0x89, 0x44, 0x24, 0x18,   // mov [rsp+18h], rsi
    0x55, 0x56, 0x57, 0x41, 0x54};  // mov [rsp+20h], rdi
/// Entry* (Slot*, FName, FIntRect*).
inline constexpr FunctionFact kFindOrAddNamedLayerEntry{L"FEndMenuRenderer::FindOrAddNamedLayerEntry", 0x140A47B68,
                                                        kFindOrAddNamedLayerEntryPrologue};

// FEndMenuLayerEntry: the FMatrix at +0x50..+0x8F that DrawEntryBatchToTarget projects the entry
// with, rebuilt from the rect's size only when the rect changes (FEndMenuLayerEntry__SetRect_
// RebuildMatrix 0x140FC8CE8). The x/y terms the placement writes:
inline constexpr std::uint32_t kEntryMatrixScaleX     = 0x50;  // row 0 x
inline constexpr std::uint32_t kEntryMatrixScaleY     = 0x64;  // row 1 y
inline constexpr std::uint32_t kEntryMatrixTranslateX = 0x80;  // row 3 x
inline constexpr std::uint32_t kEntryMatrixTranslateY = 0x84;  // row 3 y

// The entry's list of per-child view-projections: widgets SEndCanvas paints in 3D push a
// matrix each (FEndElementList__PushViewProjection 0x142B36284), and their elements are drawn with
// it instead of the entry matrix. A TArray<FMatrix>: data at +224, count at +232. It is emptied as
// the entry is reused each frame (0x140FC9120) and copied to the render batch when the slot is
// submitted (0x140FC87B1), so it is placed once per frame just before that.
inline constexpr std::uint32_t kEntryMatrixData     = 224;
inline constexpr std::uint32_t kEntryMatrixCount    = 232;
inline constexpr std::int32_t  kEntryMatrixMaxCount = 4096;  // a sanity bound; the HUD pushes a handful

// The render ends by submitting the slot: `mov r8, rsi (slot) ; mov dl, 2 (pool) ; mov rcx, rdi ;
// call FEndMenuRenderer__SubmitLayerSlot` at 0x140B0F2F6 (8 bytes into its block).
inline constexpr std::uintptr_t kSubmitCall = 0x140B0F2F6;
inline constexpr std::uint8_t   kSubmitLayerSlotPrologue[] = {
    0x48, 0x89, 0x5C,               // mov rax, rsp
    0x24, 0x10, 0x48, 0x89,         // mov [rax+10h], rbx
    0x74, 0x24, 0x18, 0x55,         // mov [rax+18h], rsi
    0x57, 0x41, 0x54, 0x41, 0x56};  // mov [rax+20h], rdi ; push rbp
inline constexpr FunctionFact kSubmitLayerSlot{L"FEndMenuRenderer::SubmitLayerSlot", 0x140A47A14,
                                               kSubmitLayerSlotPrologue};

// FEndMenuRenderer__AcquireLayerSlot(renderer, pool): rcx = the renderer, dl = the pool, the slot in rax. The root
// overlay's render takes pool 2 (its call at 0x140B0F119, hud/passes/column_hud_images), the navimap circle's paint pool
// 0.
inline constexpr std::uint8_t kAcquireLayerSlotPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00,
                                                             0x00};
inline constexpr FunctionFact kAcquireLayerSlot{L"FEndMenuRenderer::AcquireLayerSlot", 0x140A4796C,
                                                kAcquireLayerSlotPrologue};

// SEndNaviMapCircle__OnPaint_Pool0's pool-0 block: the acquire (rcx renderer, dl 0; its call
// crosses a 16-byte block, so it is installed from the game thread while that thread is outside the paint) and the
// submit (rcx renderer, dl 0, r8 slot). hud/minimap redirects them; hud/minimap_once skips the block
// (RE-p2-own-minimap.md).
inline constexpr std::uint8_t kNaviMapCircleAcquireCallBytes[] = {0xE8, 0x4F, 0xD4, 0x73, 0xFF};  // call Acquire
inline constexpr platform::FunctionFact kNaviMapCircleAcquireCall{L"the navi-map circle's acquire call", 0x14130A518,
                                                                  kNaviMapCircleAcquireCallBytes};
inline constexpr std::uint8_t kNaviMapCircleSubmitCallBytes[] = {0xE8, 0x96, 0xD2, 0x73, 0xFF};  // call Submit
inline constexpr platform::FunctionFact kNaviMapCircleSubmitCall{L"the navi-map circle's submit call", 0x14130A779,
                                                                 kNaviMapCircleSubmitCallBytes};

// A layer slot's pool buffers are FVertexBuffer and FIndexBuffer resources (their vtables, data), initialised through
// BeginInitResource_OrDirectIfRenderThread (render thread: vt+0x28 directly).
inline constexpr std::uintptr_t kVertexBufferVtable = 0x1464AF7B8;
inline constexpr std::uintptr_t kIndexBufferVtable  = 0x1464AF810;
// Such a buffer as the menu renderer's ctor lays it out (research/RE-p2-hud-second-image.md): 0x20 bytes, the vtable,
// the render resource's list index (-1 until InitResource lists it), a u16 the ctor sets to 2, the RHI buffer
// InitResource makes, and the capacity.
inline constexpr std::uint32_t  kMenuBufferSize      = 0x20;
inline constexpr std::uint32_t  kMenuBufferVtable    = 0x00;
inline constexpr std::uint32_t  kMenuBufferListIndex = 0x08;  // i32
inline constexpr std::uint32_t  kMenuBufferCtorWord  = 0x0C;  // u16
inline constexpr std::uint32_t  kMenuBufferRhi       = 0x10;
inline constexpr std::uint32_t  kMenuBufferCapacity  = 0x18;  // u32
inline constexpr std::uint8_t   kBeginInitResourcePrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89,
                                                              0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x55, 0x41, 0x54,
                                                              0x41, 0x55, 0x41, 0x56};
inline constexpr FunctionFact   kBeginInitResource{L"BeginInitResource", 0x1408F26F0,  // void (FRenderResource*)
                                                 kBeginInitResourcePrologue};

// EndUI__ContainScaleFor1920x1080: the UI scale of a rect relative to 1920x1080 (RE-p2-hud-view-levers.md).
// float(FVector2D) with the size packed in rcx (X low, Y high), the result in xmm0; a leaf that
// reads only its argument and .rdata constants, so any thread may call it.
inline constexpr std::uint8_t kContainScaleBytes[] = {
    0xC5, 0xFA, 0x6F, 0x1D, 0x70, 0x1D, 0x34, 0x05,  // vmovdqu xmm3, [abs mask]
    0xC5, 0xFA, 0x10, 0x25, 0x58, 0x1D, 0x34, 0x05,  // vmovss xmm4, [1e-8]
    0x48, 0x89, 0x4C, 0x24, 0x10,                    // mov [rsp+10h], rcx
    0xC5, 0xFA, 0x10, 0x54, 0x24, 0x10};             // vmovss xmm2, [rsp+10h]
inline constexpr FunctionFact kContainScale{L"EndUI::ContainScaleFor1920x1080", 0x140BC7998, kContainScaleBytes};

// The overlay singleton (SEndRootOverlay__GetOrCreate 0x1410EAB8C) and its marker host set: the block each overlay render
// writes for the view it renders, and the marker projection reads (EndUI__ProjectWorldToRootOverlay and its twin are its
// only readers found; RE-p2-hud.md, the projection's formula). Same offsets in 1.005 (RE-1005-offsets-A.md).
inline constexpr std::uintptr_t kRootOverlaySingleton = 0x1490099F8;
inline constexpr std::uint32_t  kOverlayLayoutMin     = 0x310;  // float x, y: the UI rect's Min (stored, not read)
inline constexpr std::uint32_t  kOverlayViewOrigin    = 0x318;  // float x, y: player 0's view rect's Min (0x140B0EF82)
inline constexpr std::uint32_t  kOverlayUiSize        = 0x320;  // float w, h: the UI rect's size (the band in a split)
inline constexpr std::uint32_t  kOverlayViewWidth     = 0x328;  // float: player 0's view size, cached before the rect
inline constexpr std::uint32_t  kOverlayViewHeight    = 0x32C;  // call (written at 0x140B0EF92)
inline constexpr std::uint32_t  kOverlayHostScale     = 0x330;  // float: the host scale (the mod's in a band)

// ---------------------------------------------------------------------------------------------
// The user widget (UUserWidget)
// ---------------------------------------------------------------------------------------------

// Its viewport layout (the stock ViewportAnchors, ViewportOffsets, ViewportAlignment),
// bound to each HUD root's SEndCanvas (RE-p2-hud.md; read live). Read only: changes
// go through the reflected Set*InViewport setters.
inline constexpr std::uint32_t kUserWidgetViewportAnchors   = 0x210;  // FAnchors, 16 bytes
inline constexpr std::uint32_t kUserWidgetViewportOffsets   = 0x220;  // FMargin, 16 bytes
inline constexpr std::uint32_t kUserWidgetViewportAlignment = 0x230;  // FVector2D, 8 bytes
inline constexpr std::uint32_t kUserWidgetViewportLayoutEnd = 0x238;

// Its render transform as UWidget keeps it (stock 4.26 FWidgetTransform and RenderTransformPivot, RE-hud-size-option.md):
// the translation and the scale (x, y floats each) and the pivot (a fraction of the widget's own size), which the root's
// SEndCanvas arranges it with. Read only: changes go through the reflected SetRenderScale and SetRenderTransformPivot.
inline constexpr std::uint32_t kWidgetRenderTranslation = 0x90;
inline constexpr std::uint32_t kWidgetRenderScale       = 0x98;
inline constexpr std::uint32_t kWidgetRenderPivot       = 0xAC;

/// UUserWidget::SetPositionInViewport(widget, FVector2D, bRemoveDPIScale): the stock setter the game's Blueprints
/// place widgets with (hud/position_probe) and the enemy gauges' placement calls.
inline constexpr std::uint8_t kSetPositionInViewportPrologue[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kSetPositionInViewport{L"UUserWidget::SetPositionInViewport", 0x140E8F54C,
                                                     kSetPositionInViewportPrologue};

/// FieldActionIconList__SortAndLayout(TArray<FWeakObjectPtr>* list, UObject* placeNow): sorts the given array by
/// priority and gives each live row the next slot in it (RE-p2-field-prompts.md). Called by the game only when a
/// row is added or removed.
inline constexpr std::uint8_t kSortAndLayoutBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55,
                                                       0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8,
                                                       0x20, 0x00};
inline constexpr FunctionFact kSortAndLayout{L"FieldActionIconList::SortAndLayout", 0x1409FB938, kSortAndLayoutBytes};

// ---------------------------------------------------------------------------------------------
// The game's message window (UEndMessageWindow; RE-message-window.md, both builds)
// ---------------------------------------------------------------------------------------------

/// Set before the game's close: the window's tick takes it off the overlay when its Out animation ends (the member
/// layout is the same in both builds).
inline constexpr std::uint32_t kMessageWindowRemoveAfterOut = 0x3D1;

// ---------------------------------------------------------------------------------------------
// A Slate widget's visibility, as the game sets it (RE-p2-hud-icon-manager.md, RE-lead-mark.md)
// ---------------------------------------------------------------------------------------------

/// SWidget::SetVisibility(widget, TAttribute<EVisibility>*): a widget's vtable slot 59; the game also calls it directly.
inline constexpr std::uint8_t kSetVisibilityBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kSetVisibility{L"SWidget::SetVisibility", 0x140DD3400, kSetVisibilityBytes};
inline constexpr std::size_t   kSetVisibilitySlot  = 59;
inline constexpr std::uint32_t kWidgetVisibility   = 0x1B0;  // the Visibility attribute: +0 value, +1 set
inline constexpr std::uint32_t kWidgetVisibilityDelegate = 0x1B8;  // its getter delegate (0: unbound)
inline constexpr std::uint8_t  kVisibilityHidden               = 0x02;
inline constexpr std::uint8_t  kVisibilitySelfHitTestInvisible = 0x11;

// ---------------------------------------------------------------------------------------------
// The map icons' shared store (RE-lead-mark.md, RE-walk-keep.md)
// ---------------------------------------------------------------------------------------------

/// EndUI_AddMapIcon(FName key, u8 kind, const FVector* position, const FString* text, const MapIconParams* params):
/// adds the key to the store every map view reads, or updates it in place; the views make their widgets on their next
/// tick. EndUI_RemoveMapIcon(FName key). Game thread.
inline constexpr std::uint8_t kAddMapIconBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                                    0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0xB8, 0x40, 0x00, 0x00};
inline constexpr FunctionFact kAddMapIcon{L"EndUI_AddMapIcon", 0x14197D8E8, kAddMapIconBytes};
inline constexpr std::uint8_t kRemoveMapIconBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0xE9,
                                                       0xC3, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
inline constexpr FunctionFact kRemoveMapIcon{L"EndUI_RemoveMapIcon", 0x14152D20C, kRemoveMapIconBytes};
inline constexpr std::uint8_t kMapIconKindMainStory = 2;  // the main objective's diamond (with its line) and pin
/// An area-map icon manager instance's icon map (a TMap's sparse array: 24-byte elements, the key at +0, the element
/// pointer at +8); an element's widget TSharedPtr at +0 (its reference controller at +8).
inline constexpr std::uint32_t kAreaIconMap        = 0x20;
inline constexpr std::uint32_t kAreaIconMapElement = 24;
inline constexpr std::uint32_t kAreaIconMapValue   = 8;
/// The minimap holder's entries (a TSet's sparse array: 160-byte elements, the key at +0, the SEndMapIcon at +0x10).
inline constexpr std::uint32_t kHolderEntries      = 0xE8;
inline constexpr std::uint32_t kHolderEntryElement = 160;
inline constexpr std::uint32_t kHolderEntryIcon    = 0x10;
inline constexpr std::uint32_t kSharedCount        = 0x8;  // a reference controller's shared count (int32)

// ---------------------------------------------------------------------------------------------
// The objective diamond's and the minimap pin's drawn size (RE-lead-marker-size.md)
// ---------------------------------------------------------------------------------------------

/// SWidget::SetAttribute on a widget's render transform (stock Slate): (widget, the widget's attribute, const new
/// attribute*, the invalidate reason the game's callers pass). It compares first: an unchanged value costs nothing.
inline constexpr std::uint8_t kSetRenderTransformBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                                            0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8};
inline constexpr FunctionFact kSetRenderTransform{L"SWidget_SetAttribute_RenderTransform", 0x140B127E0, kSetRenderTransformBytes};
inline constexpr std::uint8_t  kRenderTransformReason = 17;
/// A Slate widget's render-transform attribute (TAttribute<TOptional<FSlateRenderTransform>>, 48 bytes): the 2x2 matrix
/// at +0, the translation at +0x10, the optional's set byte at +0x18, the attribute's set byte at +0x1C, an unbound
/// getter delegate at +0x20 (its pointer) and +0x28 (its count).
inline constexpr std::uint32_t kWidgetRenderTransform  = 0x1D0;
inline constexpr std::uint32_t kRenderTransformSize    = 48;
inline constexpr std::uint32_t kRenderTransformOptSet  = 0x18;
inline constexpr std::uint32_t kRenderTransformAttrSet = 0x1C;
/// SEndMapIcon_SetIconScale(icon, FVector2D scale by value): the minimap pin's image scaled about its centre. The holder's
/// render loop writes a main-objective pin's scale on every run (1, or 0.5 while a kind-11 icon is up).
inline constexpr std::uint8_t kSetMapIconScaleBytes[] = {0x40, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x60, 0x00,
                                                         0x00, 0x00, 0xE8, 0x69, 0x9B, 0x65, 0x01};
inline constexpr FunctionFact kSetMapIconScale{L"SEndMapIcon_SetIconScale", 0x140B12008, kSetMapIconScaleBytes};
/// SEndFieldQuestIcon, the field diamond with its line under it (one per key in each view's icon manager): its vtable,
/// for the type check. Its diamond SImage and SEndMapIcon's pin image are both held at kIconImage; the diamond is laid
/// out kQuestIconSize wide and tall, its bottom on the line.
inline constexpr std::uintptr_t kFieldQuestIconVTable = 0x14569EA60;
inline constexpr std::uint32_t  kIconImage            = 0x3B0;
inline constexpr float          kQuestIconSize        = 24.0f;

// ---------------------------------------------------------------------------------------------
// The UI filters and the low-health red edge (RE-p2-command-menu-tint.md, RE-low-health-effect.md)
// ---------------------------------------------------------------------------------------------

/// The UI filter manager's latch over a reference count, per filter type: EnableType(world, type) sets the type's one
/// latch and counts it once; DisableType(type) clears it (type 1's also clears type 6's). RefInc/RefDec(manager, type)
/// count a holder of the filter's own, as the game's other menus share one.
inline constexpr std::uint8_t kFilterOnBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x60, 0xE7, 0xAD, 0x00};
inline constexpr std::uint8_t kFilterOffBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x88, 0xD9, 0xAD, 0x00};
inline constexpr std::uint8_t kRefIncBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                                   0x00, 0x00, 0x00, 0xE8, 0x24, 0xE7, 0xAD, 0x00};
inline constexpr std::uint8_t kRefDecBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                                   0x00, 0x00, 0x00, 0xE8, 0x0C, 0xD9, 0xAD, 0x00};
inline constexpr FunctionFact kFilterOn{L"EndUIFilter_EnableType", 0x14168D414, kFilterOnBytes};
inline constexpr FunctionFact kFilterOff{L"EndUIFilter_DisableType", 0x14168E1EC, kFilterOffBytes};
inline constexpr FunctionFact kRefInc{L"EndUIFilter_RefInc", 0x14168D44C, kRefIncBytes};
inline constexpr FunctionFact kRefDec{L"EndUIFilter_RefDec", 0x14168E264, kRefDecBytes};
inline constexpr std::uint32_t kFilterLatch = 0x1C;  // the manager's u8 latch per type
inline constexpr std::uint32_t kFilterCount = 0x2C;  // its i32 count per type

/// The low-health decision, each command controller's tick: any battle member at or below the danger share of its HP
/// sets the controller's danger to Dying (filter type 1, the red edge); a member in the stone state to Stone (type 6,
/// white; the petrify state) unless one is low. Gated by the controller's set-up and allowed bytes; a change of its danger
/// value (only while its DyingStone widget exists) turns the old type's filter off and the new one's on. Its HP read is
/// one call (facts/party.h kPartyHp).
inline constexpr std::uint8_t kDangerDecisionBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                        0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr FunctionFact kDangerDecision{L"FieldCommandCtrl_Tick_PinchFilter650", 0x140C2A164, kDangerDecisionBytes};
inline constexpr std::uintptr_t kDangerHpCall          = 0x140C2A1FD;  // the decision's HP read
inline constexpr std::uintptr_t kDangerDyingOnCall     = 0x14259FEC2;  // its EnableType(1)
inline constexpr std::uintptr_t kDangerStoneOnCall     = 0x14259FECB;  // its EnableType(6)
inline constexpr std::uintptr_t kDangerDyingOffCall    = 0x14259FE80;  // its DisableType(1)
inline constexpr std::uintptr_t kDangerStoneOffCall    = 0x14259FE89;  // its DisableType(6)
inline constexpr std::uint32_t  kCmdDanger             = 0x650;  // i32: 0 none, 1 Dying, 4 Stone
inline constexpr std::uint32_t  kCmdDangerAllowed      = 0x77C;  // u8: an event hides the edge here, on player 1's only
inline constexpr std::int32_t   kDangerDying           = 1;
inline constexpr std::int32_t   kDangerStone           = 4;
inline constexpr std::uint8_t   kFilterDying           = 1;
inline constexpr std::uint8_t   kFilterStone           = 6;
inline constexpr std::int32_t   kNeverLowHp            = 0x7FFFFFFF;  // an HP no danger share reaches

/// The screen-filter slot (EndUISystem[24], a pointer): the edge's intensity fades linearly to its target, run every HUD
/// tick by the slot's tick (slot, frame time), which hands the value to the world's screen-effect registry only through
/// the slot's weak world (zero in a copy: a copy only fades). Arming as the game's setter does: start = now, the target,
/// elapsed 0, the length.
inline constexpr std::uint32_t kUIScreenFilter = 0xC0;
inline constexpr std::uint8_t kFilterTickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C,
                                                    0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x70};
inline constexpr FunctionFact kFilterTick{L"EndUI__MC0_Lerps_GameModeDt", 0x14114E8CC, kFilterTickBytes};
inline constexpr std::uintptr_t kFilterTickCall    = 0x140F75AC0;  // MasterHUD_Tick's one call
inline constexpr std::uint32_t  kFilterSlotSize    = 0xC8;
inline constexpr std::uint32_t  kFilterIntensity   = 0x9C;
inline constexpr std::uint32_t  kFilterFadeStart   = 0xB4;
inline constexpr std::uint32_t  kFilterFadeTarget  = 0xB8;
inline constexpr std::uint32_t  kFilterFadeElapsed = 0xBC;
inline constexpr std::uint32_t  kFilterFadeLength  = 0xC0;
inline constexpr float          kDyingIntensity    = 1.0f;  // the Dying filter's intensity when on
inline constexpr float          kDyingFadeSeconds  = 0.5f;  // its fade's length, in and out

/// The registry's screen-filter functor: its apply (vtable slot, the one function for this functor class) writes the
/// view's filter fields from its own, each blended by the weight: (functor, the view's settings at kViewSettings,
/// weight). Its "on" float and its intensity; the object's size, copied whole for a view's own value.
inline constexpr std::uintptr_t kFilterApplySlot  = 0x145E66720;
inline constexpr std::uint8_t   kFilterApplyBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8};
inline constexpr FunctionFact   kFilterApply{L"ScreenFilterFunctor_ApplyToView", 0x144F1F7E4, kFilterApplyBytes};
inline constexpr std::uint32_t  kFunctorOn        = 0x0C;
inline constexpr std::uint32_t  kFunctorIntensity = 0x14;
inline constexpr std::uint32_t  kFunctorSize      = 0x38;
/// FSceneView: the settings the registry is applied to, and the view's player (its local player's controller id; -1
/// for a scene capture).
inline constexpr std::uint32_t kViewSettings    = 0xF10;
inline constexpr std::uint32_t kViewPlayerIndex = 0x228;

}  // namespace lss::facts
