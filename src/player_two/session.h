#pragma once

// Player 2 as their own player: join, party member, camera, and leave, in that order.
//
//   join     CreatePlayer with player 2's own controller (player_two/own_controller.h makes its
//            camera manager's BeginPlay safe).
//   member   once player 1's controller has settled after the join, player 2 takes the member in the
//            2nd slot of the game's party order (player_two/party_member.h ChooseSlot), and success is
//            read from the lease fields over the following ticks. While that member is not free (a story
//            script holds its AI, or it is not following the party), player 2 stays in and waits on it;
//            a menu reorder changes the slot. A script putting player 2's member into LEAD gets it back.
//   camera   right after the join, player 2's camera manager is pointed at its own camera actor
//            (player_two/view_target.h), as the game's own spawn aimed it; a later move is logged,
//            never re-aimed (no game code moves it). When the member is taken, player 2's
//            Field camera is brought to player 1's settings and player 2's camera update starts
//            running as the primary player's (player_two/field_camera.h).
//   movement with per-pad sticks on, the member's move state is ticked with player 2's pad
//            (player_two/member_movement.h).
//   leave    the member is given back first and awaited, then the leftover actors are destroyed
//            and player 2 removed in one step (player_two/leave_sequence.h).
//   party    the game's party funnels are heard (player_two/party_events.h): before the game makes
//            player 2's member player 1's leader, player 2 gives it back; after a status change, a
//            member out of the deployed or battle party is given back and another chosen; after a party
//            apply, a member no longer in the 2nd slot is given back and the slot's taken. A slow
//            check backs both up (story override party data passes neither funnel), and the battle
//            controller component is checked every few ticks. A downed member stays player 2's.
//   no member with none free (player 1's character alone in a story party), player 2 does not exist:
//            a join creates nothing, and a player 2 whose member went leaves. The mod is then as
//            with one player, and player 2 joins once the game's party changes free a member (a
//            slow check backs the funnels up). Ctrl+F8 stops the wait.
//   map load player 2 leaves before it, and joins again once the new map is in the field state
//            and player 1's camera is ready.
// Each stage is installed only if its engine checks pass; a stage that is not installed is
// skipped, and the ones before it still work.
//
// Game thread only, outside Draw.

#include "player_two/leave_sequence.h"
#include "player_two/outside_join.h"
#include "player_two/party_member.h"
#include "players/local_players.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two {

class Session
{
public:
    explicit Session(std::uintptr_t removeLocalPlayer) noexcept : players_(removeLocalPlayer) {}

    /// Installs every stage whose checks pass, and logs which. Call once at startup.
    void InstallStages(const platform::GameImage& image) noexcept;

    /// Puts every replaced slot back (at mod uninstall, with player 2 out).
    void RemoveStages() noexcept;

    /// Adds player 2, or, with no party member free for it, waits outside the game for one.
    void Join() noexcept;

    /// Starts a leave; it completes over the following ticks. Also stops a wait for a party member.
    void Leave() noexcept;

    /// Player 2 was asked for but is out until a party member is free for it.
    bool AwaitingMember() const noexcept { return outside_.Current() == outside_join::Mode::Await; }

    /// A map is about to load: player 2 leaves now, without waiting for the member's release.
    /// Returns true if player 2 is out.
    bool LeaveNow() noexcept;

    /// Once per tick.
    void Tick() noexcept;

    /// The periodic status line (every few seconds while player 2 is in).
    void LogStatus() noexcept;

    /// Switches per-pad sticks (player_two/pad_sticks.h), and with them player 2's movement, and logs
    /// the new state.
    void TogglePadSticks() noexcept;

    bool HasPlayerTwo() const noexcept { return players_.HasPlayerTwo(); }

    /// Player 2 is in, past the take, and drives a party member.
    bool PlaysMember() const noexcept;

    /// Player 2 is in without a member, waiting on the 2nd slot's (a story script holds it, or it is not
    /// free yet). Only player 1 plays meanwhile, so the screen shows player 1's single view.
    bool WaitsForMember() const noexcept
    {
        return HasPlayerTwo() && phase_ == Phase::Playing && member_ == nullptr && waitingForSlot_;
    }

    /// Player 1 rides a vehicle the whole party shares (the buggy or the bronco, player_two/shared_ride.h), each tick. While it
    /// holds, player 2 gives the member back through the game's release and stays in without one, as the party is
    /// in the one-player game; once it ends, player 2 takes the 2nd slot's member again. Logged on change.
    void SetSharedRide(bool on) noexcept;

