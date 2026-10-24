// Session: a seat's party member, the leave, and the status line; the session's seats and shared tick.

#include "player_two/session/session.h"

#include "facts/controller.h"
#include "platform/frame_watch.h"
#include "platform/game_thread_sites.h"
#include "platform/log.h"
#include "player_two/battle/battle_camera.h"
#include "player_two/battle/command_menu_two.h"
#include "player_two/battle/command_menu_view.h"
#include "player_two/battle/command_rules.h"
#include "player_two/battle/menu_focus.h"
#include "player_two/camera/camera_override.h"
#include "player_two/camera/field_camera.h"
#include "player_two/camera/view_target.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/input/member_movement.h"
#include "player_two/input/move_census.h"
#include "player_two/input/pad_buttons.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/object_fields.h"
#include "player_two/rides/own_ride.h"
#include "player_two/rides/shared_ride.h"
#include "player_two/scopes/leader_answer.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/scopes/scope_leaks.h"
#include "player_two/session/field_exit.h"
#include "player_two/session/handover_watch.h"
#include "player_two/session/leave_watch.h"
#include "player_two/session/leftover_actors.h"
#include "player_two/session/own_controller.h"
#include "player_two/session/seat_out.h"
#include "player_two/session/session_rules.h"
#include "player_two/session/tag_counts.h"
#include "players/seats.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two {

namespace {

using platform::Log;

// A Seat calls modules that are still player 2's (own_ride, the camera seeds and the destroyed member's camera
// override, member_movement under player 2's primary scope, handover_watch; command_menu_two is gated): raising the
// built seats waits for them to take the seat (plan/seats.md step 3).
static_assert(players::kSeatsBuilt == 1, "a Seat still calls player 2's modules: see above");

/// Ticks player 1's controller is watched after a join (it has been seen to name no pawn for one).
constexpr std::uint32_t kPlayerOneWatchTicks = 3;

/// Ticks to wait for the switch to hand the member over before giving it back.
constexpr std::uint32_t kTakeTicks = 120;

/// How often, in ticks, player 2's member is checked for its battle controller component: character
/// state that no party funnel reports (RE-p2-session-levers.md §3.2).
constexpr std::uint32_t kControllerCheckTicks = 30;

/// The game's party funnels (player_two/session/party_events.h) drive the party checks. These slow checks
/// are the backstop for a party change that passes neither funnel (story override party data); a
/// change found by them is logged as such. Without the funnels, the checks keep the earlier rate.
constexpr std::uint32_t kBackstopTicks        = 600;
constexpr std::uint32_t kBackstopTicksNoEvents = 30;

/// After a game party change while player 2 has no character: ticks during which player 2 looks for
/// a free member every tick (the game spawns a member that joins on a later tick than its status).
constexpr std::uint32_t kRetakeLookTicks = 600;

/// Lines logged for the game's party changes heard (a yield or a retake is always logged).
constexpr std::uint32_t kEventLines = 200;

/// Player 2 in, waiting on the 2nd slot's member (its AI held by a story script, or not free yet):
/// ticks between checks. A script lets go without an event; the check is a few reads.
constexpr std::uint32_t kSlotWaitTicks = 10;

/// While player 2 plays a member: ticks between checks of its AI (a story script putting it into
/// LEAD gets it back) and of the 2nd slot (a reorder no party apply reported).
constexpr std::uint32_t kHeldAiTicks    = 6;
constexpr std::uint32_t kSlotCheckTicks = 60;

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

void Seat::LogTagCounts(const wchar_t* when) noexcept
{
    if (!tag_counts::Installed() || !platform::Enabled(platform::Level::Debug))
    {
        return;  // a snapshot (Debug, dev tools): nothing is read below that level
    }
    const auto counts = tag_counts::Read(member_);
    const auto text   = [](const std::optional<std::int32_t>& count) {
        return count ? std::to_wstring(*count) : std::wstring(L"?");
    };
    platform::LogDebug(L"tag counts {}: {}'s first tag {}, trigger tag {}", when, NameOf(member_), text(counts.member),
        text(counts.trigger));
}

void Seat::Tick() noexcept
{
    if (!In())
    {
        OutsideTick();  // the rejoin after a map load, or the wait for a free party member
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
            // (player_two/scopes/leader_scope.h), which CheckMemberPlayable's leader test must not see.
            RetakeWhenFree();
            if (!In())
            {
                break;  // left to wait for a free member
            }
            CheckMemberPlayable();
            if (seat_ == players::kPlayerTwoSeat)
            {
                command_menu_two::Tick();  // player 2's command controller, built and set up per world
            }
            if (phase_ == Phase::Playing && installed_.memberMovement && generation::MemberOf(seat_) != nullptr)
            {
                member_movement::Tick(seat_, controller_, generation::MemberOf(seat_));
            }
            break;
        case Phase::Swapping: AwaitSwap(); break;
        case Phase::Out: break;
    }
    if (phase_ != Phase::Leaving && phase_ != Phase::Out && generation::Active(seat_))
    {
        WatchOwnCamera();
    }
}

void Seat::WatchOwnCamera() noexcept
{
    // Watch only (RE-p2-session-levers.md §2): the game writes player 2's view target once, at spawn
    // (InitializeFor; the join repeats that aim once), and no game code retargets a second player's
    // controller afterwards, so nothing re-aims it here. A move off its camera actor is logged, one
    // line per move for the first few; later ones are only counted on the status line.
    constexpr std::uint32_t kLoggedMoves = 3;
    const bool              own          = !installed_.viewTarget || view_target::OnOwnCamera(controller_);
    if (own || !viewOnOwnCamera_)
    {
        viewOnOwnCamera_ = own;
        return;
    }
    viewOnOwnCamera_ = false;
    ++viewTargetResets_;
    if (viewTargetResets_ <= kLoggedMoves)
    {
        Log(L"WARNING: player 2's camera manager now views {}, not its camera actor (the game moved it; not re-aimed)",
            NameOf(view_target::Current(controller_)));
    }
}

void Seat::LogPlayerOneAfterJoin() noexcept
{
    platform::LogDebug(L"after the join, tick {}: player 1's pawn {} and leader {}", ticks_,
        NameOf(ReadPointer(playerOneController_, facts::kControllerPawn)),
        NameOf(ReadPointer(playerOneController_, facts::kControllerLeader)));
}

void Seat::Settle() noexcept
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
    if (!installed_.member || !generation::Active(seat_))
    {
        Log(L"player 2 has its own controller; no party member is taken ({})",
            !installed_.member                          ? L"the party-member stage is not installed"
            : generation::FaultReason(seat_) != nullptr ? generation::FaultReason(seat_)
                                                        : L"player 2's stages were withdrawn");
        phase_ = Phase::Playing;
        return;
    }
    if (!party_member::GameReady())
    {
        if (!gateClosedLogged_)
        {
            gateClosedLogged_ = true;
            Log(L"player 2 waits for the game to be ready (a load) to take a party member");
        }
        return;
    }
    if (sharedRide_)
    {
        // Player 1 rides a vehicle the whole party shares: player 2 takes no member until it is over (RetakeWhenFree).
        member_ = nullptr;
        phase_  = Phase::Playing;
        return;
    }
    handover_watch::PartyOrder(L"choosing player 2's member", nullptr);
    const auto choice = party_member::ChooseSlot(seat_, playerOneController_);
    member_           = choice.member;
    if (choice.wait == party_member::SlotWait::NoSlot)
    {
        LeaveToAwait(L"the party has no member for player 2 any more");
        return;
    }
    if (member_ == nullptr)
    {
        WaitForSlot(choice);
        phase_ = Phase::Playing;
        return;
    }
    waitingForSlot_ = false;
    slotWaitLogged_ = 0;
    playerOneBytes_ = party_member::ReadPlayerOneBytes(playerOneController_);
    LogTagCounts(L"before the take");
    handover_watch::Snapshot(L"before the take", member_);
    if (!party_member::Take(seat_, controller_, member_))
    {
        member_ = nullptr;
        phase_  = Phase::Playing;
        return;
    }
    handover_watch::Snapshot(L"just after the take's switch", member_);
    memberWeak_ = leader_scope::WeakOf(member_);  // watched from the take on
    Log(L"player 2 is taking {}", NameOf(member_));
    phase_ = Phase::Taking;
    ticks_ = 0;
}

