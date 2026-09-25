#pragma once

// The rules for lending a party member to player 2's controller, as pure functions over values
// read from the game (tested on the host: tests/party_lease_test.cpp).
//
// A field party member that nobody plays is driven by its own AI controller. Square's character
// keeps that ownership in four fields (the "lease"):
//   +0x578 the controller that asked for the character (null when nobody asked)
//   +0x580 the controller that drives it now
//   +0x588 the AI controller it returns to
//   +0x260 its Unreal controller (APawn::Controller)
// and the AI controller's own pawn field (+0x258) names the character back. The player-2 switch
// (AEndPlayerController::SwitchLeaderInternal) moves +0x578/+0x580/+0x260 to player 2's
// controller and back; a character is only offered to it while every field says "AI-owned".

#include <cstdint>

namespace lss::player_two {

/// One party member, as read from the game just before choosing.
struct MemberSnapshot
{
    const void*  character         = nullptr;
    bool         isEndCharacter    = false;
    bool         isPlayerOnes      = false;  // the global leader, player 1's pawn or player 1's leader
    const void*  requested         = nullptr;  // +0x578
    const void*  effective         = nullptr;  // +0x580
    const void*  retainedAi        = nullptr;  // +0x588
    const void*  controller        = nullptr;  // +0x260
    const void*  aiPawn            = nullptr;  // the retained AI controller's +0x258
    std::uint8_t playerControlled  = 0;        // +0x625
    bool         aiControllerValid = false;    // the switch's own gate on the AI component
    bool         leaderSlot        = false;    // the field leader's or the default battle leader's type
    bool         battleReady       = true;     // passes player 1's battle switch's own member test
};

enum class Eligibility
{
    Eligible,
    NoCharacter,
    NotEndCharacter,
    PlayerOnes,
    LeaderSlot,    // the game hands this type back to player 1 (battle end, or battle start)
    Requested,     // another controller has asked for it
    NotAiOwned,    // +0x580, +0x588, +0x260 and the AI's pawn do not all agree
    PlayerDriven,  // +0x625 says a player drives it
    AiInvalid,         // the switch would publish the leader without moving control
    BattleUnavailable, // player 1's battle switch would not take it either (its state 6 or state id 22)
};

Eligibility Assess(const MemberSnapshot& member) noexcept;

const wchar_t* Describe(Eligibility eligibility) noexcept;

/// The fields that show whether player 2's controller holds the member.
struct LeaseState
{
    const void* memberEffective  = nullptr;  // member +0x580
    const void* memberRetainedAi = nullptr;  // member +0x588
    const void* aiPawn           = nullptr;  // (member +0x588) +0x258
    const void* playerTwoPawn    = nullptr;  // player 2's controller +0x258
    const void* playerTwoLeader  = nullptr;  // player 2's controller +0x5A0
};

/// Player 2's controller drives the member, and names it as both its pawn and its leader.
bool Acquired(const LeaseState& state, const void* member, const void* playerTwoController) noexcept;

/// The member is back with its own AI, and player 2's controller names nothing.
bool Released(const LeaseState& state, const void* member) noexcept;

}  // namespace lss::player_two
