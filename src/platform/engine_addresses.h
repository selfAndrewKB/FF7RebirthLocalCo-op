#pragma once

// Every engine address the mod relies on, with the bytes that prove it at startup.
//
// Addresses are written as IDA shows them (image base 0x140000000) so they can be looked up in
// the research documents directly; platform::GameImage rebases them. Before anything is
// installed, the mod checks every fact in kVerifiedCode against the running game. If any
// differs (a game patch, another mod), the mod installs nothing (philosophy §4.4).
//
// Research: Rebirth/Documentation/research/RE-onefamily-feasibility.md (the view-family
// submission) and RE-remove-local-player.md (player removal). Engine source for comparison:
// FF7RebirthEngineV4 (stock UE4).

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
/// `mov al,[rbx+68h]; and al,cl; mov [rdi+74h],al` (cl = 1). The field's reader in the final
/// output pass was not located statically; the spike's RenderDoc capture proves it
/// (SPLITSCREEN-ARCHITECTURE.md §5).
inline constexpr std::uintptr_t kAdditionalFamilyStore        = 0x140CC4353;
inline constexpr std::uint8_t   kAdditionalFamilyStoreBytes[] = {0x8A, 0x43, 0x68, 0x22, 0xC1, 0x88, 0x47, 0x74};

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
    {L"RemoveLocalPlayer's prologue", kRemoveLocalPlayer, kRemoveLocalPlayerPrologue},
    {L"RemoveLocalPlayer's null-controller skip", kRemoveLocalPlayerControllerTest,
     kRemoveLocalPlayerControllerTestBytes},
};

}  // namespace lss::engine
