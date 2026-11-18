#pragma once

// The stock engine's facts: what the mod relies on in Unreal's own code and objects, with the bytes that prove
// each one at startup.
//
// Addresses are written as IDA shows them (image base 0x140000000) so they can be looked up in the research
// documents directly; platform::GameImage rebases them (and translates them for a later build). Before anything
// is installed, the mod checks every fact in kStartupFacts against the running game. If any differs (a game
// patch, another mod), the mod installs nothing. A fact a single feature needs is checked where that feature
// installs, with platform::FindFunction, and the feature alone stands aside.
//
// The facts directory holds every fact two or more modules share, one declaration per address, grouped by the
// engine object it belongs to; a fact one module reads stays in that module. The host test tests/facts_once
// enforces the rule.
//
// Research: Rebirth/Documentation/research/RE-onefamily-feasibility.md (the view-family submission),
// RE-additional-view-family-reader.md (the final output's load action), RE-remove-local-player.md (player
// removal), RE-local-player-viewpoint.md, RE-vertical-split-and-fill.md, RE-game-window-reshape.md. Engine source
// for comparison: FF7RebirthEngineV4 (stock UE4; it differs from the shipped game at the load action).

#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// ---------------------------------------------------------------------------------------------
// The view-family submission (RE-onefamily-feasibility.md)
// ---------------------------------------------------------------------------------------------

/// FRendererModule::BeginRenderingViewFamily: renders one view family.
inline constexpr std::uintptr_t kBeginRenderingViewFamily = 0x1409E9CBC;

/// The IRendererModule vtable entry through which every caller reaches it (vtable 0x14698B490,
/// entry 8). The mod replaces this entry.
inline constexpr std::uintptr_t kBeginRenderingViewFamilyEntry = 0x1468101B0;

/// GFrameNumber (data, uint32): stepped once per frame by PostRenderAllViewports. Read by the HUD trace's frame line
/// and held one back for the seats' HUD passes (hud/passes/column_hud_images, RE-damage-numbers-per-view.md). Its
/// 1.004 twin is carried by the hand-found table: the generated data range around FApp::DeltaTime would put it 0x1C
/// bytes off.
inline constexpr std::uintptr_t kFrameNumber = 0x146DDD224;

/// The single call in UGameViewportClient::Draw (0x1409EDD88) that submits the game's view
/// family: `call r9`. Its return address identifies the game viewport among all callers
/// (UMG widgets submit their own families through the same entry).
inline constexpr std::uint8_t   kDrawSubmitCallBytes[] = {0x41, 0xFF, 0xD1};  // call r9
inline constexpr FunctionFact   kDrawSubmitCall{L"Draw's view-family submission call", 0x1409EEB6A,
                                              kDrawSubmitCallBytes};
inline constexpr std::uintptr_t kDrawSubmitReturn = 0x1409EEB6D;

/// BeginRenderingViewFamily skips Scene->IncrementFrameNumber() when family+0x70 is nonzero:
/// `cmp [rbx+70h], r12d` (rbx = the family). This is the instruction that makes +0x70 mean what
/// renderer/scene_view_family.h says it means.
inline constexpr std::uint8_t kFrameNumberGateBytes[] = {0x44, 0x39, 0x67, 0x70};
inline constexpr FunctionFact kFrameNumberGate{L"the frame-number gate on family+0x70", 0x1409E9D7A,
                                               kFrameNumberGateBytes};

/// The FSceneViewFamily constructor (0x1410BE634) storing bAdditionalViewFamily to family+0x74:
/// `mov al,[rbx+68h]; and al,cl; mov [rdi+74h],al` (cl = 1). The shipped game never reads the
/// field; the mod restores its stock reader below.
inline constexpr std::uint8_t kAdditionalFamilyStoreBytes[] = {0x8A, 0x43, 0x68, 0x22, 0xC1, 0x88, 0x47, 0x74};
inline constexpr FunctionFact kAdditionalFamilyStore{L"the bAdditionalViewFamily store to family+0x74", 0x1410BE6B7,
                                                     kAdditionalFamilyStoreBytes};

