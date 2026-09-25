#pragma once

// Player 2 as their own player: join, party member, camera, and leave, in that order.
//
//   join     CreatePlayer with player 2's own controller (player_two/own_controller.h makes its
//            camera manager's BeginPlay safe).
//   member   once player 1's controller has settled after the join, player 2 takes the first party
//            member nobody plays (player_two/party_member.h), and success is read from the lease
//            fields over the following ticks.
//   camera   right after the join, player 2's camera manager is pointed at its own camera actor
//            (player_two/view_target.h), and kept there. When the member is taken, player 2's
//            Field camera is brought to player 1's settings and player 2's camera update starts
//            running as the primary player's (player_two/field_camera.h).
//   movement with per-pad sticks on, the member's move state is ticked with player 2's pad
//            (player_two/member_movement.h).
//   leave    the member is given back first and awaited, then the leftover actors are destroyed
//            and player 2 removed in one step (player_two/leave_sequence.h).
// Each stage is installed only if its engine checks pass; a stage that is not installed is
// skipped, and the ones before it still work.
//
// Game thread only, outside Draw.

#include "player_two/leave_sequence.h"
#include "player_two/party_member.h"
#include "players/local_players.h"

#include <cstdint>

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

    void Join() noexcept;

    /// Starts a leave; it completes over the following ticks.
    void Leave() noexcept;

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
        Leaving,
    };

    bool JoinAllowed(const players::LocalPlayers::PlayerOne& one) const noexcept;
    void StartGeneration() noexcept;
    void Settle() noexcept;
    void AwaitTake() noexcept;
    void Carry(LeaveAction action) noexcept;
    void Teardown() noexcept;
    bool HoldsMember() const noexcept;
    bool MemberReleased() const noexcept;
    void Forget() noexcept;
    void LogPlayerOneAfterJoin() noexcept;
    void LogTagCounts(const wchar_t* when) noexcept;
    void AimOwnCamera() noexcept;
    void KeepOwnCamera() noexcept;

    players::LocalPlayers players_;
    LeaveSequence         leave_;
    Phase                 phase_            = Phase::Out;
    void*                 playerOne_        = nullptr;  // player 1's ULocalPlayer
    void*                 playerOneController_ = nullptr;
    void*                 playerTwoController_ = nullptr;
    void*                 member_           = nullptr;  // the member player 2 took (or was taking)
    std::uint32_t         ticks_            = 0;        // in the current phase
    std::uint32_t         viewTargetResets_ = 0;        // times player 2's view was found moved off its camera
    bool                  gateClosedLogged_ = false;
    bool                  refuseJoins_      = false;    // after an exception during a join

    party_member::PlayerOneBytes playerOneBytes_{};

    struct Installed
    {
        bool ownController  = false;
        bool primary        = false;
        bool member         = false;
        bool fieldSettings  = false;
        bool cameraUpdate   = false;
        bool moveCensus     = false;
        bool padSticks      = false;
        bool tagCounts      = false;
        bool viewTarget     = false;
        bool memberMovement = false;
    } installed_;
};

}  // namespace lss::player_two
