#pragma once

// The FieldActionMgr ([g_EndGameRoot]+0x1A0): the point registry's live list and lock, the eight slots' rows, targets
// and latches, the pending flag, and a point's key, preset and flags. Read by world/field_pickup (each seat's own
// pass over the registry) and world/interact_watch (its hooks and log). Each offset was read in IDA at an
// instruction that uses it.
//
// Research: Rebirth/Documentation/research/RE-p2-field-interact.md.

#include "platform/verified_code.h"

#include <cstdint>

namespace lss::facts {

// The game's pointer to the field-action global (research/RE-yuffie-box-throw.md; the traversal rumble list's owner is
// the same object): the registry of the breakable boxes' shooting-target components and, per box, the game's "the
// party leader is near" byte, which the game refreshes once per frame from the party leader's position whenever the
// object's wanted byte asks for it. A box is a candidate for anyone's throw selection only while that byte is set.
// Both builds (the pointer's address is translated); null until the game makes the object.
inline constexpr std::uintptr_t kFieldActionGlobal       = 0x1491D84B0;
inline constexpr std::uint32_t  kFieldActionGlobalWanted = 0x389;  // u8: a refresh wanted this frame (the refresh clears it)
inline constexpr std::uint8_t   kRefreshShootingTargetsBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                                                  0x89, 0x78, 0x18, 0x4C, 0x89, 0x78, 0x20, 0x55};
/// FieldActionGlobal_RefreshShootingTargets_LeaderNear261(global): when the byte above is set, clears it and writes
/// every registered box's leader-near byte from the party leader's position.
inline constexpr platform::FunctionFact kRefreshShootingTargets{L"FieldActionGlobal_RefreshShootingTargets_LeaderNear261",
                                                                0x1416AD83C, kRefreshShootingTargetsBytes};

// The registry: 1024 pool nodes of 288 bytes; the live list's head node (node+0x08 next, the point at node+0x18) under
// an SRWLOCK the game holds while it walks.
inline constexpr std::uint32_t kFieldActionPointList = 0x48320;
inline constexpr std::uint32_t kFieldActionNodeNext  = 0x08;
inline constexpr std::uint32_t kFieldActionNodePoint = 0x18;
inline constexpr std::uint32_t kFieldActionPointLock = 0x487F0;
inline constexpr int           kFieldActionSlots     = 8;
inline constexpr int           kFieldActionMaxPoints = 1024;

// The eight slots: their HUD rows (FWeakObjectPtr {index, serial}), this frame's target keys (FName), and the held,
// pressed and executed latches (u8 each, three runs of eight); the pending flag.
inline constexpr std::uint32_t kFieldActionSlotRows     = 0x483A8;
inline constexpr std::uint32_t kFieldActionSlotTargets  = 0x48608;
inline constexpr std::uint32_t kFieldActionSlotHeld     = 0x48730;
inline constexpr std::uint32_t kFieldActionSlotPressed  = 0x48738;
inline constexpr std::uint32_t kFieldActionSlotExecuted = 0x48740;
inline constexpr std::uint32_t kFieldActionPending      = 0x487D0;  // u8
// The slots' block (labels, rows, slot entries, records with the elapsed hold, keys, locations, latches): one player's,
// what a point's hold progress reads.
inline constexpr std::uint32_t kFieldActionSlotBlock     = 0x48328;
inline constexpr std::uint32_t kFieldActionSlotBlockSize = 0x48748 - 0x48328;

// A point: its key (FName), the point-side range preset (FName rpa<Owner>), and its flags (0x02 slot target, 0x04
// runner-up, 0x08 not usable from here: the update makes it no target, 0x10 disabled: the candidate query skips it).
inline constexpr std::uint32_t kFieldActionPointKey    = 0x0C;
inline constexpr std::uint32_t kFieldActionPointPreset = 0x44;
inline constexpr std::uint32_t kFieldActionPointFlags  = 0x104;
inline constexpr std::uint8_t  kFieldActionPointTargetBits = 0x06;  // slot target and runner-up: one player's selection
inline constexpr std::uint8_t  kFieldActionPointNotUsable  = 0x08;
inline constexpr std::uint8_t  kFieldActionPointDisabled   = 0x10;
// The point's kind (research/RE-rope-landing-prompt.md, both builds): the hook rope's landing point is 3 for a jump or
// climbing target (the Dodge key), 0 for a climbing rope (FieldAction), 6 for a next rope (Catch), whose press the move
// accepts only while the point is the selection's slot target.
inline constexpr std::uint32_t kFieldActionPointKind         = 0xE8;
inline constexpr std::uint8_t  kFieldActionPointKindNextRope = 6;

// A map element's entry, as AcquireEntry hands it out: element + 0x10 (the FName key is 0x10 bytes before the entry).
inline constexpr std::uint32_t kFieldActionEntryKeyBack = 0x10;