// ---------------------------------------------------------------------------------------------
// The final output's load action (RE-additional-view-family-reader.md)
// ---------------------------------------------------------------------------------------------

/// Inside AddPostProcessingPasses (0x14118C5CC), the inlined CreateViewFamilyOutput, from the first field copy out
/// of the view (r14) through the store of the load action to [rbp+0E0h]. Checked whole, so nothing between the
/// family load and the decision can have changed: 0x14118C65D mov eax,[r14+238h] ... 0x14118C699 mov rax,[r14]
/// (the family, the scene renderer's copy); vmovdqu; mov rcx,[rax]; cmp [rcx],r14 (Views[0] == View);
/// mov rcx,[r13+10h]; sete al; add al,dil (edi = 1); mov [rbp+0E0h],al.
inline constexpr std::uint8_t kOutputLoadActionSequenceBytes[] = {
    0x41, 0x8B, 0x86, 0x38, 0x02, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00, 0x00,
    0x89, 0x85, 0xD0, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xD4, 0x41, 0x8B, 0x86,
    0x3C, 0x02, 0x00, 0x00, 0x89, 0x85, 0xD4, 0x00, 0x00, 0x00, 0x41, 0x8B,
    0x86, 0x40, 0x02, 0x00, 0x00, 0x89, 0x85, 0xD8, 0x00, 0x00, 0x00, 0x41,
    0x8B, 0x86, 0x44, 0x02, 0x00, 0x00, 0x89, 0x85, 0xDC, 0x00, 0x00, 0x00,
    0x49, 0x8B, 0x06, 0xC5, 0xFA, 0x7F, 0x75, 0xB8, 0x48, 0x8B, 0x08, 0x4C,
    0x39, 0x31, 0x49, 0x8B, 0x4D, 0x10, 0x0F, 0x94, 0xC0, 0x40, 0x02, 0xC7,
    0x88, 0x85, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x85, 0xB0, 0x02, 0x00,
    0x00, 0x48, 0x89, 0x45, 0xF0, 0x48, 0x89, 0x45, 0xB0, 0xE8, 0x59, 0xF8,
    0x4D, 0x00, 0x48, 0x89, 0x85, 0x20, 0x02, 0x00, 0x00, 0x4C, 0x8D, 0x05,
    0x37, 0x2B, 0xC4, 0x04, 0x49};
inline constexpr FunctionFact kOutputLoadActionSequence{L"the final output's load-action decision", 0x14118C65D,
                                                        kOutputLoadActionSequenceBytes};

