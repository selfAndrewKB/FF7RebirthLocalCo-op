// Session: a seat's party member, the leave, and the status line; the session's seats and shared tick.

#include "session/session.h"

#include "battle/battle_camera.h"
#include "battle/command_menu.h"
#include "battle/command_menu_view.h"
#include "battle/command_rules.h"
#include "battle/menu_focus.h"
#include "camera/camera_override.h"
#include "camera/field_camera.h"
#include "camera/view_target.h"
#include "facts/controller.h"
#include "input/member_movement.h"
#include "input/move_census.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "platform/frame_watch.h"
#include "platform/game_thread_sites.h"
#include "platform/log.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "rides/own_ride.h"
#include "rides/shared_ride.h"
#include "scopes/leader_answer.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/scope_leaks.h"
#include "session/field_exit.h"
#include "session/handover_watch.h"
#include "session/leave_watch.h"
#include "session/leftover_actors.h"
#include "session/own_controller.h"
#include "session/seat_out.h"
#include "session/session_rules.h"
#include "session/tag_counts.h"
#include "views/whole_screen.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

namespace lss::session {

namespace {

using platform::Log;

/// The seat's slot of the party order after the field leader's, as the log names it (players/seats.h).
const wchar_t* SlotWord(int seat) noexcept
{
    return seat == 2 ? L"3rd" : L"2nd";
}

/// Ticks player 1's controller is watched after a join (it has been seen to name no pawn for one).
constexpr std::uint32_t kPlayerOneWatchTicks = 3;

/// Ticks to wait for the switch to hand the member over before giving it back.
constexpr std::uint32_t kTakeTicks = 120;

/// How often, in ticks, the seat's member is checked for its battle controller component: character
/// state that no party funnel reports (RE-p2-session-levers.md §3.2).
constexpr std::uint32_t kControllerCheckTicks = 30;

/// The game's party funnels (session/party_events.h) drive the party checks. These slow checks
/// are the backstop for a party change that passes neither funnel (story override party data); a
/// change found by them is logged as such. Without the funnels, the checks keep the earlier rate.
constexpr std::uint32_t kBackstopTicks        = 600;
constexpr std::uint32_t kBackstopTicksNoEvents = 30;

/// After a game party change while the seat has no character: ticks during which the seat looks for
/// a free member every tick (the game spawns a member that joins on a later tick than its status).
constexpr std::uint32_t kRetakeLookTicks = 600;

/// Lines logged for the game's party changes heard (a yield or a retake is always logged).
constexpr std::uint32_t kEventLines = 200;

/// The seat in, waiting on its slot's member (its AI held by a story script, or not free yet):
/// ticks between checks. A script lets go without an event; the check is a few reads.
constexpr std::uint32_t kSlotWaitTicks = 10;

/// The slot wait on one monitor: how long it lasts before the player leaves to wait outside the game
/// (session_rules::WaitOutside). The party swaps at a scene's end resolve within about a second.
constexpr std::chrono::milliseconds kWaitOutsideGrace{1500};

/// Why the game holds the player's member, for the wait-outside line.
const wchar_t* WaitWords(party_member::SlotWait wait) noexcept
{
    switch (wait)
    {
        case party_member::SlotWait::AiNotReady: return L"a story script, or not following the party";
        case party_member::SlotWait::LinkPartner: return L"a synergy partner the game has not given back yet";
        case party_member::SlotWait::NotEligible: return L"not free by the game's own member test yet";
        case party_member::SlotWait::Unread: return L"the party could not be read";
        case party_member::SlotWait::None:
        case party_member::SlotWait::NoSlot: break;
    }
    return L"the slot's member is not free";
}

/// While the seat plays a member: ticks between checks of its AI (a script that needs it gets it back,
/// session/field_hold.h) and of the seat's slot (a reorder no party apply reported).
constexpr std::uint32_t kHeldAiTicks    = 6;
constexpr std::uint32_t kSlotCheckTicks = 60;
/// While riding along: ticks between the ride's lines (about a second).
constexpr std::uint32_t kRideLogTicks = 60;
/// Failed returns of a freed ride member (each kHeldAiTicks apart, about a second in all) before it is given back.
constexpr std::uint32_t kRideReturnTries = 10;

}  // namespace

void Session::TogglePadSticks() noexcept
{
    if (!installed_.padSticks)
    {
        Log(L"per-pad sticks unavailable (its stage is not installed)");
        return;
    }
    if (input::pad_sticks::Toggle())
    {
        Log(L"per-pad sticks ON: each pad drives only its own player's camera and movement");
    }
    else
    {
        Log(L"per-pad sticks OFF: every pad drives every player's sticks (the game's own merge)");
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
    platform::LogDebug(L"tag counts {}: {}'s first tag {}, trigger tag {}", when, players::NameOf(member_), text(counts.member),
        text(counts.trigger));
}

void Seat::Tick(bool& commandMenusTicked) noexcept
{
    if (!In())
    {
        OutsideTick();  // the rejoin after a map load, or the wait for a free party member
        return;
    }
    if (!players_.StillListed())
    {
        Log(L"{} was removed by the game; this mod has let go of it", Name());
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
            // Before the member tick: the tick's scopes answer the party leader with the seat's member
            // (scopes/leader_scope.h), which CheckMemberPlayable's leader test must not see.
            RetakeWhenFree();
            RideAlongTick();
            if (In())
            {
                LeaveToWaitOutside();
            }
            if (!In())
            {
                break;  // left to wait for a free member
            }
            CheckMemberPlayable();
            if (!commandMenusTicked)
            {
                commandMenusTicked = true;  // once per tick, by the first seat that reaches here
                // Every playing seat's command controller, built and set up per world.
                battle::command_menu::Tick();
            }
            if (phase_ == Phase::Playing && installed_.memberMovement && players::generation::MemberOf(seat_) != nullptr)
            {
                input::member_movement::Tick(seat_, controller_, players::generation::MemberOf(seat_));
            }
            break;
        case Phase::Swapping: AwaitSwap(); break;
        case Phase::Out: break;
    }
    if (phase_ != Phase::Leaving && phase_ != Phase::Out && players::generation::Active(seat_))
    {
        WatchOwnCamera();
    }
}

void Seat::WatchOwnCamera() noexcept
{
    // Watch only (RE-p2-session-levers.md §2): the game writes the seat's view target once, at spawn
    // (InitializeFor; the join repeats that aim once), and no game code retargets a seat's
    // controller afterwards, so nothing re-aims it here (riding along re-aims it: RideAlongTick). A move off its
    // camera actor is logged, one line per move for the first few; later ones are only counted on the status line.
    constexpr std::uint32_t kLoggedMoves = 3;
    const bool              own          = !installed_.viewTarget || camera::view_target::OnOwnCamera(controller_);
    if (own || !viewOnOwnCamera_)
    {
        viewOnOwnCamera_ = own;
        return;
    }
    viewOnOwnCamera_ = false;
    ++viewTargetResets_;
    if (viewTargetResets_ <= kLoggedMoves)
    {
        Log(L"WARNING: {}'s camera manager now views {}, not its camera actor (the game moved it; not re-aimed)",
            Name(), players::NameOf(camera::view_target::Current(controller_)));
    }
}

void Seat::LogPlayerOneAfterJoin() noexcept
{
    platform::LogDebug(L"after the join, tick {}: player 1's pawn {} and leader {}", ticks_,
        players::NameOf(ReadPointer(playerOneController_, facts::kControllerPawn)),
        players::NameOf(ReadPointer(playerOneController_, facts::kControllerLeader)));
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
    if (!installed_.member || !players::generation::Active(seat_))
    {
        Log(L"{} has its own controller; no party member is taken ({})", Name(),
            !installed_.member                          ? L"the party-member stage is not installed"
            : players::generation::FaultReason(seat_) != nullptr ? players::generation::FaultReason(seat_)
                                                        : L"the player's stages were withdrawn");
        phase_ = Phase::Playing;
        return;
    }
    if (!party_member::GameReady())
    {
        if (!gateClosedLogged_)
        {
            gateClosedLogged_ = true;
            Log(L"{} waits for the game to be ready (a load) to take a party member", Name());
        }
        return;
    }
    if (sharedRide_)
    {
        // Player 1 rides a vehicle the whole party shares: the seat takes no member until it is over (RetakeWhenFree).
        member_ = nullptr;
        phase_  = Phase::Playing;
        return;
    }
    handover_watch::PartyOrder(players::BySeat(seat_, L"choosing player 2's member", L"choosing player 3's member"), seat_,
                               nullptr);
    const auto choice = party_member::ChooseSlot(seat_, playerOneController_);
    member_           = choice.member;
    if (choice.wait == party_member::SlotWait::NoSlot)
    {
        LeaveToAwait(L"the party has no member for the player any more");
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
    slotWaitSince_.reset();
    slotWaitReason_ = party_member::SlotWait::None;
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
    memberWeak_ = scopes::leader_scope::WeakOf(member_);  // watched from the take on
    Log(L"{} is taking {}", Name(), players::NameOf(member_));
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
        Log(L"{} took {} after {} tick(s); player 1's controller bytes +0x897/+0x8AB {:#04x}/{:#04x} -> "
            L"{:#04x}/{:#04x} ({})",
            Name(), players::NameOf(member_), ticks_, playerOneBytes_.at897, playerOneBytes_.at8AB, bytes.at897, bytes.at8AB,
            bytes == playerOneBytes_ ? L"unchanged" : L"CHANGED");
        LogTagCounts(L"after the take");
        players::generation::SetMember(seat_, member_);
        memberWeak_  = scopes::leader_scope::WeakOf(member_);
        retakeLooks_ = 0;  // a look window armed before the take would drain for nothing
        if (installed_.fieldSettings)
        {
            Log(L"{}'s Field camera brought to player 1's settings: {}", Name(),
                camera::field_camera::Describe(camera::field_camera::Seed(seat_)));
        }
        Log(L"{}'s Battle camera brought to player 1's state: {}", Name(),
            battle::battle_camera::Seed(seat_) ? L"matches" : L"does not match (or not installed)");
        phase_ = Phase::Playing;
        return;
    }
    if (outcome == TakeOutcome::Wait)
    {
        return;
    }
    Log(L"{} did not get {} within {} ticks (driven by {}, the player's pawn {}, leader {}); giving it back, the "
        L"player stays in on its own controller",
        Name(), players::NameOf(member_), kTakeTicks, players::NameOf(lease.memberEffective), players::NameOf(lease.seatPawn),
        players::NameOf(lease.seatLeader));
    players::generation::Fault(seat_, L"the party member was not handed over in time");
    party_member::GiveBack(seat_, controller_);  // member_ stays recorded: the leave waits for this release
    phase_ = Phase::Playing;
}

/// The seat's member destroyed under its player (a retry or checkpoint reload rebuilt the party without a
/// map load): nothing touches it from here on, and it is not given back (the give-back would reach
/// a destroyed object). True if that happened this tick.
bool Seat::MemberDestroyed() noexcept
{
    if (member_ == nullptr || memberWeak_ == 0 || scopes::leader_scope::StillNames(memberWeak_, member_))
    {
        return false;
    }
    Log(L"{}'s member was destroyed under them (the party was rebuilt without a field exit being seen); they let "
        L"go of it and join again",
        Name());
    EndRideAlong(L"the member was destroyed");
    players::generation::SetMember(seat_, nullptr);  // the seat's member movement and scopes stop at once
    // Riding, the seat's controller possessed the member's chocobo: its pawn field names that instead.
    const auto forgotten = rides::own_ride::Forget(seat_);
    camera::camera_override::MemberDestroyed(seat_);
    // The seat's controller still names the destroyed character: cleared, so neither the game's
    // give-back nor the controller's unpossess reads it (RE-p2-retry-reload.md §3.2, recipe 3).
    for (const std::uint32_t field : {facts::kControllerLeader, facts::kControllerPawn})
    {
        const void* named = ReadPointer(controller_, field);
        const bool vehicle = named == forgotten.chocobo || named == forgotten.scooter;
        if (named == member_ || (field == facts::kControllerPawn && named != nullptr && vehicle))
        {
            WriteField<void*>(controller_, field, nullptr);
            Log(L"{}'s controller field {:#x} named the destroyed character; cleared", Name(), field);
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

/// The seat in without a character (its give-back ran out of time, or the take failed) takes a member
/// as soon as one is free: looked for after each game party change, and by the slow backstop. When
/// the backstop finds none, the seat leaves and waits outside the game.
void Seat::RetakeWhenFree() noexcept
{
    const bool looking = retakeLooks_ > 0;
    if (looking && party_member::GameReady())
    {
        --retakeLooks_;  // the window runs only while the game is ready: no member is taken during a load
    }
    // MemberReleased with no member: the seat's controller holds no pawn or leader (a give-back that
    // never completed keeps it holding one). A wait on the seat's slot is checked every few ticks: the
    // game frees it without an event (its script lets go of the member's AI).
    const bool due  = looking || ticks_ % (waitingForSlot_ ? kSlotWaitTicks : BackstopTicks()) == 0;
    const auto hold = session_rules::HoldOf(member_, waitingForSlot_, sharedRide_, players::generation::MemberOf(seat_),
                                            players::generation::Active(seat_));
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
            LeaveToAwait(L"the party has no member for the player");
        }
        return;
    }
    retakeLooks_ = 0;
    Log(L"a party member is free for {} again ({}; {}); taking it", Name(), players::NameOf(free),
        waitingForSlot_          ? L"the slot's member is free now"
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
        Log(L"the shared ride is over: {} takes the {} slot's member again once it is free", Name(), SlotWord(seat_));
        retakeLooks_ = kRetakeLookTicks;
        return;
    }
    if (MemberHeld())
    {
        Yield(L"standing aside: player 1 rides a vehicle the whole party shares, with the party as in the one-player game");
    }
    else
    {
        Log(L"{} stands aside (no member held): player 1 rides a vehicle the whole party shares", Name());
    }
}

/// The seat gives its member back through the game's release and chooses again once it is released.
void Seat::Yield(const wchar_t* why) noexcept
{
    EndRideAlong(L"the member is given back");  // a party change while riding along: the give-back clears the leader too
    rides::own_ride::Release(seat_, L"the player gives the member back");  // while the seat's scopes still hold
    Log(L"{}'s {} is {}: giving it back to choose again among the deployed party", Name(), players::NameOf(member_), why);
    handover_watch::Snapshot(L"the give-back", member_);
    players::generation::SetMember(seat_, nullptr);  // the seat's member movement and scopes stop at once
    phase_ = Phase::Swapping;        // before the game's switch runs: a party event inside it sees the swap
    ticks_ = 0;
    if (!party_member::GiveBack(seat_, controller_))
    {
        Log(L"{}'s {} could not be given back (no trigger actor, or the switch faulted)", Name(), players::NameOf(member_));
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

/// The game is about to make `character` the leader of `controller` (session/party_events.h). If it
/// is the seat's member and player 1's controller, the seat lets go first, so the two controllers never
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
            Log(L"WARNING: party event: the game makes {} the leader of {}'s controller (was {}); a game leader "
                L"change ran inside player 2's or 3's scope. Not acted on",
                players::NameOf(character), Name(), players::NameOf(before));
            return;
        }
        EventLine(std::format(L"the game makes {} the leader of {} (was {})", players::NameOf(character),
                              event.controllerIsPlayerOne ? L"player 1's controller" : L"a controller that is not player 1's",
                              players::NameOf(before)));
    }
    switch (action)
    {
        case LeaderEventAction::Yield: Yield(L"about to become player 1's leader (the game's leader change)"); break;
        case LeaderEventAction::ArmLook: retakeLooks_ = kRetakeLookTicks; break;  // the member the game moved off may be free
        case LeaderEventAction::Warn:
        case LeaderEventAction::None: break;
    }
}

/// The game changed a party member's status (session/party_events.h): the seat's member is checked
/// against the deployed party at once, or, without one, the seat looks for a free member.
void Seat::OnStatusChanged(std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept
{
    if (!In() && !AwaitingMember())
    {
        return;
    }
    EventLine(std::format(L"the game changed party member type {}'s status {} -> {}", playerType, before, after));
    handover_watch::PartyOrder(L"after a party status change", seat_, MemberHeld() ? member_ : nullptr);
    // Not the slot: a squad change commits its statuses one by one and orders the list afterwards
    // (the chapter 7 log), so the order is final only at the party apply.
    CheckAfterPartyChange(false);
}

/// The game finished applying new party data (session/party_events.h): a squad or formation
/// change, a load, a script, an equipment or costume change. Every character it needed exists now.
void Seat::OnPartyApplied() noexcept
{
    if (!In() && !AwaitingMember())
    {
        return;
    }
    EventLine(L"the game applied the party data (its party stream finished)");
    handover_watch::PartyOrder(L"after the game's party apply", seat_, MemberHeld() ? member_ : nullptr);
    CheckAfterPartyChange(true);
}

/// After a game party change: the seat's member is checked against the deployed party at once (and,
/// with `followSlot`, against its slot), or, without one (or waiting outside the game), the seat
/// looks for a free member.
void Seat::CheckAfterPartyChange(bool followSlot) noexcept
{
    if (member_ == nullptr)
    {
        retakeLooks_ = kRetakeLookTicks;  // used by RetakeWhenFree, or the outside wait while the seat is out
        return;
    }
    if (!MemberHeld() || !players::generation::Active(seat_))
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

/// The seat plays its slot: when the slot names another member, the seat's is given back and the slot's
/// is taken (or waited on), even though the seat's is still in the party.
void Seat::FollowSlot(const wchar_t* when, bool log) noexcept
{
    if (phase_ != Phase::Playing || member_ == nullptr || players::generation::MemberOf(seat_) != member_ ||
        !party_member::GameReady())
    {
        return;
    }
    // Logged from the party-apply caller only: the party order after the game's apply is the one Info line a field
    // report needs for a hand-over question (the handover watch, which listed the whole order, is a dev tool now).
    // The slow slot check asks every kSlotCheckTicks and stays quiet (a line per poll spammed the log in play).
    const auto choice = party_member::ChooseSlot(seat_, playerOneController_, log);
    const auto held   = party_member::PlayerTypeOf(member_);
    if (choice.wait == party_member::SlotWait::NoSlot || !held || choice.type == *held)
    {
        return;  // no slot: the party checks give the seat's member up if it left
    }
    Log(L"the {} slot of the party order is now type {} ({}), not {}'s {} (type {})", SlotWord(seat_), choice.type,
        when, Name(), players::NameOf(member_), *held);
    Yield(L"no longer in the player's slot of the party order");
}

/// The seat stays in without a member while its slot's is not free; the reason is logged when it
/// changes.
void Seat::WaitForSlot(const party_member::SlotChoice& choice) noexcept
{
    if (!waitingForSlot_ || !slotWaitSince_)
    {
        slotWaitSince_ = std::chrono::steady_clock::now();
    }
    waitingForSlot_  = true;
    slotWaitReason_  = choice.wait;
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
        Log(L"{} waits for party member type {} (the {} slot, ordinal {}): a synergy partner the game has not "
            L"given back yet (its link: ability kind {} mode {}, partner state {}, started {} ended {}); taken once the "
            L"game ends its part of the link",
            Name(), choice.type, SlotWord(seat_), choice.ordinal, choice.link.kind, choice.link.mode,
            choice.link.state[choice.link.partner], choice.link.started[choice.link.partner],
            choice.link.ended[choice.link.partner]);
        return;
    }
    Log(L"{} waits for party member type {} (the {} slot, ordinal {}): {} (AI brain {} state {}); the game's "
        L"party order chooses the player's member, so a menu reorder ends the wait",
        Name(), choice.type, SlotWord(seat_), choice.ordinal,
        choice.wait == party_member::SlotWait::NotEligible ? Describe(choice.verdict)
        : choice.wait == party_member::SlotWait::Unread    ? L"the party could not be read"
                                                           : Describe(AssessAi(choice.ai)),
        choice.ai.brainIndex, choice.ai.state);
}

/// A script that needs the seat's member (a level's walk or lead, an event's walk, an animation: session/field_hold.h)
/// gets it: the game's own hand to its AI (party_member::HandToAi), the member kept as the seat's and the controller's
/// leader, so the player rides along with their own camera following it and their stick turning it. The member returns
/// to the player's controller when the hold ends (RideAlongTick). Without the hand (its call unavailable, the AI not
/// valid) the member is given back and the seat waits for it.
void Seat::CheckHeldAi() noexcept
{
    // While the seat's player rides their chocobo or a Wheelie the member is her AI's for the ride
    // (rides/chocobo_ride.h, rides/wheelie_ride.h).
    if (rideAlong_ || rides::own_ride::Riding(seat_) || ticks_ % kHeldAiTicks != 0)
    {
        return;
    }
    const AiSnapshot       ai   = party_member::ReadAi(member_);
    const field_hold::Hold hold = HoldOf(ai);
    if (hold == field_hold::Hold::Free)
    {
        return;
    }
    const wchar_t* what = hold == field_hold::Hold::Arrival   ? L"a walk or lead of the level's"
                          : hold == field_hold::Hold::Script  ? L"a walk of an event's"
                                                              : L"a scripted animation";
    // The seat's per-tick modules stop on the generation's member at once; the camera's update keeps running as the
    // player's through the ride member (camera/field_camera ActsFor).
    players::generation::SetMember(seat_, nullptr);
    players::generation::SetRideMember(seat_, member_);
    if (!party_member::HandToAi(member_))
    {
        players::generation::SetRideMember(seat_, nullptr);
        players::generation::SetMember(seat_, member_);  // as it was; Yield takes it out again
        Yield(L"taken by a script (the game's hand to its AI is unavailable: given back, the player waits for it)");
        return;
    }
    rideAlong_       = true;
    rideMember_      = member_;
    rideTicks_       = 0;
    rideAims_        = 0;
    rideReturnFails_ = 0;
    if (installed_.viewTarget && !camera::view_target::OnOwnCamera(controller_))
    {
        camera::view_target::Aim(controller_);  // the hand's unpossess pointed the view at the controller: no frame of it
    }
    Log(L"{}'s {} is taken by {} (AI state {} on brain {}): handed to its AI with the game's own hand; {} rides along, "
        L"their camera on it, and plays it again when it follows the party",
        Name(), players::NameOf(member_), what, ai.state, ai.brainIndex, Name());
}

void Seat::EndRideAlong(const wchar_t* why) noexcept
{
    if (!rideAlong_)
    {
        return;
    }
    Log(L"{} no longer rides along with {} ({} ticks; the view put back on their camera {} time(s)): {}", Name(),
        players::NameOf(rideMember_), rideTicks_, rideAims_, why);
    rideAlong_       = false;
    rideMember_      = nullptr;
    rideTicks_       = 0;
    rideAims_        = 0;
    rideReturnFails_ = 0;
    players::generation::SetRideMember(seat_, nullptr);
}

/// While riding along: the member back to the player's controller once its hold has ended (its AI in the base brain's
/// FOLLOW again, the state the game hands players), the seat's view kept on its camera actor (the hand's unpossess
/// points the view at the controller), and a line once a second (the first at Info) with the camera's distance from
/// the member.
void Seat::RideAlongTick() noexcept
{
    if (!rideAlong_ || phase_ != Phase::Playing)
    {
        return;
    }
    if (member_ != rideMember_)
    {
        EndRideAlong(L"the seat's member changed");
        return;
    }
    ++rideTicks_;
    if (rideTicks_ % kHeldAiTicks == 0)
    {
        const AiSnapshot ai = party_member::ReadAi(member_);
        if (HoldOf(ai) == field_hold::Hold::Free && AssessAi(ai) == AiReadiness::Ready)
        {
            // The game's possession runs at once on the game thread: the lease shows it now, as after a take.
            const bool returned = party_member::ReturnTo(seat_, member_, controller_) &&
                                  Acquired(party_member::ReadLease(member_, controller_), member_, controller_);
            if (returned)
            {
                players::generation::SetMember(seat_, member_);  // the seat's modules act on it again
                EndRideAlong(L"its hold ended: back on the player's controller");
                return;
            }
            ++rideReturnFails_;
            if (rideReturnFails_ == 1 || rideReturnFails_ >= kRideReturnTries)
            {
                Log(L"{}'s {} is free again but did not return to {}'s controller ({} time(s))", Name(),
                    players::NameOf(member_), Name(), rideReturnFails_);
            }
            if (rideReturnFails_ >= kRideReturnTries)
            {
                Yield(L"free again after a script, but its return to the player's controller kept failing (given back)");
                return;
            }
        }
    }
    if (installed_.viewTarget && rideTicks_ % kHeldAiTicks == 0 && !camera::view_target::OnOwnCamera(controller_))
    {
        const void* viewed = camera::view_target::Current(controller_);
        const bool  aimed  = camera::view_target::Aim(controller_);
        ++rideAims_;
        if (rideAims_ <= 3)
        {
            Log(L"{}'s camera manager viewed {} while riding along: put back on its camera actor ({})", Name(),
                players::NameOf(viewed), aimed ? L"done" : L"NOT done");
        }
    }
    if (rideTicks_ % kRideLogTicks != 1)
    {
        return;
    }
    void* const camera = camera::view_target::Current(controller_);
    float       at[3]{}, member[3]{};
    const bool  read = camera != nullptr && players::unreal::ActorLocation(camera, at) &&
                      players::unreal::ActorLocation(member_, member);
    const float dx = at[0] - member[0], dy = at[1] - member[1], dz = at[2] - member[2];
    const auto  ai = party_member::ReadAi(member_);
    try
    {
        const std::wstring line = std::format(
            L"{} rides along: the view {} ({}); its camera {:.1f} m from {}{}; the member's AI state {} on brain {}", Name(),
            players::NameOf(camera), camera::view_target::OnOwnCamera(controller_) ? L"its camera actor" : L"NOT its camera actor",
            read ? std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f : -1.0f, players::NameOf(member_),
            read ? std::format(L" (camera at {:.0f}, {:.0f}, {:.0f})", at[0], at[1], at[2]) : std::wstring(L" (unread)"),
            ai.state, ai.brainIndex);
        if (rideTicks_ == 1)
        {
            Log(L"{}", line);
        }
        else
        {
            platform::LogDebug(L"{}", line);
        }
    }
    catch (...)
    {
    }
}

/// The one readiness gate of a join: the seat's camera manager can be started safely, the game has started,
/// and the field runs again (the entry steps done, the party rebuilt: a join before that would be undone by the
/// entry's own unpossess of every non-leader, RE-p2-retry-reload.md §2.3). Null when ready, else why not, as the
/// join's refusal line says it. The same gate for the join by the key, the rejoin after a map load and the wait
/// for a free member.
const wchar_t* Seat::NotReadyToJoin() const noexcept
{
    if (!installed_.ownController || !own_controller::Ready())
    {
        return installed_.ownController
                   ? L"the player's camera manager cannot be started safely yet (player 1's camera names are not "
                     L"set yet)"
                   : L"the player's camera manager cannot be started safely yet (its stage is not installed)";
    }
    if (installed_.member && !party_member::GameReady())
    {
        return L"the game has not started yet (the title, or early in a load)";
    }
    if (!field_exit::FieldReady())
    {
        return L"the field is still being entered (a join now would be undone by it)";
    }
    // The engine's own refusal (the pause-family menu): tested here so the join neither counts a try nor takes the
    // mirror views out before a create the engine returns nothing from.
    return views::LocalPlayerRefused();
}

bool Seat::ReadyToJoinOutside() const noexcept
{
    return NotReadyToJoin() == nullptr;
}

/// The seat is out and wants back in (session/outside_join.h says when): the normal join once the new
/// map has been in the field state with player 1's camera ready for a few seconds; or, while the seat
/// waits for a free party member, a free member is looked for every tick after a game party change and
/// by a slow check otherwise, and the seat joins when there is one.
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
        Log(L"the new map is ready: {} joins again (try {})", Name(), step.attempt);
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
    const bool riding = !free && rides::shared_ride::Rides(one.controller);  // the seat joins standing aside (Join)
    if (!free && !riding)
    {
        return;
    }
    Log(L"{} ({}): {} joins", free ? L"a party member is free for the player" : L"player 1 rides a shared vehicle",
        step.looking ? L"after the game's party change" : L"found by the slow check", Name());
    Join();  // ends the wait once the seat is in; a refused or failed join leaves it, and is counted
    LogGaveUp(outside_.AfterJoin(Mode::Await, In()).reason);
}

/// The line for a wait that ended without the seat's player (nothing for Reason::None).
void Seat::LogGaveUp(outside_join::Reason reason) noexcept
{
    using outside_join::Reason;
    switch (reason)
    {
        case Reason::None: break;
        case Reason::RejoinTimedOut:
            Log(L"{} did not rejoin: the field did not become ready within {} s; press Ctrl+F7 to join", Name(),
                outside_join::kRejoinWait.count());
            break;
        case Reason::RejoinRefused:
            Log(L"{} does not rejoin after the map load: an earlier join raised an exception; restart the game to join",
                Name());
            break;
        case Reason::RejoinTriesExhausted:
            Log(L"{} could not rejoin after the map load; press Ctrl+F7 to join", Name());
            break;
        case Reason::AwaitRefused:
            Log(L"{} stops waiting for a party member: an earlier join raised an exception; restart the game to join",
                Name());
            break;
        case Reason::AwaitTriesExhausted:
            Log(L"{} could not join in {} tries (see above); press Ctrl+F7 to try again", Name(), outside_join::kTries);
            break;
    }
}

/// A party member the seat could take, by the game's own member test (null if none). Quiet.
void* Seat::FreeMember(void* playerOneController) const noexcept
{
    return party_member::GameReady() ? party_member::ChooseSlot(seat_, playerOneController, false).member : nullptr;
}

/// The seat is out, and waits for a party member to be free before it joins.
void Seat::AwaitMember(const wchar_t* why, bool fieldReady) noexcept
{
    if (refuseJoins_)
    {
        Log(L"{}: {} stays out and does not wait (an earlier join raised an exception; restart the game to join)",
            why, Name());
        return;
    }
    if (AwaitingMember())
    {
        Log(L"{} still waits for a free party member ({})", Name(), why);
        return;
    }
    outside_.BeginAwait(fieldReady);  // replaces a map load's rejoin, and has no time limit
    retakeLooks_ = 0;
    Log(L"{}: {} stays out (the game runs without them) and joins once a party member is free "
        L"(Ctrl+F8 stops the wait)",
        why, Name());
}

/// The seat is in without a member and none is free: it leaves at once (nothing of the seat's is held
/// by the party), and waits outside the game for one.
void Seat::LeaveToAwait(const wchar_t* why, bool fieldReady) noexcept
{
    if (LeaveNow())
    {
        AwaitMember(why, fieldReady);
    }
}

std::chrono::milliseconds Seat::WaitedForSlot() const noexcept
{
    if (!waitingForSlot_ || !slotWaitSince_)
    {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *slotWaitSince_);
}

/// On one monitor a player whose member the game holds leaves the game once the wait has lasted the grace, with two
/// players in or three, and waits outside for the member by the same slot test (OutsideTick, the first test on the
/// next tick): their view leaves the screen and the others' fill it (the user's rule). With columns held their
/// column stays on player 1's picture (views/column_source.h). The grace counts only the time on one monitor and off a
/// shared ride: the timer restarts while either holds.
void Seat::LeaveToWaitOutside() noexcept
{
    if (phase_ != Phase::Playing)
    {
        return;  // the retake just found the member free (SettlingPlayerOne): the wait is over
    }
    if (waitingForSlot_ && (installed_.columnsHeld || sharedRide_))
    {
        slotWaitSince_ = std::chrono::steady_clock::now();
        return;
    }
    const session_rules::WaitOutsideFacts facts{
        .hold        = session_rules::HoldOf(member_, waitingForSlot_, sharedRide_, players::generation::MemberOf(seat_),
                                             players::generation::Active(seat_)),
        .columnsHeld = installed_.columnsHeld,
        .waited      = WaitedForSlot(),
    };
    if (!session_rules::WaitOutside(facts, kWaitOutsideGrace))
    {
        return;
    }
    Log(L"the game has held {}'s party member for {:.1f} s on one monitor ({}): their view leaves the screen until the "
        L"member is free",
        Name(), static_cast<double>(facts.waited.count()) / 1000.0, WaitWords(slotWaitReason_));
    LeaveToAwait(L"the game holds the player's party member (one monitor: the view leaves the screen until it is free)",
                 true);
}

/// The slow backstop of the party checks (the game's party changes are heard as they happen), and the
/// battle controller check, which no game event reports.
void Seat::CheckMemberPlayable() noexcept
{
    const auto hold = session_rules::HoldOf(member_, waitingForSlot_, sharedRide_, players::generation::MemberOf(seat_),
                                            players::generation::Active(seat_));
    if (hold != session_rules::Hold::Member || !party_member::GameReady())
    {
        return;
    }
    const wchar_t* why = ticks_ % BackstopTicks() == 0 ? party_member::WhyNotInParty(member_) : nullptr;
    if (why != nullptr && installed_.partyEvents)
    {
        Log(L"the slow party check found {}'s {} {}: no game party change was heard for it", Name(), players::NameOf(member_),
            why);
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
            Log(L"{}'s {} is back with its AI after {} tick(s); choosing again", Name(), players::NameOf(member_), ticks_);
            member_ = nullptr;
            phase_  = Phase::SettlingPlayerOne;
            ticks_  = kPlayerOneWatchTicks;  // player 1 is settled already: choose on the next tick
            return;
        case SwapOutcome::PlayerOnes:
            Log(L"{}'s {} is player 1's after {} tick(s), and the player's controller holds nothing; choosing again",
                Name(), players::NameOf(member_), ticks_);
            member_ = nullptr;
            phase_  = Phase::SettlingPlayerOne;
            ticks_  = kPlayerOneWatchTicks;
            return;
        case SwapOutcome::TimedOut: break;
    }
    Log(L"{}'s {} did not return to its AI within {} ticks; the player stays without a character until their "
        L"controller holds none, then takes a free one (a leave still waits for the release)",
        Name(), players::NameOf(member_), kTakeTicks);
    // Not the seat's member any more: a leave waits on the seat's controller holding nothing
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
        Log(L"{} no longer waits for a free party member", Name());
    }
    if (!In())
    {
        return;
    }
    rides::own_ride::Release(seat_, L"the player leaves");  // before the withdraw: the swap back needs the seat's scopes
    seat_out::LetGo(seat_);
    players::generation::Withdraw(seat_);  // every wrapper passes through from here on
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
    // The seat's hold on the old world's objects goes before it does, even if the removal below fails.
    rides::own_ride::Release(seat_, L"a map load takes the player out");
    seat_out::LetGo(seat_);
    players::generation::Withdraw(seat_);
    if (HoldsMember())
    {
        // A map load cannot wait for the release; the old world is torn down with it.
        party_member::GiveBack(seat_, controller_);
        const auto lease = party_member::ReadLease(member_, controller_);
        Log(L"map load: {} given back without waiting (released already: {})", players::NameOf(member_),
            players::YesNo(Released(lease, member_)));
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
                Log(L"leave: {} could not be given back (no trigger actor, or the switch faulted)", players::NameOf(member_));
            }
            break;
        case LeaveAction::Teardown: Teardown(); break;
        case LeaveAction::ReportStuck:
            Log(L"leave stopped: {} did not return to its AI within {} ticks; {} stays in (Ctrl+F8 tries again)",
                players::NameOf(member_), LeaveSequence::kReleaseTicks, Name());
            break;
    }
}

