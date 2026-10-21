#pragma once

// The End camera manager (AEndPlayerCameraManager) and its operators: classes, fields, the spawn names its
// BeginPlay uses, SetViewTarget, and the slots the mod replaces.
//
// Addresses are as IDA shows them (image base 0x140000000). Each function's entry bytes are the first 24 bytes read in
// IDA; call and RIP-relative displacements are relative to the image, so they are identical in the running game. A
// stage is installed only if every fact it uses holds (platform/verified_code.h). Each offset was read at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-own-controller-ida.md, RE-p2-camera-movement-ida.md,
// RE-p2-viewtarget-movestate-ida.md; the view target was read live (RE-p2-first-run-ce.md).

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::SlotFact;

// The classes (vtables stored by the native constructors at offset 0)
inline constexpr std::uintptr_t kCameraManagerVTable       = 0x145D6FA30;
inline constexpr std::uintptr_t kFieldOperatorVTable       = 0x1466B5CB8;

// Fields
inline constexpr std::uint32_t kCameraManagerPcOwner           = 0x228;
inline constexpr std::uint32_t kCameraManagerViewTarget        = 0x1000;  // ViewTarget.Target
inline constexpr std::uint32_t kCameraManagerOperators         = 0x2C50;  // 13 operator pointers
inline constexpr std::uint32_t kCameraManagerOperatorCount     = 13;
inline constexpr std::uint32_t kCameraManagerFieldOperator     = 0x2CB0;  // operator 12
inline constexpr std::uint32_t kCameraManagerCurrentOperator   = 0x2CD0;  // u8 index into the operators
inline constexpr std::uint32_t kCameraManagerCollisionComponent = 0x2D48;  // outer: the collision actor
inline constexpr std::uint32_t kCameraManagerEndCameraActor    = 0x2DE8;
// Field camera operator
inline constexpr std::uint32_t kFieldOperatorCommand = 0x40;
inline constexpr std::uint32_t kFieldCommandSize     = 0x58;
inline constexpr std::uint32_t kFieldOperatorActive  = 0xD0;

// The spawn names
/// The camera manager's two spawn names (8-byte FNames; zero is None to UWorld::SpawnActor, which
/// tests the whole qword). The second is set lazily behind a thread-safe-static guard (u32:
/// 0 = never, -1 = in progress, anything else = done).
inline constexpr std::uintptr_t kEndCameraActorNameGlobal     = 0x1491DFB48;
inline constexpr std::uintptr_t kCompositeCollisionNameGlobal = 0x14934D028;
inline constexpr std::uintptr_t kCompositeCollisionNameGuard  = 0x14934D020;
inline constexpr std::uint64_t  kNameNone                     = 0;
inline bool NameGuardInitialised(const void* guard) noexcept
{
    if (guard == nullptr)
    {
        return false;
    }
    const auto value = *static_cast<const std::int32_t*>(guard);
    return value != 0 && value != -1;
}

// Functions
/// APlayerCameraManager::SetViewTarget(this, NewTarget, const FViewTargetTransitionParams*): the
/// call the End camera manager's BeginPlay makes with its camera actor.
inline constexpr std::uint8_t kSetViewTargetBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0xA0, 0xF9, 0xFF, 0xFF, 0xB8, 0x60, 0x07, 0x00};
inline constexpr FunctionFact kSetViewTarget{L"APlayerCameraManager::SetViewTarget", 0x1413C2F70, kSetViewTargetBytes};
/// FViewTargetTransitionParams as the End BeginPlay builds it: no blend time, cubic, exponent 2.
struct ViewTargetTransition
{
    float         blendTime     = 0.0f;
    std::uint8_t  blendFunction = 1;  // VTBlend_Cubic
    std::uint8_t  padding[3]    = {};
    float         blendExp      = 2.0f;
    std::uint32_t lockOutgoing  = 0;
};
static_assert(sizeof(ViewTargetTransition) == 16);

// Replaced slots
inline constexpr std::uint8_t kCameraManagerBeginPlayBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0xB8, 0x70, 0x00, 0x00, 0x00};
inline constexpr SlotFact kCameraManagerBeginPlay{
    L"the End camera manager's BeginPlay slot (103)", 0x145D6FD68,
    {L"AEndPlayerCameraManager::BeginPlay", 0x143D33994, kCameraManagerBeginPlayBytes}};
inline constexpr std::uint8_t kCameraUpdateBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41,
    0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x90, 0x05, 0x00, 0x00};
inline constexpr SlotFact kCameraUpdate{
    L"the End camera manager's UpdateCamera slot (211)", 0x145D700C8,
    {L"AEndPlayerCameraManager::UpdateCamera", 0x140E55E40, kCameraUpdateBytes}};
inline constexpr std::uint8_t kFieldActivateBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x88, 0x99, 0x80, 0x01,
    0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0xE8, 0x75, 0x01, 0x00, 0x00, 0x48};
inline constexpr SlotFact kFieldActivate{
    L"the Field operator's Activate slot (+0x10)", 0x1466B5CC8,
    {L"FEndFieldCameraOperator::Activate", 0x1408A559C, kFieldActivateBytes}};
inline constexpr std::uint8_t kFieldDeactivateBytes[] = {
    0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x56, 0x0F, 0xCB, 0xFE, 0x48, 0x2B,
    0xE0, 0x48, 0x83, 0xC4, 0x28, 0xE9, 0x02, 0xA3, 0x73, 0xFF, 0xCC, 0xCC};
inline constexpr SlotFact kFieldDeactivate{
    L"the Field operator's Deactivate slot (+0x20)", 0x1466B5CD8,
    {L"FEndFieldCameraOperator::Deactivate", 0x1433FDFD0, kFieldDeactivateBytes}};
inline constexpr std::uint8_t kFieldCopyCommandBytes[] = {
    0xC5, 0xFC, 0x10, 0x02, 0xC5, 0xFC, 0x11, 0x41, 0x40, 0xC5, 0xFC, 0x10,
    0x4A, 0x20, 0xC5, 0xFC, 0x11, 0x49, 0x60, 0xC5, 0xF8, 0x10, 0x42, 0x40};
inline constexpr SlotFact kFieldCopyCommand{
    L"the Field operator's CopyCommand slot (+0x58)", 0x1466B5D10,
    {L"FEndCameraOperator::CopyCommandSnapshot", 0x140FA0E80, kFieldCopyCommandBytes}};

}  // namespace lss::facts