/// The store of the computed load action (al = 2 clear if first in the family, 1 load otherwise),
/// `mov [rbp+0E0h], al`, which the mod replaces; and the instruction after it, where execution
/// resumes. A single instruction is replaced so that no thread can be stopped inside the replaced
/// bytes. The flags and al are dead after the store (0x14118C6B7 reloads rax; a call follows).
inline constexpr std::uintptr_t kOutputLoadActionStore        = 0x14118C6B1;
inline constexpr std::uint8_t   kOutputLoadActionStoreBytes[] = {0x88, 0x85, 0xE0, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kOutputLoadActionResume       = 0x14118C6B7;

/// Game version 1.004 (RE-additional-view-family-reader.md): the store is `mov [rsp+610h], al`, 7 bytes, so resume
/// is store + 7. Both addresses translate through the 1.004 table; these bytes and renderer/output_load_action_code.h's
/// 1.004 code replace the source build's there.
inline constexpr std::uint8_t kOutputLoadActionStoreBytes1004[] = {0x88, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00};

/// With frame generation active, the tonemap of the final output pass binds two more colour targets beside the back
/// buffer ("WithoutHUD" and "HUD", the HUD-less colour and UI that DLSS-G reads), with this store of their load action:
/// `mov byte [rsp+40h], 2` (clear), one 5-byte instruction inside one aligned 16-byte block (research/
/// RE-p2-framegen-3.md). The D3D12 RHI clears every bound target when any one is set to clear, so a later family's
/// tonemap clears the whole back buffer despite its own load action. The mod writes 1 (load). A first family's back
/// buffer clears anyway, which clears these too, so the change shows only in a later family.
inline constexpr std::uintptr_t kTonemapExtraTargetsLoadStore     = 0x140C16FA5;
inline constexpr std::uint8_t   kTonemapExtraTargetsClearBytes[] = {0xC6, 0x44, 0x24, 0x40, 0x02};
inline constexpr std::uint8_t   kTonemapExtraTargetsLoadBytes[]  = {0xC6, 0x44, 0x24, 0x40, 0x01};

// ---------------------------------------------------------------------------------------------
// A local player's view point (RE-local-player-viewpoint.md)
// ---------------------------------------------------------------------------------------------

/// ULocalPlayer::GetViewPoint(FMinimalViewInfo& Out, EStereoscopicPass): where each player's view
/// is. Stock: the controller's camera-manager cache, so a player with a borrowed controller gets
/// the lender's view. Called only virtually, twice per player per frame (for the view matrix and
/// for CalcSceneView's location/rotation), through the ULocalPlayer vtable (0x146A18768) entry
/// at +0x278. No native subclass overrides it. Its prologue, up to the stack-probe call whose target is relative.
inline constexpr std::uintptr_t kLocalPlayerVTable = 0x146A18768;
inline constexpr std::uintptr_t kGetViewPointEntry = 0x146A189E8;
inline constexpr std::uint8_t   kGetViewPointPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xF9, 0xFF, 0xFF, 0xB8, 0x40, 0x07, 0x00, 0x00, 0xE8};
inline constexpr FunctionFact kGetViewPoint{L"ULocalPlayer::GetViewPoint", 0x1409D4708, kGetViewPointPrologue};

// ---------------------------------------------------------------------------------------------
// Removing a local player (RE-remove-local-player.md)
// ---------------------------------------------------------------------------------------------

/// UGameInstance::RemoveLocalPlayer(ULocalPlayer*). Stock GameInstance.cpp:798. Reached through
/// the game instance's vtable at +0x2E8; no FF7R game-instance class overrides it. 1.005 inserted a UObject virtual
/// at slot 19, so the entry is +0x2F0 there (RE-1005-core-facts.md). Its prologue, up to the stack-probe call.
inline constexpr std::uint8_t kRemoveLocalPlayerPrologue[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70};
inline constexpr FunctionFact kRemoveLocalPlayer{L"UGameInstance::RemoveLocalPlayer", 0x144D695A8,
                                                 kRemoveLocalPlayerPrologue};
inline constexpr platform::PerBuild<std::uint32_t> kRemoveLocalPlayerVTableOffset = {0x2F0, 0x2E8};
/// UObject's GetWorld virtual (1.005's UObject vtables are one slot longer; RE-p2-trigger-overlap.md,
/// RE-coop-rows-facts.md).
inline constexpr platform::PerBuild<std::uint32_t> kObjectGetWorldSlot = {0x168, 0x160};

/// Right after the probe call: `sub rsp,rax; mov rsi,rcx; mov [rsp+20h],rdx;
/// mov rcx,[rdx+30h]; mov rbx,rdx; test rcx,rcx; jz` (the jz is at 0x144F56217). It reads the
/// player's PlayerController at +0x30 and skips all controller cleanup when it is null.
/// That skip is what makes removing a seat's player safe once its borrowed controller is cleared.
inline constexpr std::uint8_t kRemoveLocalPlayerControllerTestBytes[] = {
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xDA, 0x48, 0x8B,
    0x4A, 0x30, 0x48, 0x85, 0xC9, 0x74, 0x5B, 0x80, 0xB9, 0xF8, 0x00};
inline constexpr FunctionFact kRemoveLocalPlayerControllerTest{L"RemoveLocalPlayer's null-controller skip", 0x144D695C6,
                                                               kRemoveLocalPlayerControllerTestBytes};