void Seat::Teardown() noexcept
{
    if (member_ != nullptr)
    {
        const auto bytes = party_member::ReadPlayerOneBytes(playerOneController_);
        Log(L"leave: {} is back with its AI; player 1's controller bytes {:#04x}/{:#04x}", players::NameOf(member_),
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
        Log(L"leave: {} left; {} leftover actor(s) destroyed ({} failed); PlayerInput {}", Name(), result.destroyed,
            result.failed,
            actors.playerInput == nullptr ? L"none" : result.inputReleased ? L"marked pending kill" : L"NOT marked (rooted or moved)");
    }
    else
    {
        Log(L"leave: {} could NOT be removed; their actors are kept", Name());
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
    return In() && phase_ == Phase::Playing && players::generation::MemberOf(seat_) != nullptr;
}

players::SeatState Seat::State() const noexcept
{
    // A leave during a shared ride is a leave, not the ride's stand-aside.
    return players::StateOf({.in            = In(),
                             .playsMember   = PlaysMember(),
                             .handed        = phase_ != Phase::Leaving && (WaitsForMember() || StandsAside()),
                             .awaiting      = AwaitingMember(),
                             .rejoinPending = RejoinPending()});
}

void Seat::NoteState() noexcept
{
    const players::SeatState now = State();
    if (now != loggedState_)
    {
        Log(L"{} is {}{} (state {}, was {})", players::SeatName(seat_), players::StateMeaning(now),
            RidesAlong() ? L", riding along while a script has their member" : L"", players::StateName(now),
            players::StateName(loggedState_));
        loggedState_ = now;
    }
}

std::wstring Seat::MemberReading() const
{
    const int slot = seat_ + 1;  // the seat's slot of the party order (players/seats.h)
    if (PlaysMember())
    {
        return std::format(L"{} (slot {} of the party order)", players::NameOf(players::generation::MemberOf(seat_)), slot);
    }
    if (!In())
    {
        return AwaitingMember() ? L"none: out until a party member is free"
               : RejoinPending() ? L"none: joins again once the new map is ready"
                                 : L"none: out";
    }
    if (phase_ == Phase::Leaving)
    {
        return L"none: leaving";
    }
    if (StandsAside())
    {
        return L"none: stands aside while player 1 rides a vehicle the whole party shares";
    }
    if (RidesAlong())
    {
        return std::format(L"{} (slot {} of the party order), with its AI for a script: the player rides along", players::NameOf(member_),
                           slot);
    }
    if (WaitsForMember())
    {
        return std::format(L"none: waits for the member in slot {} of the party order (a story script holds it, or "
                           L"it is not free yet)",
                           slot);
    }
    if (!installed_.member || !players::generation::Active(seat_))
    {
        return L"none: takes no member this time (its stage is not installed, or a fault stopped it; see the log)";
    }
    switch (phase_)
    {
        case Phase::SettlingPlayerOne:
        case Phase::Taking:
            return member_ != nullptr ? std::format(L"taking {}", players::NameOf(member_)) : L"none yet: joining";
        case Phase::Swapping: return L"none: changing members";
        case Phase::Leaving: return L"none: leaving";
        case Phase::Playing:
        case Phase::Out: return L"none: takes one as soon as one is free";
    }
    return L"none";
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
        return lease.seatPawn == nullptr && lease.seatLeader == nullptr;
    }
    return Released(lease, member_);
}

void Seat::Forget() noexcept
{
    seat_out::LetGo(seat_);
    players::generation::End(seat_);  // the seat's player is removed: its controller is freed at the next collection
    scopes::primary_controller::Unbind(seat_);
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
    slotWaitSince_.reset();
    slotWaitReason_      = party_member::SlotWait::None;
    viewTargetResets_    = 0;
    viewOnOwnCamera_     = true;
    EndRideAlong(L"the player is out");
}

void Seat::LogStatus() noexcept
{
    if (!In())
    {
        return;
    }
    const auto     settings = camera::field_camera::SettingsCounters(seat_).Take();
    const auto     update   = camera::field_camera::UpdateCounters(seat_).Take();
    const auto     input    = input::pad_sticks::InputCounters().Take();
    const auto     move     = input::pad_sticks::MoveCounters().Take();
    const wchar_t* fault    = players::generation::FaultReason(seat_);
    Log(L"{}: member {}{}, generation {}{}; field settings mirrored {} passed {} mismatched {} (not mirrored under a "
        L"cutscene camera {}); camera update as primary {} passed {} refused {}; per-pad sticks {} (input acted {} "
        L"refused {}, movement acted {} refused {})",
        Name(), players::NameOf(players::generation::CameraMemberOf(seat_)),
        RidesAlong() ? L" (with its AI for a script: riding along)" : L"",
        players::generation::Active(seat_) ? L"active" : L"withdrawn",
        fault ? L" (fault: " + std::wstring(fault) + L")" : std::wstring(), settings.acted, settings.passed,
        camera::field_camera::TakeMismatches(seat_), camera::field_camera::TakeNotMirrored(seat_), update.acted, update.passed,
        update.refused, input::pad_sticks::Enabled() ? L"on" : L"off", input.acted, input.refused, move.acted, move.refused);
    const auto member = input::member_movement::Counters(seat_).Take();
    Log(L"  {}'s view: own camera {} (moved off it {} time(s) so far); member movement ticked {} skipped {} "
        L"refused {}, leader test answered yes {} time(s)",
        Name(), players::YesNo(camera::view_target::OnOwnCamera(controller_)), viewTargetResets_, member.acted, member.passed,
        member.refused, scopes::leader_answer::TakeAnswers());
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
    players::generation::SetGameThread();  // the engine-tick callback runs on the thread that ticks the world
    platform::game_thread_sites::Run();  // the 1.005 placements (platform/deferred_sites.h), outside the game's ticks
    battle::command_rules::Update();      // which members player 1 may not command (none with every seat out)
    battle::command_menu_view::KeepHidden();  // each seat's menu widgets stay out of every view
    if (AnyIn())
    {
        battle::menu_focus::Watch();  // player 1's UI focus, logged on change (read-only)
    }
    bool commandMenusTicked = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        At(seat)->Tick(commandMenusTicked);
        At(seat)->NoteState();  // after the tick: a join, leave or key between ticks is logged here too
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
    scopes::scope_leaks::LogStatus();
    input::pad_buttons::LogStatus();
    input::move_census::Log();  // dev tools
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

bool Session::RejoinPending(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr && s->RejoinPending();
}

players::SeatState Session::State(int seat) const noexcept
{
    const Seat* s = At(seat);
    return s != nullptr ? s->State() : players::SeatState::Out;
}

std::wstring Session::MemberReading(int seat) const
{
    const Seat* s = At(seat);
    return s != nullptr ? s->MemberReading() : std::wstring(L"none: that player is not built");
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
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (const void* one = At(seat)->PlayerOne())
        {
            return one;
        }
    }
    return nullptr;
}

#if LSS_DEV

namespace {

/// One seat's join and readings, named with `prefix` (`p2`, `p3`); `join` is the join command's name. Taken from
/// Remake's RegisterSeatCommands (mod.cpp); its leave stays the module table's (modules.h).
void RegisterSeatCommands(Session& session, int seat, const wchar_t* prefix, const wchar_t* join)
{
    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    const std::wstring name = players::SeatName(seat);
    Register({.name = join,
              .help = std::format(L"add {} (Ctrl+F7 adds the first player out); `wait-until {}.state == playing` "
                                  L"confirms it",
                                  name, prefix),
              .set  = [&session, seat, name](const Value&) {
                  if (session.In(seat))
                  {
                      return Outcome{false, name + L" is already in"};
                  }
                  session.Join(seat);  // logs its own outcome
                  return Outcome{};
              }});
    Register({.name = std::format(L"{}.in", prefix), .shape = Shape::OnOff,
              .help = std::format(L"{}'s local player exists", name),
              .get  = [&session, seat] { return Value{session.In(seat)}; }});
    Register({.name  = std::format(L"{}.state", prefix),
              .shape = Shape::Word,
              .words = {L"out", L"awaiting", L"rejoining", L"in", L"playing", L"handed"},
              .help  = std::format(L"{}: out, awaiting (out until a party member is free), rejoining (taken out by a "
                                   L"map load, joins again once the new map is ready), in (without a member of its "
                                   L"own: joining, changing members, leaving, or none to be taken; `{}.member` says "
                                   L"which), playing (plays its member), or handed (in without its member while the "
                                   L"game has it: a story script, a shared ride)",
                                   name, prefix),
              .get   = [&session, seat] { return Value{std::wstring(players::StateName(session.State(seat)))}; }});
    Register({.name = std::format(L"{}.member", prefix), .shape = Shape::Text,
              .help = std::format(L"the member {} plays (its name and slot of the party order), or why it has none",
                                  name),
              .get  = [&session, seat] { return Value{session.MemberReading(seat)}; }});
    Register({.name = std::format(L"{}.rejoin", prefix), .shape = Shape::OnOff,
              .help = std::format(L"{} was taken out by a map load and joins again once the new map is ready (the "
                                  L"leave cancels it)",
                                  name),
              .get  = [&session, seat] { return Value{session.RejoinPending(seat)}; }});
}

}  // namespace

void RegisterCommands(Session& session)
{
    using control::Outcome;
    using control::Register;
    using control::Shape;
    using control::Value;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        RegisterSeatCommands(session, seat, players::BySeat(seat, L"p2", L"p3"), players::BySeat(seat, L"join", L"p3.join"));
    }
    Register({.name = L"p2.log-status", .help = L"write every other player's status lines to the log",
              .set  = [&session](const Value&) {
                  if (!session.AnyIn())
                  {
                      platform::Log(L"control: status: no other player is in");
                      return Outcome{true, L"no other player is in"};
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
                  if (!input::pad_sticks::Installed())
                  {
                      return Outcome{false, L"the per-pad sticks stage is not installed"};
                  }
                  if (input::pad_sticks::SwitchedOn() != on)
                  {
                      session.TogglePadSticks();  // logs the new state
                  }
                  return input::pad_sticks::SwitchedOn() == on ? Outcome{} : Outcome{false, L"the switch did not take"};
              },
              .get = [] { return Value{std::wstring(input::pad_sticks::SwitchedOn() ? L"per-pad" : L"merged")}; }});
}

#endif

}  // namespace lss::session