void Seat::AwaitTake() noexcept
{
    const auto lease   = party_member::ReadLease(member_, controller_);
    const auto outcome = TakeOutcomeOf(Acquired(lease, member_, controller_), ticks_, kTakeTicks);
    if (outcome == TakeOutcome::Took)
    {
        const auto bytes = party_member::ReadPlayerOneBytes(playerOneController_);
        Log(L"player 2 took {} after {} tick(s); player 1's controller bytes +0x897/+0x8AB {:#04x}/{:#04x} -> "
            L"{:#04x}/{:#04x} ({})",
            NameOf(member_), ticks_, playerOneBytes_.at897, playerOneBytes_.at8AB, bytes.at897, bytes.at8AB,
            bytes == playerOneBytes_ ? L"unchanged" : L"CHANGED");
        LogTagCounts(L"after the take");
        generation::SetMember(seat_, member_);
        memberWeak_  = leader_scope::WeakOf(member_);
        retakeLooks_ = 0;  // a look window armed before the take would drain for nothing
        if (installed_.fieldSettings)
        {
            Log(L"player 2's Field camera brought to player 1's settings: {}",
                field_camera::Seed(seat_) ? L"matches" : L"DOES NOT MATCH");
        }
        Log(L"player 2's Battle camera brought to player 1's state: {}",
            battle_camera::Seed() ? L"matches" : L"does not match (or not installed)");
        phase_ = Phase::Playing;
        return;
    }
    if (outcome == TakeOutcome::Wait)
    {
        return;
    }
    Log(L"player 2 did not get {} within {} ticks (driven by {}, player 2's pawn {}, leader {}); giving it back, "
        L"player 2 stays in on its own controller",
        NameOf(member_), kTakeTicks, NameOf(lease.memberEffective), NameOf(lease.playerTwoPawn),
        NameOf(lease.playerTwoLeader));
    generation::Fault(seat_, L"the party member was not handed over in time");
    party_member::GiveBack(seat_, controller_);  // member_ stays recorded: the leave waits for this release
    phase_ = Phase::Playing;
}