/// ULocalPlayer::PlayerController, as RemoveLocalPlayer reads it. The shared camera writes this
/// field through reflection and refuses if reflection reports any other offset.
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;

// ---------------------------------------------------------------------------------------------
// The split layout and each half's field of view (RE-vertical-split-and-fill.md)
// ---------------------------------------------------------------------------------------------
//
// Both are set through reflection by name. The offsets are what the engine's own code reads, so a
// reflected offset that differs means reflection and the code disagree, and the mod refuses.

/// UGameMapsSettings::TwoPlayerSplitscreenLayout (a byte; 0 top/bottom, 1 side by side), read
/// from the class default object on every call by UpdateActiveSplitscreenType (0x1410A1340).
/// Reflection data 0x1463E88E8.
inline constexpr std::uint32_t kTwoPlayerSplitscreenLayoutOffset = 0x51;

/// UGameMapsSettings::ThreePlayerSplitscreenLayout (a byte; 0 favour top, 1 favour bottom, 2 three columns,
/// 3 three rows): the byte after TwoPlayerSplitscreenLayout (UE 4.26 GameMapsSettings.h), read by the same
/// function once there are three local players. Reflection confirms the offset before any write
/// (the three equal columns of views/columns.h, held by mod.cpp while three local players are listed).
inline constexpr std::uint32_t kThreePlayerSplitscreenLayoutOffset = 0x52;

/// ULocalPlayer::AspectRatioAxisConstraint (a byte), read by GetProjectionData after GetViewPoint:
/// `mov dl,[r14+94h]` at 0x140CC0B1D. Reflection data 0x1464B6818.
inline constexpr std::uint32_t kAspectRatioAxisConstraintOffset = 0x94;

// ---------------------------------------------------------------------------------------------
// The engine's layout table (RE-splitscreen-layout-table.md; views/column_layout.h)
// ---------------------------------------------------------------------------------------------
//
// Stock UE 4.26 in both builds, no Square Enix override: UGameViewportClient::SplitscreenInfo is a TArray of ten
// FSplitscreenData (one per ESplitScreenType, in the enum's order), each a TArray of FPerPlayerSplitscreenData
// records {SizeX, SizeY, OriginX, OriginY}, filled once by the viewport client's constructor and read every redraw
// by LayoutPlayers (record i to the local player at place i), with nothing cached in between. The GameViewport
// pointer is where RedrawViewports reads it.

/// UGameEngine::GameViewport (UGameViewportClient*).
inline constexpr std::uint32_t kEngineGameViewport = 0x7E0;

/// UGameViewportClient::SplitscreenInfo: the TArray's data pointer (its Num follows at +kTArrayNum).
inline constexpr std::uint32_t kViewportClientSplitscreenInfo = 0x60;
inline constexpr std::uint32_t kTArrayNum                     = 0x08;
inline constexpr std::uint32_t kTArrayMax                     = 0x0C;  // a TArray's capacity (i32)

/// The constructor's ten entries; an entry (FSplitscreenData) and a record (FPerPlayerSplitscreenData) are 16 bytes
/// each.
inline constexpr std::int32_t  kSplitscreenTypes     = 10;
inline constexpr std::uint32_t kSplitscreenDataBytes = 0x10;
inline constexpr std::uint32_t kPerPlayerSplitBytes  = 0x10;

/// The entries the mod writes: ESplitScreenType TwoPlayer_Horizontal (top and bottom), TwoPlayer_Vertical (side by
/// side) and ThreePlayer_Vertical (three columns: the layout kThreePlayerSplitscreenLayoutOffset's byte names as 2).
inline constexpr std::uint32_t kSplitTypeTwoPlayerHorizontal = 1;
inline constexpr std::uint32_t kSplitTypeTwoPlayerVertical   = 2;
inline constexpr std::uint32_t kSplitTypeThreePlayerVertical = 5;