// The switches (AEndFieldActionSwitchAccessActor: levers, wall switches, lift panels; RE-further-player-levers.md): the
// native vtable placed Blueprint switches keep, and the class's unregister override (the leave: the leader's list, the
// point off, the in-range byte cleared). 1.005 through replaced-levers.json.
inline constexpr std::uintptr_t kSwitchAccessVTable     = 0x14567E3F0;
inline constexpr std::uintptr_t kSwitchAccessUnregister = 0x140D7F428;
// The actor's own bytes, one per actor: in range (the register sets it, the unregister clears it), the press latch
// (the point's press listener sets it when the hold completes; the point setter clears it on its on and off edges), the
// point shown (the setter's own record of its last show), the "cannot act" answer of the last notify.
inline constexpr std::uint32_t kSwitchInRange    = 0x9E0;
inline constexpr std::uint32_t kSwitchPressLatch = 0x9F0;
inline constexpr std::uint32_t kSwitchPointShown = 0x9F1;
inline constexpr std::uint32_t kSwitchCannotAct  = 0x9F3;
// A state-managed field-action actor's default start and end state triggers (FName each): sent at the move's state enter
// and exit only for a character whose tag is the party leader's player type, so never for a seat's member.
inline constexpr std::uint32_t kStateManagedDefaultStart = 0x3BC;
inline constexpr std::uint32_t kStateManagedDefaultEnd   = 0x3C4;
// A switch executer: the character it runs on and the switch, each a weak reference. A tower terminal's executer keeps
// them at the same offsets.
inline constexpr std::uint32_t kSwitchExecCharacter = 0x10;
inline constexpr std::uint32_t kSwitchExecActor     = 0x58;

// The communication towers' terminals (AEndFieldActionOneShotAccessActor with bTower set; RE-tower-activation.md): the
// native vtable placed Blueprint terminals keep, and the class's unregister override (the leave: the leader's list, the
// point off, the registered character cleared). 1.005 through replaced-towers.json.
inline constexpr std::uintptr_t kOneShotAccessVTable     = 0x14567D908;
inline constexpr std::uintptr_t kOneShotAccessUnregister = 0x1433D8550;
// The actor's own fields, one per actor: the hub row it activates (FName), the tower flag (bool; the class's other
// actors, life springs and story terminals, leave it clear), the character that registered (a weak reference the
// register sets to the party leader and the unregister clears; the point shows only while it is valid), the press latch
// (the point's listener sets it; the setter clears it on its edges; the executer's end clears it), the point shown.
inline constexpr std::uint32_t kOneShotAccessHubId      = 0x718;
inline constexpr std::uint32_t kOneShotAccessTower      = 0x8A0;
inline constexpr std::uint32_t kOneShotAccessRegistered = 0x8B0;
inline constexpr std::uint32_t kOneShotAccessPressLatch = 0x8E0;
inline constexpr std::uint32_t kOneShotAccessPointShown = 0x8E1;

// The chocobo stops (AEndFieldActionChocoboStopActor; RE-chocobo-stop-seats.md): the native vtable placed Blueprint stops
// keep, and the class's unregister override (the leave: the leader's list, the point off, the registered character
// cleared).
inline constexpr std::uintptr_t kChocoboStopVTable     = 0x14570CDE8;
inline constexpr std::uintptr_t kChocoboStopUnregister = 0x143397520;
// The actor's own fields, one per actor: the hub row it activates (FName), the character that registered (a weak
// reference the register sets to the party leader and the unregister clears; nothing reads it), the sign state in use
// (0 idle, 1 start, 2 hold, 3 cancel, 4 success), the availability (2: the prompt may show), the press latch (the point's
// listener sets it when the hold completes; the hold reads 1 while it is set; the setter clears it on its edges), the
// point shown, and the character of the stop's move (a weak reference the move sets at each phase's start).
inline constexpr std::uint32_t kChocoboStopHubId         = 0x718;
inline constexpr std::uint32_t kChocoboStopRegistered    = 0x734;
inline constexpr std::uint32_t kChocoboStopSignState     = 0x7BA;
inline constexpr std::uint32_t kChocoboStopAvailability  = 0x7BB;
inline constexpr std::uint32_t kChocoboStopPressLatch    = 0x7BD;
inline constexpr std::uint32_t kChocoboStopPointShown    = 0x7BE;
inline constexpr std::uint32_t kChocoboStopMoveCharacter = 0x7E8;
// A field action's move: its character (a weak reference), its animations by index (a TArray of the anims its Setup
// found by name, null for a name the character lacks), and the vtable slot that registers them with fallback names.
inline constexpr std::uint32_t kFieldActionMoveCharacter        = 0x450;
inline constexpr std::uint32_t kFieldActionMoveAnims            = 0x470;
inline constexpr std::uint32_t kFieldActionMoveRegisterFallback = 0x110;
}  // namespace lss::facts
