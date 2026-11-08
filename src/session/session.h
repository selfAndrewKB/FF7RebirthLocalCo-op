#pragma once

// The seats' players as their own players: join, party member, camera, and leave, in that order, per seat
// (players/seats.h; seat 1 is player 2). A Seat is one player's record and its stages; the Session holds the built
// seats, installs the stages once, hears the game's party funnels once and hands each event to every seat in turn.
//
//   join     CreatePlayer with the seat's own controller (session/own_controller.h makes its
//            camera manager's BeginPlay safe).
//   member   once player 1's controller has settled after the join, the seat takes the member in its slot of the
//            game's party order (player 2 the 2nd, player 3 the 3rd: session/party_member.h ChooseSlot),
//            and success is read from the lease fields over the following ticks. While that member is not free (a
//            story script holds its AI, or it is not following the party), the seat stays in and waits on it; on
//            one monitor, after a short grace, it leaves and waits outside (session_rules::WaitOutside). A menu
//            reorder changes the slot. A script putting the seat's member into LEAD gets it back.
//   camera   right after the join, the seat's camera manager is pointed at its own camera actor
//            (camera/view_target.h), as the game's own spawn aimed it; a later move is logged,
//            never re-aimed (no game code moves it). When the member is taken, the seat's
//            Field camera is brought to player 1's settings and the seat's camera update starts
//            running as the primary player's (camera/field_camera.h).
//   movement with per-pad sticks on, the member's move state is ticked with the seat's pad
//            (input/member_movement.h).
//   leave    the member is given back first and awaited, then the leftover actors are destroyed
//            and the seat's player removed in one step (session/leave_sequence.h).
//   party    the game's party funnels are heard (session/party_events.h): before the game makes
//            the seat's member player 1's leader, the seat gives it back; after a status change, a
//            member out of the deployed or battle party is given back and another chosen; after a party
//            apply, a member no longer in the seat's slot is given back and the slot's taken. A slow
//            check backs both up (story override party data passes neither funnel), and the battle
//            controller component is checked every few ticks. A downed member stays the seat's.
//   no member with none free (player 1's character alone in a story party), the seat's player does not exist:
//            a join creates nothing, and a seat whose member went leaves. The mod is then as
//            with one player, and the seat joins once the game's party changes free a member (a
//            slow check backs the funnels up). Ctrl+F8 stops the wait.
//   map load every seat leaves before it (the last first), and joins again once the new map is in the field state
//            and player 1's camera is ready.
// Each stage is installed only if its engine checks pass; a stage that is not installed is
// skipped, and the ones before it still work. The session installs its own stages only; the other
// modules are installed by mod.cpp next to their siblings.
//
// Every module a Seat calls takes the seat (plan/seats.md step 6); the command controllers' tick runs once per tick,
// from the first seat that plays. The log lines name the seat's player (players::SeatName).
//
// Game thread only, outside Draw.

#include "platform/dev_switch.h"
#include "players/local_players.h"
#include "players/seat_state.h"
#include "players/seats.h"
#include "session/leave_sequence.h"
#include "session/outside_join.h"
#include "session/party_member.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// WinBase.h's obsolete Yield() macro (empty) would swallow Seat::Yield wherever Windows.h comes first.
#ifdef Yield
#undef Yield
#endif

namespace lss::platform {
class GameImage;
}