/// Player 2's member destroyed under player 2 (a retry or checkpoint reload rebuilt the party without a
/// map load): nothing touches it from here on, and it is not given back (the give-back would reach
/// a destroyed object). True if that happened this tick.
bool Seat::MemberDestroyed() noexcept
{
    if (member_ == nullptr || memberWeak_ == 0 || leader_scope::StillNames(memberWeak_, member_))
    {
        return false;
    }
    Log(L"player 2's member was destroyed under player 2 (the party was rebuilt without a field exit being seen); "
        L"player 2 lets go of it and joins again");
    generation::SetMember(seat_, nullptr);  // player 2's member movement and scopes stop at once
    // Riding, player 2's controller possessed the member's chocobo: its pawn field names that instead.
    const auto forgotten = own_ride::Forget();
    camera_override::MemberDestroyed();
    // Player 2's controller still names the destroyed character: cleared, so neither the game's
    // give-back nor the controller's unpossess reads it (RE-p2-retry-reload.md §3.2, recipe 3).
    for (const std::uint32_t field : {facts::kControllerLeader, facts::kControllerPawn})
    {
        const void* named = ReadPointer(controller_, field);
        const bool vehicle = named == forgotten.chocobo || named == forgotten.scooter;
        if (named == member_ || (field == facts::kControllerPawn && named != nullptr && vehicle))
        {
            WriteField<void*>(controller_, field, nullptr);
            Log(L"player 2's controller field {:#x} named the destroyed character; cleared", field);
        }
    }
    member_                 = nullptr;
    memberWeak_             = 0;
    const bool leaveAsked   = phase_ == Phase::Leaving;  // a leave player 1 asked for stays a leave
    LeaveNow();  // a clean leave (nothing to give back now), and the rejoin once the field is ready
    if (leaveAsked)
    {
        outside_.End();
    }
    return true;
}

std::uint32_t Seat::BackstopTicks() const noexcept
{
    return installed_.partyEvents ? kBackstopTicks : kBackstopTicksNoEvents;
}

/// Player 2 in without a character (its give-back ran out of time, or the take failed) takes a member
/// as soon as one is free: looked for after each game party change, and by the slow backstop. When
/// the backstop finds none, player 2 leaves and waits outside the game.
void Seat::RetakeWhenFree() noexcept
{
    const bool looking = retakeLooks_ > 0;
    if (looking && party_member::GameReady())
    {
        --retakeLooks_;  // the window runs only while the game is ready: no member is taken during a load
    }
    // MemberReleased with no member: player 2's controller holds no pawn or leader (a give-back that
    // never completed keeps it holding one). A wait on the 2nd slot is checked every few ticks: the
    // game frees it without an event (its script lets go of the member's AI).
    const bool due  = looking || ticks_ % (waitingForSlot_ ? kSlotWaitTicks : BackstopTicks()) == 0;
    const auto hold = session_rules::HoldOf(member_, waitingForSlot_, sharedRide_, generation::MemberOf(seat_),
                                            generation::Active(seat_));
    if (!session_rules::MayRetake(hold) || !installed_.member || !due || !party_member::GameReady() ||
        !MemberReleased())
    {
        return;
    }
    const auto choice = party_member::ChooseSlot(seat_, playerOneController_, false);
    void*      free   = choice.member;
    if (free == nullptr)
    {
        if (choice.wait != party_member::SlotWait::NoSlot)
        {
            WaitForSlot(choice);
        }
        else if (!looking)  // a member the game spawns joins a few ticks after its party change: not given up then
        {
            LeaveToAwait(L"the party has no member for player 2");
        }
        return;
    }
    retakeLooks_ = 0;
    Log(L"a party member is free for player 2 again ({}; {}); taking it", NameOf(free),
        waitingForSlot_          ? L"the 2nd slot's member is free now"
        : looking                ? L"after the game's party change"
        : installed_.partyEvents ? L"found by the slow check: no game party change was heard for it"
                                 : L"found by the party check");
    phase_ = Phase::SettlingPlayerOne;
    ticks_ = kPlayerOneWatchTicks;  // player 1 is settled already: choose on the next tick
}

void Seat::SetSharedRide(bool on) noexcept
{
    if (on == sharedRide_)
    {
        return;
    }
    sharedRide_ = on;
    if (!In())
    {
        return;
    }
    if (!on)
    {
        Log(L"the shared ride is over: player 2 takes the 2nd slot's member again once it is free");
        retakeLooks_ = kRetakeLookTicks;
        return;
    }
    if (MemberHeld())
    {
        Yield(L"standing aside: player 1 rides a vehicle the whole party shares, with the party as in the one-player game");
    }
    else
    {
        Log(L"player 2 stands aside (no member held): player 1 rides a vehicle the whole party shares");
    }
}

/// Player 2 gives its member back through the game's release and chooses again once it is released.
void Seat::Yield(const wchar_t* why) noexcept
{
    own_ride::Release(L"player 2 gives the member back");  // while player 2's scopes still hold
    Log(L"player 2's {} is {}: giving it back to choose again among the deployed party", NameOf(member_), why);
    handover_watch::Snapshot(L"the give-back", member_);
    generation::SetMember(seat_, nullptr);  // player 2's member movement and scopes stop at once
    phase_ = Phase::Swapping;        // before the game's switch runs: a party event inside it sees the swap
    ticks_ = 0;
    if (!party_member::GiveBack(seat_, controller_))
    {
        Log(L"player 2's {} could not be given back (no trigger actor, or the switch faulted)", NameOf(member_));
    }
}