    /// Player 2 is in and stands aside for a shared ride (only player 1 plays: player 1's single view).
    bool StandsAside() const noexcept { return HasPlayerTwo() && sharedRide_; }

    /// Player 2's and player 1's ULocalPlayer (null when player 2 is out).
    const void* PlayerTwo() const noexcept { return players_.PlayerTwo(); }
    const void* PlayerOne() const noexcept { return playerOne_; }

    std::uint32_t JoinCount() const noexcept { return players_.JoinCount(); }

private:
    enum class Phase
    {
        Out,
        SettlingPlayerOne,  // waiting for player 1's controller to name its pawn and leader again
        Taking,             // the switch was called; waiting for the lease to show player 2
        Playing,
        Swapping,           // player 2's member stopped being playable: given back, then chosen again
        Leaving,
    };

    bool JoinAllowed(const players::LocalPlayers::PlayerOne& one) const noexcept;
    void StartGeneration() noexcept;
    void Settle() noexcept;
    void AwaitTake() noexcept;
    void CheckMemberPlayable() noexcept;
    void AwaitSwap() noexcept;
    void Carry(LeaveAction action) noexcept;
    void Teardown() noexcept;
    bool HoldsMember() const noexcept;
    bool MemberReleased() const noexcept;
    void Forget() noexcept;
    void LogPlayerOneAfterJoin() noexcept;
    void LogTagCounts(const wchar_t* when) noexcept;
    void AimOwnCamera() noexcept;
    void WatchOwnCamera() noexcept;
    void RetakeWhenFree() noexcept;
    bool MemberDestroyed() noexcept;
    bool ReadyToJoinOutside() const noexcept;
    void OutsideTick() noexcept;
    void LogGaveUp(outside_join::Reason reason) noexcept;
    void* FreeMember(void* playerOneController) const noexcept;
    void AwaitMember(const wchar_t* why) noexcept;
    void LeaveToAwait(const wchar_t* why) noexcept;
    std::uint32_t BackstopTicks() const noexcept;
    void Yield(const wchar_t* why) noexcept;
    bool MemberHeld() const noexcept;  // taking or playing a member
    void EventLine(const std::wstring& text) noexcept;
    void OnLeaderChanging(void* controller, void* character) noexcept;
    void OnStatusChanged(std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept;
    void OnPartyApplied() noexcept;
    void CheckAfterPartyChange(bool followSlot) noexcept;
    void FollowSlot(const wchar_t* when, bool log) noexcept;  // log: the slot's verdict as an Info line
    void WaitForSlot(const party_member::SlotChoice& choice) noexcept;
    void CheckHeldAi() noexcept;

    players::LocalPlayers players_;
    LeaveSequence         leave_;
    Phase                 phase_            = Phase::Out;
    void*                 playerOne_        = nullptr;  // player 1's ULocalPlayer
    void*                 playerOneController_ = nullptr;
    void*                 playerTwoController_ = nullptr;
    void*                 member_           = nullptr;  // the member player 2 took (or was taking)
    std::uint64_t         memberWeak_       = 0;        // its FWeakObjectPtr, to see it destroyed
    std::uint32_t         ticks_            = 0;        // in the current phase
    std::uint32_t         retakeLooks_      = 0;        // ticks left to look for a free member after a party change
    std::uint32_t         eventLines_       = 0;        // party-event lines logged
    std::uint32_t         viewTargetResets_ = 0;        // times player 2's view was found moved off its camera
    bool                  viewOnOwnCamera_  = true;     // as last seen (a move is logged once per move)
    bool                  gateClosedLogged_ = false;
    bool                  refuseJoins_      = false;    // after an exception during a join
    outside_join::OutsideJoin outside_;                 // out: the rejoin after a map load, or the wait for a member
    bool                  waitingForSlot_   = false;    // in, without a member: the 2nd slot's is not free yet
    std::uint32_t         slotWaitLogged_   = 0;        // the wait's reason as last logged (logged on change)
    bool                  sharedRide_       = false;    // player 1 rides a shared vehicle: player 2 takes no member

    party_member::PlayerOneBytes playerOneBytes_{};

    struct Installed
    {
        bool ownController  = false;
        bool primary        = false;
        bool member         = false;
        bool fieldSettings  = false;
        bool cameraUpdate   = false;
        bool moveTicks      = false;  // the game's move-state ticks recorded (member movement runs on them)
        bool padSticks      = false;
        bool viewTarget     = false;
        bool memberMovement = false;
        bool scopeLeaks     = false;
        bool padButtons     = false;
        bool partyEvents    = false;
    } installed_;
};

}  // namespace lss::player_two