namespace lss::session {

/// The stages the session installed, which every seat reads.
struct SessionStages
{
    bool ownController  = false;
    bool primary        = false;
    bool member         = false;
    bool fieldSettings  = false;
    bool moveTicks      = false;  // the game's move-state ticks recorded (member movement runs on them)
    bool padSticks      = false;
    bool viewTarget     = false;
    bool memberMovement = false;
    bool partyEvents    = false;
    // Not a stage: set by mod.cpp (Session::SetClearMirrors). Takes the mirror views out of the engine's list right
    // before a seat's CreatePlayer, so the seat lands in the column after the players in (views/mirror_player.h;
    // they come back on the same tick); true when none is listed.
    bool (*clearMirrors)() noexcept = nullptr;
    // Not a stage: set by mod.cpp each tick (Session::SetColumnsHeld), as the last columns' tick read it. Columns held
    // beyond the players (the monitor row, the console's `view-columns`): a player waiting for their member stays in
    // then, their column on player 1's picture; on one monitor they leave and wait outside (session_rules::WaitOutside).
    bool columnsHeld = false;
};

/// One seat's player: its local player, its member, its leave, and its wait outside the game.
class Seat
{
public:
    /// `refuseJoins` is the session's: a join that raised an exception refuses every later join.
    Seat(int seat, std::uintptr_t removeLocalPlayer, const SessionStages& installed, bool& refuseJoins) noexcept
        : seat_(seat), players_(removeLocalPlayer, seat), installed_(installed), refuseJoins_(refuseJoins)
    {
    }
    Seat(const Seat&)            = delete;
    Seat& operator=(const Seat&) = delete;

    int Number() const noexcept { return seat_; }

    /// "player 2" or "player 3", for the log.
    const wchar_t* Name() const noexcept { return players::SeatName(seat_); }

    /// Adds the seat's player, or, with no party member free for it, waits outside the game for one.
    void Join() noexcept;

    /// Starts a leave; it completes over the following ticks. Also stops a wait for a party member.
    void Leave() noexcept;

    /// The seat's player was asked for but is out until a party member is free for it.
    bool AwaitingMember() const noexcept { return outside_.Current() == outside_join::Mode::Await; }

    /// The seat's player was taken out by a map load and joins again once the new map is ready.
    bool RejoinPending() const noexcept { return outside_.Current() == outside_join::Mode::Rejoin; }

    /// The seat's state in one word (players/seat_state.h), from the flags below.
    players::SeatState State() const noexcept;

    /// After the seat's tick: logs the state when it changed since the last call (the one place a change is logged).
    void NoteState() noexcept;

    /// The member the seat's player plays (its name and slot of the party order), or why it has none.
    std::wstring MemberReading() const;

    /// A map is about to load: the seat's player leaves now, without waiting for the member's release.
    /// Returns true if it is out.
    bool LeaveNow() noexcept;

    /// Once per tick, after the session's shared work. `commandMenusTicked`: the session's, false at each tick's start;
    /// the first seat that plays ticks every seat's command controller and sets it.
    void Tick(bool& commandMenusTicked) noexcept;

    /// The seat's lines of the periodic status (the session adds the shared modules' after every seat's).
    void LogStatus() noexcept;

    bool In() const noexcept { return players_.HasPlayer(); }

    /// In, past the take, and driving a party member.
    bool PlaysMember() const noexcept;

    /// In without a member, waiting on its slot's (a story script holds it, or it is not free yet). Only player 1
    /// plays meanwhile: the player's column shows player 1's picture, unless they ride along (RidesAlong).
    bool WaitsForMember() const noexcept
    {
        return In() && phase_ == Phase::Playing && member_ == nullptr && waitingForSlot_;
    }

    /// Waiting on a member a script took (a level's walk or lead, an event's walk, an animation: session/field_hold.h)
    /// with the player's own view kept on it: the player rides along until the member follows the party again and the
    /// seat takes it back. Their column stays their own (views/column_source.h), and they do not leave to wait outside.
    bool RidesAlong() const noexcept { return WaitsForMember() && rideAlong_; }

    /// Player 1 rides a vehicle the whole party shares (the buggy or the bronco, rides/shared_ride.h), each
    /// tick. While it holds, the seat gives the member back through the game's release and stays in without one, as
    /// the party is in the one-player game; once it ends, the seat takes its slot's member again. Logged on change.
    void SetSharedRide(bool on) noexcept;

    /// In and standing aside for a shared ride (only player 1 plays: player 1's single view).
    bool StandsAside() const noexcept { return In() && sharedRide_; }

    /// The seat's and player 1's ULocalPlayer (null while the seat is out).
    const void* LocalPlayer() const noexcept { return players_.Player(); }
    const void* PlayerOne() const noexcept { return playerOne_; }

