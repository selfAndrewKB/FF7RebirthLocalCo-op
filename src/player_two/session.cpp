// Session: player 2's party member, the leave, and the status line.

#include "player_two/session.h"

#include "platform/frame_watch.h"
#include "platform/log.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/field_exit.h"
#include "player_two/battle_camera.h"
#include "player_two/camera_override.h"
#include "player_two/command_rules.h"
#include "player_two/lock_on.h"
#include "player_two/field_camera.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/leftover_actors.h"
#include "player_two/member_movement.h"
#include "player_two/move_state.h"
#include "player_two/object_fields.h"
#include "player_two/own_controller.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/primary_controller.h"
#include "player_two/scope_leaks.h"
#include "player_two/tag_counts.h"
#include "player_two/view_target.h"

#include <optional>
#include <string>

namespace lss::player_two {

namespace {

using platform::Log;

/// Ticks player 1's controller is watched after a join (it has been seen to name no pawn for one).
constexpr std::uint32_t kPlayerOneWatchTicks = 3;

/// Ticks to wait for the switch to hand the member over before giving it back.
constexpr std::uint32_t kTakeTicks = 120;

/// How often, in ticks, player 2's member is checked against the deployed party and the battle
/// switch's member test (every call is a read).
constexpr std::uint32_t kPlayableCheckTicks = 30;

/// How often, in ticks, player 2 without a character looks for a free member.
constexpr std::uint32_t kRetakeTicks = 120;

/// After a map load: ticks the new map must look ready before player 2 joins again, how many joins
/// are tried, and how long the whole wait may take, before the rejoin is left to Ctrl+F7.
constexpr std::uint32_t kRejoinSettleTicks = 180;
constexpr std::uint32_t kRejoinTries       = 5;
constexpr std::uint32_t kRejoinWaitTicks   = 3600;

}  // namespace

void Session::TogglePadSticks() noexcept
{
    if (!installed_.padSticks)
    {
        Log(L"per-pad sticks unavailable (its stage is not installed)");
        return;
    }
    if (pad_sticks::Toggle())
    {
        Log(L"per-pad sticks ON: each pad drives only its own player's camera and movement");
    }
    else
    {
        Log(L"per-pad sticks OFF: every pad drives both players' sticks (the game's own merge)");
    }
}

void Session::LogTagCounts(const wchar_t* when) noexcept
{
    if (!installed_.tagCounts)
    {
        return;
    }
    const auto counts = tag_counts::Read(member_);
    const auto text   = [](const std::optional<std::int32_t>& count) {
        return count ? std::to_wstring(*count) : std::wstring(L"?");
    };
    Log(L"tag counts {}: {}'s first tag {}, trigger tag {}", when, NameOf(member_), text(counts.member),
        text(counts.trigger));
}

void Session::Tick() noexcept
{
    generation::SetGameThread();  // the engine-tick callback runs on the thread that ticks the world
    command_rules::Update();      // which member player 1 may not command (none without player 2)
    if (!HasPlayerTwo())
    {
        TryRejoin();
        return;
    }
    if (!players_.StillListed())
    {
        Log(L"player 2 was removed by the game; this mod has let go of it");
        Forget();
        return;
    }
    ++ticks_;
    if (MemberDestroyed())  // every phase: the take, the swap and the leave read the member too
    {
        return;
    }
    switch (phase_)
    {
        case Phase::SettlingPlayerOne: Settle(); break;
        case Phase::Taking: AwaitTake(); break;
        case Phase::Leaving: Carry(leave_.Next(MemberReleased())); break;
        case Phase::Playing:
            // Before the member tick: the tick's scopes answer the party leader with player 2's member
            // (player_two/leader_scope.h), which CheckMemberPlayable's leader test must not see.
            RetakeWhenFree();
            CheckMemberPlayable();
            if (phase_ == Phase::Playing && installed_.memberMovement && generation::Member() != nullptr)
            {
                member_movement::Tick(playerTwoController_, generation::Member());
            }
            break;
        case Phase::Swapping: AwaitSwap(); break;
        case Phase::Out: break;
    }
    if (phase_ != Phase::Leaving && phase_ != Phase::Out && generation::Active())
    {
        KeepOwnCamera();
    }
}

void Session::KeepOwnCamera() noexcept
{
    // Resets logged one by one; later ones only counted on the status line.
    constexpr std::uint32_t kLoggedResets = 3;
    if (!installed_.viewTarget || view_target::OnOwnCamera(playerTwoController_))
    {
        return;
    }
    ++viewTargetResets_;
    void*      moved = view_target::Current(playerTwoController_);
    const bool aimed = view_target::Aim(playerTwoController_);
    if (viewTargetResets_ <= kLoggedResets)
    {
        Log(L"player 2's camera manager was viewing {}; aimed back at its camera actor: {}", NameOf(moved), YesNo(aimed));
    }
}

void Session::LogPlayerOneAfterJoin() noexcept
{
    Log(L"after the join, tick {}: player 1's pawn {} and leader {}", ticks_,
        NameOf(ReadPointer(playerOneController_, facts::kControllerPawn)),
        NameOf(ReadPointer(playerOneController_, facts::kControllerLeader)));
}

void Session::Settle() noexcept
{
    if (ticks_ <= kPlayerOneWatchTicks)
    {
        LogPlayerOneAfterJoin();
    }
    const bool settled = ticks_ >= kPlayerOneWatchTicks &&
                         ReadPointer(playerOneController_, facts::kControllerPawn) != nullptr &&
                         ReadPointer(playerOneController_, facts::kControllerLeader) != nullptr;
    if (!settled)
    {
        return;
    }
    if (!installed_.member || !generation::Active())
    {
        Log(L"player 2 has its own controller; no party member is taken ({})",
            !installed_.member                   ? L"the party-member stage is not installed"
            : generation::FaultReason() != nullptr ? generation::FaultReason()
                                                   : L"player 2's stages were withdrawn");
        phase_ = Phase::Playing;
        return;
    }
    if (!party_member::FieldGateOpen())
    {
        if (!gateClosedLogged_)
        {
            gateClosedLogged_ = true;
            Log(L"player 2 waits for the field to take a party member");
        }
        return;
    }
    member_ = party_member::Choose(playerOneController_);
    if (member_ == nullptr)
    {
        Log(L"no party member is free for player 2; player 2 stays on its own controller without a character");
        phase_ = Phase::Playing;
        return;
    }
    playerOneBytes_ = party_member::ReadPlayerOneBytes(playerOneController_);
    LogTagCounts(L"before the take");
    if (!party_member::Take(playerTwoController_, member_))
    {
        member_ = nullptr;
        phase_  = Phase::Playing;
        return;
    }
    memberWeak_ = leader_scope::WeakOf(member_);  // watched from the take on
    Log(L"player 2 is taking {}", NameOf(member_));
    phase_ = Phase::Taking;
    ticks_ = 0;
}

void Session::AwaitTake() noexcept
{
    const auto lease = party_member::ReadLease(member_, playerTwoController_);
    if (Acquired(lease, member_, playerTwoController_))
    {
        const auto bytes = party_member::ReadPlayerOneBytes(playerOneController_);
        Log(L"player 2 took {} after {} tick(s); player 1's controller bytes +0x897/+0x8AB {:#04x}/{:#04x} -> "
            L"{:#04x}/{:#04x} ({})",
            NameOf(member_), ticks_, playerOneBytes_.at897, playerOneBytes_.at8AB, bytes.at897, bytes.at8AB,
            bytes == playerOneBytes_ ? L"unchanged" : L"CHANGED");
        LogTagCounts(L"after the take");
        generation::SetMember(member_);
        memberWeak_ = leader_scope::WeakOf(member_);
        if (installed_.fieldSettings)
        {
            Log(L"player 2's Field camera brought to player 1's settings: {}",
                field_camera::Seed() ? L"matches" : L"DOES NOT MATCH");
        }
        Log(L"player 2's Battle camera brought to player 1's state: {}",
            battle_camera::Seed() ? L"matches" : L"does not match (or not installed)");
        phase_ = Phase::Playing;
        return;
    }
    if (ticks_ < kTakeTicks)
    {
        return;
    }
    Log(L"player 2 did not get {} within {} ticks (driven by {}, player 2's pawn {}, leader {}); giving it back, "
        L"player 2 stays in on its own controller",
        NameOf(member_), kTakeTicks, NameOf(lease.memberEffective), NameOf(lease.playerTwoPawn),
        NameOf(lease.playerTwoLeader));
    generation::Fault(L"the party member was not handed over in time");
    party_member::GiveBack(playerTwoController_);  // member_ stays recorded: the leave waits for this release
    phase_ = Phase::Playing;
}

/// Player 2's member destroyed under player 2 (a retry or checkpoint reload rebuilt the party without a
/// map load): nothing touches it from here on, and it is not given back (the give-back would reach
/// a destroyed object). True if that happened this tick.
bool Session::MemberDestroyed() noexcept
{
    if (member_ == nullptr || memberWeak_ == 0 || leader_scope::StillNames(memberWeak_, member_))
    {
        return false;
    }
    Log(L"player 2's member was destroyed under player 2 (the party was rebuilt without a field exit being seen); "
        L"player 2 lets go of it and joins again");
    generation::SetMember(nullptr);  // player 2's member movement and scopes stop at once
    camera_override::MemberDestroyed();
    // Player 2's controller still names the destroyed character: cleared, so neither the game's
    // give-back nor the controller's unpossess reads it (RE-p2-retry-reload.md §3.2, recipe 3).
    for (const std::uint32_t field : {facts::kControllerLeader, facts::kControllerPawn})
    {
        if (ReadPointer(playerTwoController_, field) == member_)
        {
            WriteField<void*>(playerTwoController_, field, nullptr);
            Log(L"player 2's controller field {:#x} named the destroyed character; cleared", field);
        }
    }
    member_                 = nullptr;
    memberWeak_             = 0;
    const bool leaveAsked   = phase_ == Phase::Leaving;  // a leave player 1 asked for stays a leave
    LeaveNow();  // a clean leave (nothing to give back now), and the rejoin once the field is ready
    if (leaveAsked)
    {
        rejoinPending_ = false;
    }
    return true;
}

/// Player 2 without a character (none was free: a solo story party) takes one as soon as one is.
void Session::RetakeWhenFree() noexcept
{
    // MemberReleased with no member: player 2's controller holds no pawn or leader (a give-back that
    // never completed keeps it holding one).
    if (member_ != nullptr || !installed_.member || !generation::Active() || ticks_ % kRetakeTicks != 0 ||
        !party_member::FieldGateOpen() || !MemberReleased())
    {
        return;
    }
    void* free = party_member::Choose(playerOneController_, false);
    if (free == nullptr)
    {
        return;
    }
    Log(L"a party member is free for player 2 again ({}); taking it", NameOf(free));
    phase_ = Phase::SettlingPlayerOne;
    ticks_ = kPlayerOneWatchTicks;  // player 1 is settled already: choose on the next tick
}

/// After a map load took player 2 out: the normal join, once the new map has been in the field state
/// with player 1's camera ready for a few seconds.
void Session::TryRejoin() noexcept
{
    if (!rejoinPending_)
    {
        return;
    }
    if (++rejoinWaited_ > kRejoinWaitTicks)
    {
        rejoinPending_ = false;
        Log(L"player 2 did not rejoin: the field did not become ready within {} ticks; press Ctrl+F7 to join",
            kRejoinWaitTicks);
        return;
    }
    if (refuseJoins_)
    {
        rejoinPending_ = false;  // Join would refuse and say why
        Log(L"player 2 does not rejoin after the map load: an earlier join raised an exception; restart the game to join");
        return;
    }
    // The field running again (the entry steps done, the party rebuilt): a join before that would be
    // undone by the entry's own unpossess of every non-leader (RE-p2-retry-reload.md §2.3).
    if (!own_controller::Ready() || (installed_.member && !party_member::FieldGateOpen()) || !field_exit::FieldReady())
    {
        rejoinTicks_ = 0;
        return;
    }
    if (++rejoinTicks_ < kRejoinSettleTicks)
    {
        return;
    }
    rejoinTicks_ = 0;
    if (++rejoinTries_ > kRejoinTries)
    {
        rejoinPending_ = false;
        Log(L"player 2 could not rejoin after the map load; press Ctrl+F7 to join");
        return;
    }
    Log(L"the new map is ready: player 2 joins again (try {})", rejoinTries_);
    Join();
    if (HasPlayerTwo())
    {
        rejoinPending_ = false;
    }
}

void Session::CheckMemberPlayable() noexcept
{
    if (member_ == nullptr || generation::Member() != member_ || !generation::Active() ||
        ticks_ % kPlayableCheckTicks != 0 || !party_member::FieldGateOpen())
    {
        return;
    }
    const wchar_t* why = party_member::WhyNotPlayable(member_);
    if (why == nullptr)
    {
        return;
    }
    Log(L"player 2's {} is {}: giving it back to choose again among the deployed party", NameOf(member_), why);
    generation::SetMember(nullptr);  // player 2's member movement and scopes stop at once
    if (!party_member::GiveBack(playerTwoController_))
    {
        Log(L"player 2's {} could not be given back (no trigger actor, or the switch faulted)", NameOf(member_));
    }
    phase_ = Phase::Swapping;
    ticks_ = 0;
}

void Session::AwaitSwap() noexcept
{
    if (MemberReleased())
    {
        Log(L"player 2's {} is back with its AI after {} tick(s); choosing again", NameOf(member_), ticks_);
        member_ = nullptr;
        phase_  = Phase::SettlingPlayerOne;
        ticks_  = kPlayerOneWatchTicks;  // player 1 is settled already: choose on the next tick
        return;
    }
    if (ticks_ >= kTakeTicks)
    {
        Log(L"player 2's {} did not return to its AI within {} ticks; player 2 stays without a character until "
            L"player 2's controller holds none, then takes a free one (a leave still waits for the release)",
            NameOf(member_), kTakeTicks);
        // Not player 2's member any more: a leave waits on player 2's controller holding nothing
        // (MemberReleased with no member), and RetakeWhenFree waits for the same.
        member_ = nullptr;
        phase_  = Phase::Playing;
    }
}

void Session::Leave() noexcept
{
    rejoinPending_ = false;  // a leave asked for: no rejoin after a later map load
    if (!HasPlayerTwo())
    {
        return;
    }
    camera_override::PlayerTwoOut();  // the member's camera delegates back to the engine's, while it is still held
    lock_on::PlayerTwoOut();
    generation::Withdraw();  // every wrapper passes through from here on
    phase_ = Phase::Leaving;
    ticks_ = 0;
    Carry(leave_.Begin(HoldsMember()));
}

bool Session::LeaveNow() noexcept
{
    if (!HasPlayerTwo())
    {
        return true;
    }
    // Player 2's hold on the old world's objects goes before it does, even if the removal below fails.
    camera_override::PlayerTwoOut();
    lock_on::PlayerTwoOut();
    generation::Withdraw();
    if (HoldsMember())
    {
        // A map load cannot wait for the release; the old world is torn down with it.
        party_member::GiveBack(playerTwoController_);
        const auto lease = party_member::ReadLease(member_, playerTwoController_);
        Log(L"map load: {} given back without waiting (released already: {})", NameOf(member_),
            YesNo(Released(lease, member_)));
    }
    Teardown();
    if (!HasPlayerTwo())
    {
        rejoinPending_ = true;  // join again once the new map is ready
        rejoinTicks_   = 0;
        rejoinTries_   = 0;
        rejoinWaited_  = 0;
    }
    return !HasPlayerTwo();
}

void Session::Carry(LeaveAction action) noexcept
{
    switch (action)
    {
        case LeaveAction::None: break;
        case LeaveAction::Release:
            if (!party_member::GiveBack(playerTwoController_))
            {
                Log(L"leave: {} could not be given back (no trigger actor, or the switch faulted)", NameOf(member_));
            }
            break;
        case LeaveAction::Teardown: Teardown(); break;
        case LeaveAction::ReportStuck:
            Log(L"leave stopped: {} did not return to its AI within {} ticks; player 2 stays in (Ctrl+F8 tries again)",
                NameOf(member_), LeaveSequence::kReleaseTicks);
            break;
    }
}

void Session::Teardown() noexcept
{
    if (member_ != nullptr)
    {
        const auto bytes = party_member::ReadPlayerOneBytes(playerOneController_);
        Log(L"leave: {} is back with its AI; player 1's controller bytes {:#04x}/{:#04x}", NameOf(member_),
            bytes.at897, bytes.at8AB);
        LogTagCounts(L"after the give-back");
    }
    const auto actors = leftover_actors::Collect(playerTwoController_);
    const auto result = leftover_actors::Destroy(actors, playerTwoController_);
    const bool out    = players_.Remove();
    leave_.Finish();
    Log(L"leave: {} leftover actor(s) destroyed ({} failed); player 2 {}", result.destroyed, result.failed,
        out ? L"left" : L"could NOT be removed");
    if (member_ != nullptr)
    {
        LogTagCounts(L"after the teardown");
    }
    if (out)
    {
        Forget();
    }
    else
    {
        phase_ = Phase::Playing;  // a later Ctrl+F8 or map load tries again
    }
}

bool Session::PlaysMember() const noexcept
{
    return HasPlayerTwo() && phase_ == Phase::Playing && generation::Member() != nullptr;
}

bool Session::HoldsMember() const noexcept
{
    return member_ != nullptr || ReadPointer(playerTwoController_, facts::kControllerLeader) != nullptr;
}

bool Session::MemberReleased() const noexcept
{
    const auto lease = party_member::ReadLease(member_, playerTwoController_);
    if (member_ == nullptr)
    {
        return lease.playerTwoPawn == nullptr && lease.playerTwoLeader == nullptr;
    }
    return Released(lease, member_);
}

void Session::Forget() noexcept
{
    camera_override::PlayerTwoOut();
    lock_on::PlayerTwoOut();
    generation::Withdraw();
    primary_controller::Unbind();
    leave_.Finish();
    phase_               = Phase::Out;
    playerOne_           = nullptr;
    playerOneController_ = nullptr;
    playerTwoController_ = nullptr;
    member_              = nullptr;
    ticks_               = 0;
    viewTargetResets_    = 0;
}

void Session::LogStatus() noexcept
{
    const platform::frame_watch::Job job(L"status line");
    if (!HasPlayerTwo())
    {
        return;
    }
    const auto     settings = field_camera::SettingsCounters().Take();
    const auto     update   = field_camera::UpdateCounters().Take();
    const auto     input    = pad_sticks::InputCounters().Take();
    const auto     move     = pad_sticks::MoveCounters().Take();
    const wchar_t* fault    = generation::FaultReason();
    Log(L"player 2: member {}, generation {}{}; field settings mirrored {} passed {} mismatched {}; camera update as primary {} passed {} "
        L"refused {}; per-pad sticks {} (input acted {} refused {}, movement acted {} refused {})",
        NameOf(generation::Member()), generation::Active() ? L"active" : L"withdrawn",
        fault ? L" (fault: " + std::wstring(fault) + L")" : std::wstring(), settings.acted, settings.passed,
        field_camera::TakeMismatches(), update.acted, update.passed, update.refused,
        pad_sticks::Enabled() ? L"on" : L"off", input.acted, input.refused, move.acted, move.refused);
    const auto member = member_movement::Counters().Take();
    Log(L"  player 2's view: own camera {} (moved off it {} time(s) so far); member movement ticked {} skipped {} "
        L"refused {}, leader test answered yes {} time(s)",
        YesNo(view_target::OnOwnCamera(playerTwoController_)), viewTargetResets_, member.acted, member.passed,
        member.refused, member_movement::TakeLeaderAnswers());
    scope_leaks::LogStatus();
    pad_buttons::LogStatus();
    move_state::LogCensus();
}

}  // namespace lss::player_two
