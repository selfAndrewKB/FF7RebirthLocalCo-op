#pragma once

// Every engine address the mod relies on, with the bytes that prove it at startup.
//
// Addresses are written as IDA shows them (image base 0x140000000) so they can be looked up in
// the research documents directly; platform::GameImage rebases them. Before anything is
// installed, the mod checks every fact in kVerifiedCode against the running game. If any
// differs (a game patch, another mod), the mod installs nothing.
//
// Research: Rebirth/Documentation/research/RE-onefamily-feasibility.md (the view-family
// submission), RE-additional-view-family-reader.md (the final output's load action) and
// RE-remove-local-player.md (player removal). Engine source for comparison: FF7RebirthEngineV4
// (stock UE4; it differs from the shipped game at the load action).

#include <cstdint>
#include <span>

namespace lss::engine {

// ---------------------------------------------------------------------------------------------
// The view-family submission (RE-onefamily-feasibility.md §Q1, §Q2)
// ---------------------------------------------------------------------------------------------

/// FRendererModule::BeginRenderingViewFamily: renders one view family.
inline constexpr std::uintptr_t kBeginRenderingViewFamily = 0x140BEE184;

/// The IRendererModule vtable entry through which every caller reaches it (vtable 0x14698B490,
/// entry 8). The mod replaces this entry.
inline constexpr std::uintptr_t kBeginRenderingViewFamilyEntry = 0x14698B4D0;

/// The single call in UGameViewportClient::Draw (0x140CC2194) that submits the game's view
/// family: `call r9`. Its return address identifies the game viewport among all callers
/// (UMG widgets submit their own families through the same entry).
inline constexpr std::uintptr_t kDrawSubmitCall   = 0x140CC2F86;
inline constexpr std::uintptr_t kDrawSubmitReturn = 0x140CC2F89;
inline constexpr std::uint8_t   kDrawSubmitCallBytes[] = {0x41, 0xFF, 0xD1};  // call r9

/// BeginRenderingViewFamily skips Scene->IncrementFrameNumber() when family+0x70 is nonzero:
/// `cmp [rbx+70h], r12d` (rbx = the family). This is the instruction that makes +0x70 mean what
/// renderer/scene_view_family.h says it means.
inline constexpr std::uintptr_t kFrameNumberGate        = 0x140BEE266;
inline constexpr std::uint8_t   kFrameNumberGateBytes[] = {0x44, 0x39, 0x63, 0x70};

/// The FSceneViewFamily constructor (0x140CC42D0) storing bAdditionalViewFamily to family+0x74:
/// `mov al,[rbx+68h]; and al,cl; mov [rdi+74h],al` (cl = 1). The shipped game never reads the
/// field; the mod restores its stock reader below.
inline constexpr std::uintptr_t kAdditionalFamilyStore        = 0x140CC4353;
inline constexpr std::uint8_t   kAdditionalFamilyStoreBytes[] = {0x8A, 0x43, 0x68, 0x22, 0xC1, 0x88, 0x47, 0x74};

// ---------------------------------------------------------------------------------------------
// The final output's load action (RE-additional-view-family-reader.md §Q1)
// ---------------------------------------------------------------------------------------------

/// Inside AddPostProcessingPasses (0x14111A238), the inlined CreateViewFamilyOutput, from
/// `mov rcx,[r12]` (rcx = View->Family, the scene renderer's copy) through the store of the load
/// action to [rsp+610h]. Checked whole, so nothing between the family load and the decision can
/// have changed rcx: 0x14111A301 mov rcx,[r12] ... 0x14111A363 mov rax,[rcx];
/// cmp [rax],r12 (Views[0] == View); vmovdqu; setz al; add al,1; mov [rsp+610h],al.
inline constexpr std::uintptr_t kOutputLoadActionSequence        = 0x14111A301;
inline constexpr std::uint8_t   kOutputLoadActionSequenceBytes[] = {
    0x49, 0x8B, 0x0C, 0x24, 0x74, 0x08, 0x49, 0x8D, 0x9C, 0x24, 0x38, 0x02,
    0x00, 0x00, 0xC5, 0xF8, 0x10, 0x03, 0x48, 0x89, 0x84, 0x24, 0xF8, 0x05,
    0x00, 0x00, 0x49, 0x8B, 0xD5, 0x41, 0x8B, 0x84, 0x24, 0x38, 0x02, 0x00,
    0x00, 0xC5, 0xFA, 0x7F, 0x84, 0x24, 0x00, 0x06, 0x00, 0x00, 0x89, 0x84,
    0x24, 0x00, 0x06, 0x00, 0x00, 0x41, 0x8B, 0x84, 0x24, 0x3C, 0x02, 0x00,
    0x00, 0x89, 0x84, 0x24, 0x04, 0x06, 0x00, 0x00, 0x41, 0x8B, 0x84, 0x24,
    0x40, 0x02, 0x00, 0x00, 0x89, 0x84, 0x24, 0x08, 0x06, 0x00, 0x00, 0x41,
    0x8B, 0x84, 0x24, 0x44, 0x02, 0x00, 0x00, 0x89, 0x84, 0x24, 0x0C, 0x06,
    0x00, 0x00, 0x48, 0x8B, 0x01, 0x4C, 0x39, 0x20, 0xC5, 0xFA, 0x7F, 0xBC,
    0x24, 0xC8, 0x01, 0x00, 0x00, 0x0F, 0x94, 0xC0, 0x04, 0x01, 0x88, 0x84,
    0x24, 0x10, 0x06, 0x00, 0x00};

/// The store of the computed load action (al = 2 clear if first in the family, 1 load otherwise),
/// `mov [rsp+610h], al`, which the mod replaces; and the instruction after it, where execution
/// resumes. A single instruction is replaced so that no thread can be stopped inside the replaced
/// bytes. The flags and al are dead after the store (0x14111A37E reloads rax; a call follows).
inline constexpr std::uintptr_t kOutputLoadActionStore        = 0x14111A377;
inline constexpr std::uint8_t   kOutputLoadActionStoreBytes[] = {0x88, 0x84, 0x24, 0x10, 0x06, 0x00, 0x00};
inline constexpr std::uintptr_t kOutputLoadActionResume       = 0x14111A37E;

// ---------------------------------------------------------------------------------------------
// A local player's view point (RE-local-player-viewpoint.md)
// ---------------------------------------------------------------------------------------------

/// ULocalPlayer::GetViewPoint(FMinimalViewInfo& Out, EStereoscopicPass): where each player's view
/// is. Stock: the controller's camera-manager cache, so a player with a borrowed controller gets
/// the lender's view. Called only virtually, twice per player per frame (for the view matrix and
/// for CalcSceneView's location/rotation), through the ULocalPlayer vtable (0x146BB33D8) entry
/// at +0x278. No native subclass overrides it.
inline constexpr std::uintptr_t kLocalPlayerVTable = 0x146BB33D8;
inline constexpr std::uintptr_t kGetViewPointEntry = 0x146BB3650;
inline constexpr std::uintptr_t kGetViewPoint      = 0x140BF1270;

/// Its prologue, up to the stack-probe call whose target is relative.
inline constexpr std::uint8_t kGetViewPointPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xF9, 0xFF, 0xFF, 0xB8, 0x40, 0x07, 0x00, 0x00, 0xE8};