bool Seat::MemberHeld() const noexcept
{
    return member_ != nullptr && (phase_ == Phase::Playing || phase_ == Phase::Taking);
}

void Seat::EventLine(const std::wstring& text) noexcept
{
    if (eventLines_ < kEventLines)
    {
        ++eventLines_;
        Log(L"party event: {}", text);
    }
}

/// The game is about to make `character` the leader of `controller` (player_two/session/party_events.h). If it
/// is player 2's member and player 1's controller, player 2 lets go first, so the two controllers never
/// both claim it; the game's switch then seats player 1 on it.
void Seat::OnLeaderChanging(void* controller, void* character) noexcept
{
    using session_rules::LeaderEventAction;
    const session_rules::LeaderEvent event{In(),
                                           AwaitingMember(),
                                           controller == controller_,
                                           controller == playerOneController_,
                                           character != nullptr && character == member_,
                                           MemberHeld(),
                                           member_ == nullptr};
    const auto action = session_rules::DecideLeaderEvent(event);
    if (In())
    {
        const void* before = ReadPointer(controller, facts::kControllerLeader);
        if (action == LeaderEventAction::Warn)
        {
            Log(L"WARNING: party event: the game makes {} the leader of player 2's controller (was {}); a game leader "
                L"change ran inside a player-2 scope. Not acted on",
                NameOf(character), NameOf(before));
            return;
        }
        EventLine(std::format(L"the game makes {} the leader of {} (was {})", NameOf(character),
                              event.playerOnesController ? L"player 1's controller" : L"a controller that is not player 1's",
                              NameOf(before)));
    }
    switch (action)
    {
        case LeaderEventAction::Yield: Yield(L"about to become player 1's leader (the game's leader change)"); break;
        case LeaderEventAction::ArmLook: retakeLooks_ = kRetakeLookTicks; break;  // the member the game moved off may be free
        case LeaderEventAction::Warn:
        case LeaderEventAction::None: break;
    }
}