    std::uint32_t JoinCount() const noexcept { return players_.JoinCount(); }

    // The game's party funnels (session/party_events.h), heard by the session and handed to each seat.
    void OnLeaderChanging(void* controller, void* character) noexcept;
    void OnStatusChanged(std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept;
    void OnPartyApplied() noexcept;

private:
    enum class Phase
    {
        Out,
        SettlingPlayerOne,  // waiting for player 1's controller to name its pawn and leader again
        Taking,             // the switch was called; waiting for the lease to show the seat
        Playing,
        Swapping,           // the seat's member stopped being playable: given back, then chosen again
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
    const wchar_t* NotReadyToJoin() const noexcept;  // null when a join may run, else why not
    bool ReadyToJoinOutside() const noexcept;
    void OutsideTick() noexcept;
    void LogGaveUp(outside_join::Reason reason) noexcept;
    void* FreeMember(void* playerOneController) const noexcept;
    void AwaitMember(const wchar_t* why, bool fieldReady = false) noexcept;   // fieldReady: outside_join::BeginAwait
    void LeaveToAwait(const wchar_t* why, bool fieldReady = false) noexcept;
    void LeaveToWaitOutside() noexcept;  // the slot wait on one monitor, past the grace: leaves to wait outside
    std::chrono::milliseconds WaitedForSlot() const noexcept;  // how long the slot wait has lasted (0 outside it)
    std::uint32_t BackstopTicks() const noexcept;
    void Yield(const wchar_t* why) noexcept;
    bool MemberHeld() const noexcept;  // taking or playing a member
    void EventLine(const std::wstring& text) noexcept;
    void CheckAfterPartyChange(bool followSlot) noexcept;
    void FollowSlot(const wchar_t* when, bool log) noexcept;  // log: the slot's verdict as an Info line
    void WaitForSlot(const party_member::SlotChoice& choice) noexcept;
    void CheckHeldAi() noexcept;
    void RideAlongTick() noexcept;  // the view on the member while riding along, and the lines the checks read
    void EndRideAlong(const wchar_t* why) noexcept;

    const int             seat_;
    players::LocalPlayers players_;
    const SessionStages&  installed_;
    bool&                 refuseJoins_;                 // the session's: after an exception during a join
    LeaveSequence         leave_;
    Phase                 phase_            = Phase::Out;
    void*                 playerOne_        = nullptr;  // player 1's ULocalPlayer
    void*                 playerOneController_ = nullptr;
    void*                 controller_          = nullptr;  // the seat's controller
    void*                 member_           = nullptr;  // the member the seat took (or was taking)
    std::uint64_t         memberWeak_       = 0;        // its FWeakObjectPtr, to see it destroyed
    std::uint32_t         ticks_            = 0;        // in the current phase
    std::uint32_t         retakeLooks_      = 0;        // ticks left to look for a free member after a party change
    std::uint32_t         eventLines_       = 0;        // party-event lines logged
    std::uint32_t         viewTargetResets_ = 0;        // times the seat's view was found moved off its camera
    bool                  viewOnOwnCamera_  = true;     // as last seen (a move is logged once per move)
    bool                  gateClosedLogged_ = false;
    outside_join::OutsideJoin outside_;                 // out: the rejoin after a map load, or the wait for a member
    bool                  waitingForSlot_   = false;    // in, without a member: the slot's is not free yet
    std::uint32_t         slotWaitLogged_   = 0;        // the wait's reason as last logged (logged on change)
    std::optional<std::chrono::steady_clock::time_point> slotWaitSince_;  // when the slot wait began (on one monitor)
    party_member::SlotWait slotWaitReason_ = party_member::SlotWait::None;  // the wait's reason as last chosen
    bool                  sharedRide_       = false;    // player 1 rides a shared vehicle: the seat takes no member
    bool                  rideAlong_        = false;    // the member a script took is watched, the view kept (RidesAlong)
    void*                 rideMember_       = nullptr;  // that member
    std::uint64_t         rideWeak_         = 0;        // its FWeakObjectPtr, to see it destroyed
    std::uint32_t         rideTicks_        = 0;        // ticks riding along
    std::uint32_t         rideAims_         = 0;        // times the view was put back on the seat's camera actor
    players::SeatState    loggedState_      = players::SeatState::Out;  // as NoteState last logged it

    party_member::PlayerOneBytes playerOneBytes_{};
};

class Session
{
public:
    explicit Session(std::uintptr_t removeLocalPlayer) noexcept;
    Session(const Session&)            = delete;  // the seats hold references into it
    Session& operator=(const Session&) = delete;

    /// Installs every stage whose checks pass, and logs which. Call once at startup.
    void InstallStages(const platform::GameImage& image) noexcept;

    /// Puts every replaced slot back (at mod uninstall, with every seat out).
    void RemoveStages() noexcept;

    /// Adds `seat`'s player (Seat::Join); nothing for a seat not built.
    void Join(int seat) noexcept;

    /// Starts `seat`'s leave (Seat::Leave); nothing for a seat not built.
    void Leave(int seat) noexcept;

    /// A map is about to load: every seat leaves now, the last first. True if every seat is out.
    bool LeaveNow() noexcept;

    /// Once per tick: the shared work, then every seat's.
    void Tick() noexcept;

    /// The periodic status lines (every few seconds while a seat is in): each seat's, then the shared modules'.
    void LogStatus() noexcept;

    /// Switches per-pad sticks (input/pad_sticks.h), and with them the seats' movement, and logs the new
    /// state.
    void TogglePadSticks() noexcept;

    /// Player 1 rides a vehicle the whole party shares, each tick: every seat stands aside together.
    void SetSharedRide(bool on) noexcept;

    /// Columns held beyond the players (SessionStages::columnsHeld), each tick, as the last columns' tick read it.
    void SetColumnsHeld(bool held) noexcept { installed_.columnsHeld = held; }

    /// What every seat's join calls right before its CreatePlayer (SessionStages::clearMirrors). Once, at startup.
    void SetClearMirrors(bool (*clearMirrors)() noexcept) noexcept { installed_.clearMirrors = clearMirrors; }

    bool          In(int seat) const noexcept;
    bool          AnyIn() const noexcept;
    bool          AwaitingMember(int seat) const noexcept;
    bool          RejoinPending(int seat) const noexcept;
    bool          PlaysMember(int seat) const noexcept;
    bool          WaitsForMember(int seat) const noexcept;
    bool          RidesAlong(int seat) const noexcept;  // Seat::RidesAlong
    bool          StandsAside(int seat) const noexcept;
    const void*   LocalPlayer(int seat) const noexcept;  // null while out
    std::uint32_t JoinCount(int seat) const noexcept;

    /// The seat's state in one word (Seat::State); out for a seat not built.
    players::SeatState State(int seat) const noexcept;
    /// The member the seat's player plays, or why it has none (Seat::MemberReading).
    std::wstring MemberReading(int seat) const;

    /// Player 1's ULocalPlayer as the first seat in recorded it at its join (null while every seat is out).
    const void*   PlayerOne() const noexcept;

private:
    Seat*       At(int seat) noexcept;
    const Seat* At(int seat) const noexcept;

    void OnLeaderChanging(void* controller, void* character) noexcept;
    void OnStatusChanged(std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept;
    void OnPartyApplied() noexcept;

    SessionStages installed_;
    bool          refuseJoins_ = false;  // after an exception during any seat's join
    std::array<std::optional<Seat>, players::kMaxPlayers> seats_;  // the built seats; index 0 (player 1) unused
};

#if LSS_DEV
/// The session's control commands. Per built seat, from one function: its join (`join` for player 2, `p3.join` for
/// player 3) and its readings `p2.in`, `p2.state` (players/seat_state.h's words), `p2.member` and `p2.rejoin` (`p3.`
/// for player 3). Shared: sticks and p2.log-status. Player 2's leave is the module table's (modules.h).
/// Before the console opens.
void RegisterCommands(Session& session);
#endif

}  // namespace lss::session