// ---------------------------------------------------------------------------------------------
// The game window (RE-game-window-reshape.md; views/window_span.h, the multi-monitor branch)
// ---------------------------------------------------------------------------------------------
//
// Stock UE 4.26 throughout, the same offsets in 1.004 and 1.005. The chain the engine's own readers walk
// (UKismetSystemLibrary::SetWindowTitle, UGameEngine::OnViewportResized, EndUI::GetWindowMonitorDpiScale):
// GEngine -> UGameEngine::GameViewportWindow (TWeakPtr<SWindow>) -> SWindow::NativeWindow (TSharedPtr<FGenericWindow>)
// -> the FWindowsWindow vtable, 37 entries in the stock order (GenericWindow.h).

/// The GEngine global (data).
inline constexpr std::uintptr_t kGEngine = 0x148F4B580;

/// UKismetSystemLibrary::SetWindowTitle: the reader whose code proves the two offsets below. Its prologue.
inline constexpr std::uint8_t kSetWindowTitlePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                           0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kSetWindowTitle{L"UKismetSystemLibrary::SetWindowTitle", 0x144DEB75C,
                                              kSetWindowTitlePrologue};

/// UGameEngine::GameViewportWindow: the SWindow pointer (the weak referencer follows at +0xE60).
inline constexpr std::uint32_t kGameViewportWindowOffset = 0xE58;

/// SWindow::NativeWindow: the FGenericWindow pointer (its reference controller follows at +0x930).
inline constexpr std::uint32_t kSWindowNativeWindowOffset = 0x928;

/// SWindow::SizeLimits (FWindowSizeLimits: four TOptional<float>, a float then its IsSet byte), read by the WM_GETMINMAXINFO
/// handler for the OS's limit on the window's size; the game window's MaxWidth and MaxHeight are set once, by
/// UGameEngine::CreateGameWindow, from the desktop's size when the game starts in the Windowed mode, and never again
/// (RE-window-size-limits.md).
inline constexpr std::uint32_t kSWindowSizeLimitsOffset = 0x904;
inline constexpr std::uint32_t kSizeLimitsMaxWidth      = 0x10;
inline constexpr std::uint32_t kSizeLimitsMaxHeight     = 0x18;
inline constexpr std::uint32_t kOptionalIsSet           = 0x04;  // the IsSet byte after a TOptional<float>'s value

/// The FWindowsWindow vtable (data) and the slots the mod calls, with the functions they must hold:
/// ReshapeWindow (WindowsWindow.cpp:411), GetWindowMode (`mov eax,[rcx+38h]; ret`) and GetOSWindowHandle
/// (`mov rax,[rcx+28h]; ret`).
inline constexpr std::uintptr_t kWindowsWindowVTable                = 0x1467E3290;
inline constexpr std::size_t    kWindowsWindowSlotReshapeWindow     = 1;
inline constexpr std::size_t    kWindowsWindowSlotGetWindowMode     = 13;
inline constexpr std::size_t    kWindowsWindowSlotGetOSWindowHandle = 24;
inline constexpr std::uint8_t   kWindowsWindowReshapeWindowPrologue[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                                                       0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE1};
inline constexpr FunctionFact   kWindowsWindowReshapeWindow{L"FWindowsWindow::ReshapeWindow", 0x14455722C,
                                                          kWindowsWindowReshapeWindowPrologue};
inline constexpr std::uint8_t   kWindowsWindowGetWindowModePrologue[] = {0x8B, 0x41, 0x38, 0xC3};
inline constexpr FunctionFact   kWindowsWindowGetWindowMode{L"FWindowsWindow::GetWindowMode", 0x141959170,
                                                          kWindowsWindowGetWindowModePrologue};
inline constexpr std::uint8_t   kWindowsWindowGetOSWindowHandlePrologue[] = {0x48, 0x8B, 0x41, 0x28, 0xC3};
inline constexpr FunctionFact   kWindowsWindowGetOSWindowHandle{L"FWindowsWindow::GetOSWindowHandle", 0x141958710,
                                                              kWindowsWindowGetOSWindowHandlePrologue};