/// The game changed a party member's status (player_two/session/party_events.h): player 2's member is checked
/// against the deployed party at once, or, without one, player 2 looks for a free member.
void Seat::OnStatusChanged(std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept
{
    if (!In() && !AwaitingMember())
    {
        return;
    }
    EventLine(std::format(L"the game changed party member type {}'s status {} -> {}", playerType, before, after));
    handover_watch::PartyOrder(L"after a party status change", MemberHeld() ? member_ : nullptr);
    // Not the slot: a squad change commits its statuses one by one and orders the list afterwards
    // (the chapter 7 log), so the order is final only at the party apply.
    CheckAfterPartyChange(false);
}

/// The game finished applying new party data (player_two/session/party_events.h): a squad or formation
/// change, a load, a script, an equipment or costume change. Every character it needed exists now.
void Seat::OnPartyApplied() noexcept
{
    if (!In() && !AwaitingMember())
    {
        return;
    }
    EventLine(L"the game applied the party data (its party stream finished)");
    handover_watch::PartyOrder(L"after the game's party apply", MemberHeld() ? member_ : nullptr);
    CheckAfterPartyChange(true);
}

/// After a game party change: player 2's member is checked against the deployed party at once (and,
/// with `followSlot`, against the 2nd slot), or, without one (or waiting outside the game), player 2
/// looks for a free member.
void Seat::CheckAfterPartyChange(bool followSlot) noexcept
{
    if (member_ == nullptr)
    {
        retakeLooks_ = kRetakeLookTicks;  // used by RetakeWhenFree, or JoinWhenMemberFree while player 2 is out
        return;
    }
    if (!MemberHeld() || !generation::Active(seat_))
    {
        return;
    }
    if (const wchar_t* why = party_member::WhyNotPlayable(member_))
    {
        Yield(why);
    }
    else if (followSlot)
    {
        FollowSlot(L"after the game's party apply", true);  // one Info line per party apply
    }
}

/// Player 2 plays the 2nd slot: when it names another member, player 2's is given back and the slot's
/// is taken (or waited on), even though player 2's is still in the party.
void Seat::FollowSlot(const wchar_t* when, bool log) noexcept
{
    if (phase_ != Phase::Playing || member_ == nullptr || generation::MemberOf(seat_) != member_ ||
        !party_member::GameReady())
    {
        return;
    }
    // Logged from the party-apply caller only: the party order after the game's apply is the one Info line a field
    // report needs for a hand-over question (the handover watch, which listed the whole order, is a dev tool now).
    // The slow slot check asks every kSlotCheckTicks and stays quiet (the v0.83.0 play check: a line per poll).
    const auto choice = party_member::ChooseSlot(seat_, playerOneController_, log);
    const auto held   = party_member::PlayerTypeOf(member_);
    if (choice.wait == party_member::SlotWait::NoSlot || !held || choice.type == *held)
    {
        return;  // no slot: the party checks give player 2's member up if it left
    }
    Log(L"the 2nd slot of the party order is now type {} ({}), not player 2's {} (type {})", choice.type, when,
        NameOf(member_), *held);
    Yield(L"no longer in the 2nd slot of the party order");
}

/// Player 2 stays in without a member while the 2nd slot's is not free; the reason is logged when it
/// changes.
void Seat::WaitForSlot(const party_member::SlotChoice& choice) noexcept
{
    waitingForSlot_  = true;
    const auto key   = static_cast<std::uint32_t>(choice.type) | static_cast<std::uint32_t>(choice.wait) << 8 |
                     static_cast<std::uint32_t>(choice.verdict) << 12 | static_cast<std::uint32_t>(choice.ai.state) << 16 |
                     static_cast<std::uint32_t>(choice.ai.brainIndex) << 24;
    if (key == slotWaitLogged_)
    {
        return;
    }
    slotWaitLogged_ = key;
    if (choice.wait == party_member::SlotWait::LinkPartner)
    {
        // RE-p2-synergy-skills.md S-SYNTAKE: the game gives a partner back only at the link's member end.
        Log(L"player 2 waits for party member type {} (the 2nd slot, ordinal {}): a synergy partner the game has not "
            L"given back yet (its link: ability kind {} mode {}, partner state {}, started {} ended {}); taken once the "
            L"game ends its part of the link",
            choice.type, choice.ordinal, choice.link.kind, choice.link.mode,
            choice.link.state[choice.link.partner], choice.link.started[choice.link.partner],
            choice.link.ended[choice.link.partner]);
        return;
    }
    Log(L"player 2 waits for party member type {} (the 2nd slot, ordinal {}): {} (AI brain {} state {}); the game's "
        L"party order chooses player 2's member, so a menu reorder ends the wait",
        choice.type, choice.ordinal,
        choice.wait == party_member::SlotWait::NotEligible ? Describe(choice.verdict)
        : choice.wait == party_member::SlotWait::Unread    ? L"the party could not be read"
                                                           : Describe(AssessAi(choice.ai)),
        choice.ai.brainIndex, choice.ai.state);
}

/// A story script that puts player 2's member into LEAD gets it back: the LEAD runs only under its AI.
void Seat::CheckHeldAi() noexcept
{
    // While player 2 rides their chocobo or a Wheelie the member is her AI's for the ride
    // (player_two/rides/chocobo_ride.h,
    // player_two/rides/wheelie_ride.h).
    if (own_ride::Riding() || ticks_ % kHeldAiTicks != 0 ||
        !AiTakesBack(party_member::ReadAi(member_)))
    {
        return;
    }
    Yield(L"put into LEAD by a story script (given back so the script can run; player 2 waits for it to follow again)");
}

/// The one readiness gate of a join: player 2's camera manager can be started safely, the game has started,
/// and the field runs again (the entry steps done, the party rebuilt: a join before that would be undone by the
/// entry's own unpossess of every non-leader, RE-p2-retry-reload.md §2.3). Null when ready, else why not, as the
/// join's refusal line says it. The same gate for the join by the key, the rejoin after a map load and the wait
/// for a free member.
const wchar_t* Seat::NotReadyToJoin() const noexcept
{
    if (!installed_.ownController || !own_controller::Ready())
    {
        return installed_.ownController
                   ? L"player 2's camera manager cannot be started safely yet (player 1's camera names are not set "
                     L"yet)"
                   : L"player 2's camera manager cannot be started safely yet (its stage is not installed)";
    }
    if (installed_.member && !party_member::GameReady())
    {
        return L"the game has not started yet (the title, or early in a load)";
    }
    if (!field_exit::FieldReady())
    {
        return L"the field is still being entered (a join now would be undone by it)";
    }
    return nullptr;
}

bool Seat::ReadyToJoinOutside() const noexcept
{
    return NotReadyToJoin() == nullptr;
}

/// Player 2 is out and wants back in (player_two/session/outside_join.h says when): the normal join once the new
/// map has been in the field state with player 1's camera ready for a few seconds; or, while player 2
/// waits for a free party member, a free member is looked for every tick after a game party change and
/// by a slow check otherwise, and player 2 joins when there is one.
void Seat::OutsideTick() noexcept
{
    using outside_join::Action;
    using outside_join::Mode;
    const Mode mode = outside_.Current();
    if (mode == Mode::None)
    {
        return;
    }
    const auto step = outside_.Tick(std::chrono::steady_clock::now(), refuseJoins_, ReadyToJoinOutside(), retakeLooks_);
    if (step.action == Action::GiveUp)
    {
        LogGaveUp(step.reason);
        return;
    }
    if (step.action != Action::TryJoin)
    {
        return;
    }
    if (mode == Mode::Rejoin)
    {
        Log(L"the new map is ready: player 2 joins again (try {})", step.attempt);
        Join();
        outside_.AfterJoin(Mode::Rejoin, In());
        return;
    }
    // Player 1 on a controller of another class for a moment is not ready either (the join would refuse).
    const auto one = players_.FirstPlayer(false);
    if (one.controller == nullptr || !own_controller::UsesNativeClasses(one.controller))
    {
        return;
    }
    const bool free   = FreeMember(one.controller) != nullptr;
    const bool riding = !free && shared_ride::Rides(one.controller);  // player 2 joins standing aside (Join)
    if (!free && !riding)
    {
        return;
    }
    Log(L"{} ({}): player 2 joins", free ? L"a party member is free for player 2" : L"player 1 rides a shared vehicle",
        step.looking ? L"after the game's party change" : L"found by the slow check");
    Join();  // ends the wait once player 2 is in; a refused or failed join leaves it, and is counted
    LogGaveUp(outside_.AfterJoin(Mode::Await, In()).reason);
}

/// The line for a wait that ended without player 2 (nothing for Reason::None).
void Seat::LogGaveUp(outside_join::Reason reason) noexcept
{
    using outside_join::Reason;
    switch (reason)
    {
        case Reason::None: break;
        case Reason::RejoinTimedOut:
            Log(L"player 2 did not rejoin: the field did not become ready within {} s; press Ctrl+F7 to join",
                outside_join::kRejoinWait.count());
            break;
        case Reason::RejoinRefused:
            Log(L"player 2 does not rejoin after the map load: an earlier join raised an exception; restart the game to join");
            break;
        case Reason::RejoinTriesExhausted:
            Log(L"player 2 could not rejoin after the map load; press Ctrl+F7 to join");
            break;
        case Reason::AwaitRefused:
            Log(L"player 2 stops waiting for a party member: an earlier join raised an exception; restart the game to join");
            break;
        case Reason::AwaitTriesExhausted:
            Log(L"player 2 could not join in {} tries (see above); press Ctrl+F7 to try again", outside_join::kTries);
            break;
    }
}

/// A party member player 2 could take, by the game's own member test (null if none). Quiet.
void* Seat::FreeMember(void* playerOneController) const noexcept
{
    return party_member::GameReady() ? party_member::ChooseSlot(seat_, playerOneController, false).member : nullptr;
}

/// Player 2 is out, and waits for a party member to be free before it joins.
void Seat::AwaitMember(const wchar_t* why) noexcept
{
    if (refuseJoins_)
    {
        Log(L"{}: player 2 stays out and does not wait (an earlier join raised an exception; restart the game to join)",
            why);
        return;
    }
    if (AwaitingMember())
    {
        Log(L"player 2 still waits for a free party member ({})", why);
        return;
    }
    outside_.BeginAwait();  // replaces a map load's rejoin, and has no time limit
    retakeLooks_ = 0;
    Log(L"{}: player 2 stays out (the game runs as with one player) and joins once a party member is free "
        L"(Ctrl+F8 stops the wait)",
        why);
}

/// Player 2 is in without a member and none is free: it leaves at once (nothing of player 2's is held
/// by the party), and waits outside the game for one.
void Seat::LeaveToAwait(const wchar_t* why) noexcept
{
    if (LeaveNow())
    {
        AwaitMember(why);
    }
}

/// The slow backstop of the party checks (the game's party changes are heard as they happen), and the
/// battle controller check, which no game event reports.
void Seat::CheckMemberPlayable() noexcept
{
    const auto hold = session_rules::HoldOf(member_, waitingForSlot_, sharedRide_, generation::MemberOf(seat_),
                                            generation::Active(seat_));
    if (hold != session_rules::Hold::Member || !party_member::GameReady())
    {
        return;
    }
    const wchar_t* why = ticks_ % BackstopTicks() == 0 ? party_member::WhyNotInParty(member_) : nullptr;
    if (why != nullptr && installed_.partyEvents)
    {
        Log(L"the slow party check found player 2's {} {}: no game party change was heard for it", NameOf(member_), why);
    }
    if (why == nullptr && ticks_ % kControllerCheckTicks == 0 && !party_member::HasBattleController(member_))
    {
        why = party_member::kWithoutBattleController;
    }
    if (why != nullptr)
    {
        Yield(why);
        return;
    }
    CheckHeldAi();
    if (phase_ == Phase::Playing && ticks_ % kSlotCheckTicks == 0)
    {
        FollowSlot(L"found by the slow slot check", false);
    }
}

void Seat::AwaitSwap() noexcept
{
    const bool released = MemberReleased();
    const auto lease    = party_member::ReadLease(member_, controller_);
    switch (SwapOutcomeOf(released, lease, playerOneController_, ticks_, kTakeTicks))
    {
        case SwapOutcome::Wait: return;
        case SwapOutcome::Released:
            Log(L"player 2's {} is back with its AI after {} tick(s); choosing again", NameOf(member_), ticks_);
            member_ = nullptr;
            phase_  = Phase::SettlingPlayerOne;
            ticks_  = kPlayerOneWatchTicks;  // player 1 is settled already: choose on the next tick
            return;
        case SwapOutcome::PlayerOnes:
            Log(L"player 2's {} is player 1's after {} tick(s), and player 2's controller holds nothing; choosing again",
                NameOf(member_), ticks_);
            member_ = nullptr;
            phase_  = Phase::SettlingPlayerOne;
            ticks_  = kPlayerOneWatchTicks;
            return;
        case SwapOutcome::TimedOut: break;
    }
    Log(L"player 2's {} did not return to its AI within {} ticks; player 2 stays without a character until "
        L"player 2's controller holds none, then takes a free one (a leave still waits for the release)",
        NameOf(member_), kTakeTicks);
    // Not player 2's member any more: a leave waits on player 2's controller holding nothing
    // (MemberReleased with no member), and RetakeWhenFree waits for the same.
    member_ = nullptr;
    phase_  = Phase::Playing;
}

void Seat::Leave() noexcept
{
    const bool awaited = AwaitingMember();
    outside_.End();  // a leave asked for: no rejoin after a later map load, no wait for a member
    if (awaited)
    {
        Log(L"player 2 no longer waits for a free party member");
    }
    if (!In())
    {
        return;
    }
    own_ride::Release(L"player 2 leaves");  // before the withdraw: the swap back needs player 2's scopes
    seat_out::LetGo(seat_);
    generation::Withdraw(seat_);  // every wrapper passes through from here on
    phase_ = Phase::Leaving;
    ticks_ = 0;
    Carry(leave_.Begin(HoldsMember()));
}

bool Seat::LeaveNow() noexcept
{
    if (!In())
    {
        return true;
    }
    // Player 2's hold on the old world's objects goes before it does, even if the removal below fails.
    own_ride::Release(L"a map load takes player 2 out");
    seat_out::LetGo(seat_);
    generation::Withdraw(seat_);
    if (HoldsMember())
    {
        // A map load cannot wait for the release; the old world is torn down with it.
        party_member::GiveBack(seat_, controller_);
        const auto lease = party_member::ReadLease(member_, controller_);
        Log(L"map load: {} given back without waiting (released already: {})", NameOf(member_),
            YesNo(Released(lease, member_)));
    }
    Teardown();
    if (!In())
    {
        outside_.BeginRejoin(std::chrono::steady_clock::now());  // join again once the new map is ready
    }
    return !In();
}

void Seat::Carry(LeaveAction action) noexcept
{
    switch (action)
    {
        case LeaveAction::None: break;
        case LeaveAction::Release:
            handover_watch::Snapshot(L"the leave's give-back", member_);
            if (!party_member::GiveBack(seat_, controller_))
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

void Seat::Teardown() noexcept
{
    if (member_ != nullptr)
    {
        const auto bytes = party_member::ReadPlayerOneBytes(playerOneController_);
        Log(L"leave: {} is back with its AI; player 1's controller bytes {:#04x}/{:#04x}", NameOf(member_),
            bytes.at897, bytes.at8AB);
        LogTagCounts(L"after the give-back");
    }
    const auto actors = leftover_actors::Collect(controller_);
    leave_watch::BeforeRemove(players_.Player(), controller_, actors);  // logging only (S-PC2LEAK)
    const bool out = players_.Remove();
    leave_watch::AfterRemove(out);
    leave_.Finish();
    if (out)
    {
        const auto result = leftover_actors::Destroy(actors);
        Log(L"leave: player 2 left; {} leftover actor(s) destroyed ({} failed); PlayerInput {}", result.destroyed,
            result.failed,
            actors.playerInput == nullptr ? L"none" : result.inputReleased ? L"marked pending kill" : L"NOT marked (rooted or moved)");
    }
    else
    {
        Log(L"leave: player 2 could NOT be removed; their actors are kept");
    }
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

bool Seat::PlaysMember() const noexcept
{
    return In() && phase_ == Phase::Playing && generation::MemberOf(seat_) != nullptr;
}

bool Seat::HoldsMember() const noexcept
{
    return member_ != nullptr || ReadPointer(controller_, facts::kControllerLeader) != nullptr;
}

bool Seat::MemberReleased() const noexcept
{
    const auto lease = party_member::ReadLease(member_, controller_);
    if (member_ == nullptr)
    {
        return lease.playerTwoPawn == nullptr && lease.playerTwoLeader == nullptr;
    }
    return Released(lease, member_);
}

void Seat::Forget() noexcept
{
    seat_out::LetGo(seat_);
    generation::End(seat_);  // the seat's player is removed: its controller is freed at the next collection
    primary_controller::Unbind(seat_);
    leave_.Finish();
    phase_               = Phase::Out;
    playerOne_           = nullptr;
    playerOneController_ = nullptr;
    controller_          = nullptr;
    member_              = nullptr;
    ticks_               = 0;
    retakeLooks_         = 0;
    waitingForSlot_      = false;
    slotWaitLogged_      = 0;
    viewTargetResets_    = 0;
    viewOnOwnCamera_     = true;
}

void Seat::LogStatus() noexcept
{
    if (!In())
    {
        return;
    }
    const auto     settings = field_camera::SettingsCounters(seat_).Take();
    const auto     update   = field_camera::UpdateCounters(seat_).Take();
    const auto     input    = pad_sticks::InputCounters().Take();
    const auto     move     = pad_sticks::MoveCounters().Take();
    const wchar_t* fault    = generation::FaultReason(seat_);
    Log(L"player 2: member {}, generation {}{}; field settings mirrored {} passed {} mismatched {}; camera update as primary {} passed {} "
        L"refused {}; per-pad sticks {} (input acted {} refused {}, movement acted {} refused {})",
        NameOf(generation::MemberOf(seat_)), generation::Active(seat_) ? L"active" : L"withdrawn",
        fault ? L" (fault: " + std::wstring(fault) + L")" : std::wstring(), settings.acted, settings.passed,
        field_camera::TakeMismatches(seat_), update.acted, update.passed, update.refused,
        pad_sticks::Enabled() ? L"on" : L"off", input.acted, input.refused, move.acted, move.refused);
    const auto member = member_movement::Counters(seat_).Take();
    Log(L"  player 2's view: own camera {} (moved off it {} time(s) so far); member movement ticked {} skipped {} "
        L"refused {}, leader test answered yes {} time(s)",
        YesNo(view_target::OnOwnCamera(controller_)), viewTargetResets_, member.acted, member.passed,
        member.refused, leader_answer::TakeAnswers());
}

// ---- the session: the built seats ----

Session::Session(std::uintptr_t removeLocalPlayer) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        seats_[static_cast<std::size_t>(seat)].emplace(seat, removeLocalPlayer, installed_, refuseJoins_);
    }
}

Seat* Session::At(int seat) noexcept
{
    return players::SeatBuilt(seat) ? &*seats_[static_cast<std::size_t>(seat)] : nullptr;
}

const Seat* Session::At(int seat) const noexcept
{
    return players::SeatBuilt(seat) ? &*seats_[static_cast<std::size_t>(seat)] : nullptr;
}

void Session::Join(int seat) noexcept
{
    if (Seat* s = At(seat))
    {
        s->Join();
    }
}

void Session::Leave(int seat) noexcept
{
    if (Seat* s = At(seat))
    {
        s->Leave();
    }
}

bool Session::LeaveNow() noexcept
{
    bool out = true;
    for (int seat = players::kSeatsBuilt; seat >= 1; --seat)  // the last first: the engine's list closes up behind
    {
        out = At(seat)->LeaveNow() && out;
    }
    return out;
}

void Session::Tick() noexcept
{
    generation::SetGameThread();  // the engine-tick callback runs on the thread that ticks the world
    platform::game_thread_sites::Run();  // the 1.005 placements (platform/deferred_sites.h), outside the game's ticks
    command_rules::Update();      // which member player 1 may not command (none without player 2)
    command_menu_view::KeepHidden();  // player 2's menu widget stays out of both halves
    if (AnyIn())
    {
        menu_focus::Watch();  // player 1's UI focus, logged on change (read-only)
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->Tick();
    }
}

void Session::LogStatus() noexcept
{
    const platform::frame_watch::Job job(L"status line");
    if (!AnyIn())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->LogStatus();
    }
    scope_leaks::LogStatus();
    pad_buttons::LogStatus();
    move_census::Log();  // dev tools
}

void Session::SetSharedRide(bool on) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->SetSharedRide(on);
    }
}

