#pragma once

// The session's pure decisions (session/session.h), as functions over values read from the game, tested on
// the host (tests/session_rules_test.cpp).
//
// Playing is one phase of the session with several holds, told apart by what the seat's member fields say: the
// retake and the member checks act on the hold, not on the fields one by one.

#include <chrono>

namespace lss::session::session_rules {

/// What the seat holds while its session phase is Playing.
enum class Hold
{
    Withdrawn,   // the seat's stages were withdrawn (a fault): nothing is taken or checked
    Member,      // plays its member (the generation names it)
    Faulted,     // a member is recorded but the generation does not name it (a take that timed out, or a script's hand
                 // while the player rides along: nothing acts on the member then)
    StandAside,  // no member: player 1 rides a vehicle the whole party shares
    SlotWait,    // no member: waits on its slot's (a story script holds it, or it is not free yet)
    Orphaned,    // no member: its give-back timed out, or the take failed; takes one as soon as one is free
};

constexpr Hold HoldOf(const void* member, bool waitingForSlot, bool sharedRide, const void* generationMember,
                      bool generationActive) noexcept
{
    if (!generationActive)
    {
        return Hold::Withdrawn;
    }
    if (member != nullptr)
    {
        return generationMember == member ? Hold::Member : Hold::Faulted;
    }
    if (sharedRide)
    {
        return Hold::StandAside;
    }
    return waitingForSlot ? Hold::SlotWait : Hold::Orphaned;
}

/// The holds in which the seat takes a member as soon as one is free (RetakeWhenFree).
constexpr bool MayRetake(Hold hold) noexcept
{
    return hold == Hold::SlotWait || hold == Hold::Orphaned;
}

/// What decides whether a player waiting for their member leaves the game to wait outside (WaitOutside).
struct WaitOutsideFacts
{
    Hold                      hold        = Hold::Withdrawn;
    bool                      columnsHeld = false;  // the monitor row, or the console's `view-columns`
    std::chrono::milliseconds waited{0};            // how long the slot wait has lasted on one monitor, not riding
};

/// A player in without their member while the game holds it (the slot wait, whatever its reason) leaves the game on
/// one monitor and waits outside for it, with two players in or three (the user's rule: on one monitor no view of
/// a waiting player is shown, no copy of player 1's view and no switch to player 1's view alone on their account; the
/// player's view goes until their character is back), once the wait has lasted `grace`: a party swap that resolves
/// within a second keeps the player in. With columns held (the monitor row, the console's `view-columns`) the player
/// stays in and their column shows player 1's picture on their own monitor (views/column_source.h). Standing aside for
/// a shared ride never leaves: every seat's player stands aside together, and player 1's single view follows. (A player
/// whose member a script took rides along with it and does not wait at all: session's RidesAlong.)
constexpr bool WaitOutside(const WaitOutsideFacts& facts, std::chrono::milliseconds grace) noexcept
{
    return facts.hold == Hold::SlotWait && !facts.columnsHeld && facts.waited >= grace;
}

/// The game is about to make a character the leader of a controller (session/party_events.h), as the session
/// sees it.
struct LeaderEvent
{
    bool seatIn                = false;
    bool awaitingMember        = false;  // the seat is out, waiting for a free party member
    bool controllerIsSeat      = false;  // the controller is the seat's
    bool controllerIsPlayerOne = false;  // ... player 1's
    bool memberIsSeat          = false;  // the character is the seat's member (not null)
    bool memberHeld            = false;  // taking or playing a member
    bool noMember              = false;  // the seat is in without a member
    bool inOwnPass             = false;  // the change runs inside the seat's own command pass (its d-pad switch)
};

enum class LeaderEventAction
{
    None,
    ArmLook,  // the member the game moved off may be free: look for one every tick for a while
    Warn,     // the game changes the seat's controller's leader outside the seat's own pass: not acted on
    Yield,    // the seat's member is about to become player 1's leader: given back first
    Adopt,    // the seat's own switch: the game moves the seat's controller onto the character; the seat plays it
};

constexpr LeaderEventAction DecideLeaderEvent(const LeaderEvent& event) noexcept
{
    if (!event.seatIn)
    {
        return event.awaitingMember ? LeaderEventAction::ArmLook : LeaderEventAction::None;
    }
    if (event.controllerIsSeat)
    {
        return event.inOwnPass && event.memberHeld ? LeaderEventAction::Adopt : LeaderEventAction::Warn;
    }
    if (event.controllerIsPlayerOne && event.memberIsSeat && event.memberHeld)
    {
        return LeaderEventAction::Yield;
    }
    return event.noMember ? LeaderEventAction::ArmLook : LeaderEventAction::None;
}

/// A player's own switch holds while the fight lasts: the slot is not followed meanwhile (a party apply or the slow
/// check would send the player back), and the switch ends with the fight, when the seat takes its slot's member again
/// as the game sends player 1 back to the field leader.
constexpr bool OwnSwitchHolds(bool ownSwitch, bool inBattle) noexcept
{
    return ownSwitch && inBattle;
}

}  // namespace lss::session::session_rules