// ---------------------------------------------------------------------------------------------
// Actors, pawns, local players and the viewport (stock UE4 fields; RE-p2-build-facts-ida.md,
// RE-p2-own-controller-ida.md)
// ---------------------------------------------------------------------------------------------

// UObject
inline constexpr std::uint32_t kObjectInternalIndex = 0x0C;  // UObjectBase::InternalIndex: its GUObjectArray slot
inline constexpr std::uint32_t kObjectOuter         = 0x20;  // UObjectBase::OuterPrivate (an actor's level, a level's world)
// UActorComponent
inline constexpr std::uint32_t kActorComponentOwner = 0xA0;  // UActorComponent::OwnerPrivate
// UWorld
/// PlayerControllerList, a TArray of weak controllers (the data, then the count): its first entry whose player is
/// local is "the first local player" (UWorld::GetFirstLocalPlayerFromControllers), player 1's (RE-p2-hud.md).
inline constexpr std::uint32_t kWorldPlayerControllers     = 0x1F0;
inline constexpr std::uint32_t kWorldPlayerControllerCount = 0x1F8;
// AActor
inline constexpr std::uint32_t kActorOwner         = 0xE8;   // AActor::SetOwner stores it
inline constexpr std::uint32_t kActorRootComponent = 0x138;  // AActor::RootComponent
inline constexpr std::uint32_t kActorTagsData  = 0x178;  // Tags.GetData()
inline constexpr std::uint32_t kActorTagsCount = 0x180;  // Tags.Num()
inline constexpr std::uint32_t kPawnController = 0x260;  // APawn::Controller
// USceneComponent
inline constexpr std::uint32_t kSceneComponentAttachParent = 0xC0;  // AttachParent (UE4SS's header dump)
// ULocalPlayer
inline constexpr std::uint32_t kLocalPlayerViewportClient = 0x70;
/// Origin and Size (two FVector2D: the view's fractions of the viewport), as LayoutPlayers writes them from the
/// layout table and GetProjectionData reads them (RE-splitscreen-layout-table.md): the view's shape for the split FOV.
inline constexpr std::uint32_t kLocalPlayerOrigin       = 0x78;
inline constexpr std::uint32_t kLocalPlayerSize         = 0x80;
inline constexpr std::uint32_t kLocalPlayerControllerId   = 0xB8;
// UGameViewportClient: the four input-mode bytes (+0x361..+0x364) the controller's BeginPlay sets
// through SetInputMode on the one viewport all local players share.
inline constexpr std::uint32_t kViewportInputModeBytes = 0x361;

// FWeakObjectPtr
/// FWeakObjectPtr::Get and assignment from an object.
inline constexpr std::uint8_t kWeakObjectGetBytes[] = {
    0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45, 0x85, 0xC9, 0x74, 0x4F,
    0x8B, 0x01, 0x85, 0xC0, 0x78, 0x49, 0x3B, 0x05, 0x38, 0xA1, 0xEE, 0x07};
inline constexpr FunctionFact kWeakObjectGet{L"FWeakObjectPtr::Get", 0x1408349FC, kWeakObjectGetBytes};
inline constexpr std::uint8_t kWeakObjectAssignBytes[] = {
    0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
    0x20, 0x73, 0x93, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF1, 0x48, 0x85};
inline constexpr FunctionFact kWeakObjectAssign{L"FWeakObjectPtr assignment", 0x140834850, kWeakObjectAssignBytes};

// ---------------------------------------------------------------------------------------------
// Memory and smart pointers (stock UE4)
// ---------------------------------------------------------------------------------------------

/// FMemory::Malloc(size, alignment) and FMemory::Free(block): the game's allocator (GMalloc behind them), for memory
/// the game itself frees or that must be the game's (an array handed to a game function).
inline constexpr std::uint8_t kMallocBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                                0x00, 0x00, 0x00, 0xE8, 0xF0, 0xC1, 0x9C, 0x01};
inline constexpr FunctionFact kMalloc{L"FMemory::Malloc", 0x14079F980, kMallocBytes};
inline constexpr std::uint8_t kFreeBytes[] = {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA0, 0x03, 0x00, 0x00, 0x57, 0xB8, 0x50,
                                              0x00, 0x00, 0x00, 0xE8, 0x2C, 0x76, 0x7D, 0x00, 0x48, 0x2B, 0xE0, 0x4C};