void Session::OnLeaderChanging(void* controller, void* character) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->OnLeaderChanging(controller, character);
    }
}

void Session::OnStatusChanged(std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->OnStatusChanged(playerType, before, after);
    }
}

void Session::OnPartyApplied() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->OnPartyApplied();
    }
}

bool Session::In(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr && s->In();
}

bool Session::AnyIn() const noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (At(seat)->In())
        {
            return true;
        }
    }
    return false;
}

bool Session::AwaitingMember(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr && s->AwaitingMember();
}

bool Session::PlaysMember(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr && s->PlaysMember();
}

bool Session::WaitsForMember(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr && s->WaitsForMember();
}

bool Session::StandsAside(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr && s->StandsAside();
}

const void* Session::LocalPlayer(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr ? s->LocalPlayer() : nullptr;
}

std::uint32_t Session::JoinCount(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr ? s->JoinCount() : 0;
}

const void* Session::PlayerOne() const noexcept
{
    return At(players::kPlayerTwoSeat)->PlayerOne();
}

#if LSS_DEV

namespace {

std::wstring PlayerTwoState(const Session& session)
{
    if (session.HasPlayerTwo())
    {
        return session.PlaysMember() ? L"playing" : L"in";
    }
    return session.AwaitingMember() ? L"awaiting" : L"out";
}

}  // namespace