// ---------------------------------------------------------------------------------------------
// Removing a local player (RE-remove-local-player.md §Q1)
// ---------------------------------------------------------------------------------------------

/// UGameInstance::RemoveLocalPlayer(ULocalPlayer*). Stock GameInstance.cpp:798. Reached through
/// the game instance's vtable at +0x2E8; no FF7R game-instance class overrides it.
inline constexpr std::uintptr_t kRemoveLocalPlayer            = 0x144F561F0;
inline constexpr std::uint32_t  kRemoveLocalPlayerVTableOffset = 0x2E8;

/// Its prologue, up to the stack-probe call whose target is relative.
inline constexpr std::uint8_t kRemoveLocalPlayerPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8};

/// Right after the probe call: `sub rsp,rax; mov rsi,rcx; mov [rsp+20h],rdx;
/// mov rcx,[rdx+30h]; mov rbx,rdx; test rcx,rcx; jz` (the jz is at 0x144F56217). It reads the
/// player's PlayerController at +0x30 and skips all controller cleanup when it is null.
/// That skip is what makes removing player 2 safe once its borrowed controller is cleared.
inline constexpr std::uintptr_t kRemoveLocalPlayerControllerTest = 0x144F56202;
inline constexpr std::uint8_t   kRemoveLocalPlayerControllerTestBytes[] = {
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xF1, 0x48, 0x89, 0x54, 0x24, 0x20,
    0x48, 0x8B, 0x4A, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9, 0x74};

/// ULocalPlayer::PlayerController, as RemoveLocalPlayer reads it. The shared camera writes this
/// field through reflection and refuses if reflection reports any other offset.
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;

// ---------------------------------------------------------------------------------------------
// The split layout and each half's field of view (RE-vertical-split-and-fill.md §Q1, §Q3)
// ---------------------------------------------------------------------------------------------
//
// Both are set through reflection by name. The offsets are what the engine's own code reads, so a
// reflected offset that differs means reflection and the code disagree, and the mod refuses.

/// UGameMapsSettings::TwoPlayerSplitscreenLayout (a byte; 0 top/bottom, 1 side by side), read
/// from the class default object on every call by UpdateActiveSplitscreenType (0x1410A1340).
/// Reflection data 0x1463E88E8.
inline constexpr std::uint32_t kTwoPlayerSplitscreenLayoutOffset = 0x51;

/// ULocalPlayer::AspectRatioAxisConstraint (a byte), read by GetProjectionData after GetViewPoint:
/// `mov dl,[r14+94h]` at 0x140CC0B1D. Reflection data 0x1464B6818.
inline constexpr std::uint32_t kAspectRatioAxisConstraintOffset = 0x94;

// ---------------------------------------------------------------------------------------------
// The startup check list
// ---------------------------------------------------------------------------------------------

struct VerifiedCode
{
    const wchar_t*                what;
    std::uintptr_t                idaAddress;
    std::span<const std::uint8_t> bytes;
};

inline constexpr VerifiedCode kVerifiedCode[] = {
    {L"Draw's view-family submission call", kDrawSubmitCall, kDrawSubmitCallBytes},
    {L"the frame-number gate on family+0x70", kFrameNumberGate, kFrameNumberGateBytes},
    {L"the bAdditionalViewFamily store to family+0x74", kAdditionalFamilyStore, kAdditionalFamilyStoreBytes},
    {L"the final output's load-action decision", kOutputLoadActionSequence, kOutputLoadActionSequenceBytes},
    {L"ULocalPlayer::GetViewPoint's prologue", kGetViewPoint, kGetViewPointPrologue},
    {L"RemoveLocalPlayer's prologue", kRemoveLocalPlayer, kRemoveLocalPlayerPrologue},
    {L"RemoveLocalPlayer's null-controller skip", kRemoveLocalPlayerControllerTest,
     kRemoveLocalPlayerControllerTestBytes},
};

}  // namespace lss::engine