inline constexpr FunctionFact kFree{L"FMemory::Free", 0x141994540, kFreeBytes};

/// The engine's array-data free (IDA: ME_FreeResourceBindingArray): `mov rcx,[rcx]; if (rcx) FMemory::Free(rcx)`
/// for the TArray at rcx. hud/minimap_labels calls it; world/anim_views redirects a call to it.
inline constexpr std::uint8_t kFreeArrayDataBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xCE, 0x6C,
                                                       0xA0, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x09};
inline constexpr FunctionFact kFreeArrayData{L"the array-data free (ME_FreeResourceBindingArray)", 0x140764EA8,
                                             kFreeArrayDataBytes};

/// UWorld::GetFirstPlayerController (entry `mov eax, 28h`).
inline constexpr std::uint8_t kGetFirstPlayerControllerBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kGetFirstPlayerController{L"UWorld::GetFirstPlayerController", 0x140F99D50,
                                                        kGetFirstPlayerControllerBytes};

/// Thread-safe shared pointers: TWeakPtr's release of its reference (rcx = the weak pointer; `mov rcx,[rcx+8]` the
/// controller) and the reference controller's ReleaseWeakReference (`lock xadd [rcx+0Ch], -1`).
inline constexpr std::uint8_t kWeakPtrReleaseBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x7E, 0x48, 0x7F, 0x01,
                                                        0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x49, 0x08};
inline constexpr FunctionFact kWeakPtrRelease{L"TWeakPtrTS::Release", 0x1409772F8, kWeakPtrReleaseBytes};
inline constexpr std::uint8_t kReleaseWeakReferenceBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x8A, 0x36,
                                                              0x81, 0x00, 0x48, 0x2B, 0xE0, 0x83, 0xC8, 0xFF,
                                                              0xF0, 0x0F, 0xC1, 0x41, 0x0C};
inline constexpr FunctionFact kReleaseWeakReference{L"TReferenceControllerTS::ReleaseWeakReference", 0x1419584EC,
                                                    kReleaseWeakReferenceBytes};

// ---------------------------------------------------------------------------------------------
// The platform application and the RHI (stock UE4)
// ---------------------------------------------------------------------------------------------

/// The platform application (a pointer global): +0x1E0 is the i32 device code of the last active input device (an
/// XInput pad k reads -2-3k), which force feedback goes to; its send interface is at +0x80.
inline constexpr std::uintptr_t kPlatformApplication   = 0x14907F8F0;
inline constexpr std::uint32_t  kPlatformAppDeviceCode = 0x1E0;

/// FRHICommandSubmitCommandsHint: its vtable (data) and the Execute in slot 0 (renderer/submit_hints,
/// renderer/submission_census).
inline constexpr std::uintptr_t kSubmitCommandsHintVTable  = 0x145AD4AB0;
inline constexpr std::uintptr_t kSubmitCommandsHintExecute = 0x140AE3E90;

// ---------------------------------------------------------------------------------------------
// The pads: XInput's import slots and the engine's XInput device (stock UE4; RE-pad-slots.md, both builds)
// ---------------------------------------------------------------------------------------------

/// The exe's import slots for xinput1_3's XInputGetState and XInputSetState (.idata, read-only): every pad read of
/// the game (the stock poll, Square's pad-type rescan and its last-active-pad check) and its one rumble send go
/// through them.
inline constexpr std::uintptr_t kXInputGetStateImport = 0x145569800;
inline constexpr std::uintptr_t kXInputSetStateImport = 0x1455697F8;