void RegisterCommands(Session& session)
{
    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    Register({.name = L"join", .help = L"add player 2 (as Ctrl+F7); `wait-until p2.state == playing` confirms it",
              .set  = [&session](const Value&) {
                  if (session.HasPlayerTwo())
                  {
                      return Outcome{false, L"player 2 is already in"};
                  }
                  session.Join(players::kPlayerTwoSeat);  // logs its own outcome
                  return Outcome{};
              }});
    Register({.name = L"p2.in", .shape = Shape::OnOff, .help = L"player 2's local player exists",
              .get  = [&session] { return Value{session.HasPlayerTwo()}; }});
    Register({.name  = L"p2.state",
              .shape = Shape::Word,
              .words = {L"out", L"awaiting", L"in", L"playing"},
              .help  = L"player 2: out, awaiting a member, in without one, or playing their member",
              .get   = [&session] { return Value{PlayerTwoState(session)}; }});
    Register({.name = L"p2.log-status", .help = L"write player 2's status line to the log",
              .set  = [&session](const Value&) {
                  if (!session.HasPlayerTwo())
                  {
                      platform::Log(L"control: status: player 2 is not in");
                      return Outcome{true, L"player 2 is not in"};
                  }
                  session.LogStatus();
                  return Outcome{};
              }});
    Register({.name  = L"sticks",
              .shape = Shape::Word,
              .words = {L"per-pad", L"merged"},
              .help  = L"each pad drives only its own player, or the game's merge",
              .set   = [&session](const Value& v) {
                  const bool on = std::get<std::wstring>(v) == L"per-pad";
                  if (!pad_sticks::Installed())
                  {
                      return Outcome{false, L"the per-pad sticks stage is not installed"};
                  }
                  if (pad_sticks::SwitchedOn() != on)
                  {
                      session.TogglePadSticks();  // logs the new state
                  }
                  return pad_sticks::SwitchedOn() == on ? Outcome{} : Outcome{false, L"the switch did not take"};
              },
              .get = [] { return Value{std::wstring(pad_sticks::SwitchedOn() ? L"per-pad" : L"merged")}; }});
}

#endif

}  // namespace lss::player_two