/// GWindowsApplication, the FWindowsApplication pointer; its XInputDevice at +0xD8, the external
/// input devices' array at +0xE8 (data, then Num at +0xF0).
inline constexpr std::uintptr_t kWindowsApplicationGlobal = 0x1491D8608;
inline constexpr std::uint32_t  kAppXInputInterface       = 0xD8;
inline constexpr std::uint32_t  kAppInputDevices          = 0xE8;

/// XInputDevice: the re-check byte (bNeedsControllerStateUpdate: the window procedure's WM_DEVICECHANGE sets it, the
/// poll clears it at its end; a slot the engine counts empty is polled only while it is set), the four slot records
/// (stride 0x108), each with its connected byte and the last motor values sent (rumble is sent when they change).
inline constexpr std::uint32_t kXInputNeedsStateUpdate     = 0x08;
inline constexpr std::uint32_t kXInputControllerStates     = 0x28;
inline constexpr std::uint32_t kXInputControllerStateBytes = 0x108;
inline constexpr std::uint32_t kXInputStateConnected       = 0xE2;
inline constexpr std::uint32_t kXInputStateLastLarge       = 0xFC;   // float
inline constexpr std::uint32_t kXInputStateLastSmall       = 0x100;  // float

// ---------------------------------------------------------------------------------------------
// The engine's per-player HUD pass (stock AHUD; RE-view-text.md, both builds)
// ---------------------------------------------------------------------------------------------

/// AHUD::PostRender: UGameViewportClient::Draw calls it through the AHUD vtable for each player's view, with the HUD's
/// canvas already set to that view's rect. Every controller's HUD is the plain AHUD (one table).
inline constexpr std::uint8_t   kHudPostRenderBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                                         0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0xB8, 0x80, 0x00, 0x00, 0x00};
inline constexpr FunctionFact   kHudPostRender{L"AHUD::PostRender", 0x1415C97FC, kHudPostRenderBytes};
/// Its vtable slot, vtbl_AHUD (0x146A051F8) + 0x6D0 (1.004: 0x146BACFD8 + 0x6C8: every UObject vtable grew one slot
/// after it).
inline constexpr platform::SlotFact kHudPostRenderSlot{L"AHUD vtable: PostRender", 0x146A058C8, kHudPostRender};
/// AHUD: PlayerOwner (the controller), Canvas (set only during PostRender); UCanvas: ClipX, ClipY (the view's size).
inline constexpr std::uint32_t kHudOwner         = 0x228;
inline constexpr std::uint32_t kHudCanvas        = 0x278;
inline constexpr std::uint32_t kCanvasClipX      = 0x30;
inline constexpr std::uint32_t kCanvasClipY      = 0x34;
/// UEngine::MediumFont (Roboto), read through GEngine.
inline constexpr std::uint32_t kEngineMediumFont = 0x70;

/// The PlayStation pad reader's device vtable (the SonyController plugin, scePad): not XInput, so
/// the pad table does not move its pads.
inline constexpr std::uintptr_t kSonyControllerDeviceVTable = 0x146432E10;

// ---------------------------------------------------------------------------------------------
// Slate (stock UE4)
// ---------------------------------------------------------------------------------------------

/// FGeometry (0x38 bytes): its size (FVector2D), its scale (float), and its absolute translation (FVector2D).
inline constexpr std::uint32_t kGeometryBytes       = 0x38;
inline constexpr std::uint32_t kGeometrySize        = 0x00;
inline constexpr std::uint32_t kGeometryScale       = 0x08;
inline constexpr std::uint32_t kGeometryTranslation = 0x2C;

// ---------------------------------------------------------------------------------------------
// The startup check list
// ---------------------------------------------------------------------------------------------

/// Checked before anything is installed; one failure and the mod installs nothing.
inline constexpr FunctionFact kStartupFacts[] = {
    kDrawSubmitCall,
    kFrameNumberGate,
    kAdditionalFamilyStore,
    kOutputLoadActionSequence,
    kGetViewPoint,
    kRemoveLocalPlayer,
    kRemoveLocalPlayerControllerTest,
};

}  // namespace lss::facts
