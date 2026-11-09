#include "traversal/traversal_watch.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/traversal.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/unreal_access.h"
#include "rides/chocobo_ride.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "session/tag_counts.h"
#include "traversal/traversal_camera.h"
#include "world/charapop_facts.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <tuple>

namespace lss::traversal::traversal_watch {

namespace {

// 1.004 addresses (1.005 through replaced-traversal.json and replaced-chocobo-traversal.json). Every slot and entry was
// read from both exes (tools/version_match/_traversal_check.py).

// End2FieldActionActor OnTriggerEnter / OnTriggerLeave (facts/traversal.h; RE-p2-traversal.md §1.3): the first
// instruction, `mov [rsp+10h], rbx`, is 5 bytes.
constexpr std::size_t    kFirstInstruction = 5;
using TriggerFn = std::uintptr_t (*)(void* actor, void* key, void* other, void* fourth);

// EndCharacter__Tick_Vt308's call of EndCharacter_FieldActionActorNotifyPass_Vt748(character, dt) (E8 at +0x5 of its
// block in both builds). The pass walks the character's own controller's list (+0x618). RE-p2-traversal.md §1.3.
constexpr std::uintptr_t kNotifyCall    = 0x14125B205;
constexpr std::uint8_t   kNotifyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                           0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr platform::FunctionFact kNotifyPass{L"EndCharacter_FieldActionActorNotifyPass_Vt748", 0x14125B3B0, kNotifyBytes};
using NotifyFn = void (*)(void* character, float dt);

// The field tick's trigger pass (FieldTick_StreamingThenLocationUpdate 0x140A55FCC; RE-p2-trigger-overlap.md §2.2):
// the pending key PlayerTrigger is resolved to the PRIMARY controller's trigger (`call FWeakObjectPtr::Get(PM+0x140)`,
// E8 inside one block in both builds). The tracker's dispatch call that follows belongs to world/seat_triggers,
// which reports each pass here (NoteDispatch).
constexpr std::uintptr_t kTriggerResolveCall = 0x140A56428;
using ResolveFn = void* (*)(const void* weak);

// The ladder's per-frame offer: Ladder_Vt748_NotifyNear_AutoEngage(ladder, context), context +0x8 the character
// (1.005 slot +0x750). It requests state 8 on that character when the approach and distance checks pass (§4.1).
constexpr std::uint8_t kLadderNotifyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                               0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24};
constexpr platform::SlotFact kLadderNotifySlot{L"the ladder's notify slot (+0x748)", 0x145940E90,
                                               {L"Ladder_Vt748_NotifyNear_AutoEngage", 0x141643238, kLadderNotifyBytes}};
using LadderNotifyFn = std::uintptr_t (*)(void* ladder, void* context);

// The ladder executer (state 8's per-class executer; vtable 0x1458125C0): Start +0x10, End +0x18, UpdateInput +0x30
// (reached from move-state slot 16; it polls the climb move, which reads the stick). Executer +0x10: the character
// (FWeakObjectPtr). §4.2.
constexpr std::uint8_t kExecStartBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                            0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0xB8, 0xC0};
constexpr std::uint8_t kExecEndBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                            0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr std::uint8_t kExecInputBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                            0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr platform::SlotFact kExecStartSlot{L"the ladder executer's Start slot (+0x10)", 0x1458125D0,
                                            {L"LadderExecuter_Start_Vt10", 0x14132DA38, kExecStartBytes}};
constexpr platform::SlotFact kExecEndSlot{L"the ladder executer's End slot (+0x18)", 0x1458125D8,
                                          {L"LadderExecuter_End_Vt18", 0x14132D860, kExecEndBytes}};
constexpr platform::SlotFact kExecInputSlot{L"the ladder executer's UpdateInput slot (+0x30)", 0x1458125F0,
                                            {L"LadderExecuter_UpdateInput_Vt30", 0x14132F4A4, kExecInputBytes}};
using ExecFn      = std::uintptr_t (*)(void* executer);
using ExecInputFn = std::uintptr_t (*)(void* executer, float dt);
constexpr std::uint32_t kExecCharacter = 0x10;

// The chocobo's glide (state 26) and water-jet (state 27) control states: enter +0x18 (state, param), exit +0x20
// (state). State +0x8: the chocobo (FWeakObjectPtr). RE-p2-chocobo-traversal.md §3, §4.
constexpr std::uint8_t kGlideEnterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                             0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr std::uint8_t kGlideExitBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41};
constexpr std::uint8_t kJetEnterBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x18, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8D};
constexpr std::uint8_t kJetExitBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8};
constexpr platform::SlotFact kGlideEnterSlot{L"the chocobo glide state's enter slot (+0x18)", 0x145795408,
                                             {L"CharCtrlStateChocoboGlide_OnEnter_Vt18", 0x143427234, kGlideEnterBytes}};
constexpr platform::SlotFact kGlideExitSlot{L"the chocobo glide state's exit slot (+0x20)", 0x145795410,
                                            {L"CharCtrlStateChocoboGlide_OnExit_Vt20", 0x14342810C, kGlideExitBytes}};
constexpr platform::SlotFact kJetEnterSlot{L"the chocobo jet state's enter slot (+0x18)", 0x1457C5958,
                                           {L"CharCtrlStateChocoboJet_OnEnter_Vt18", 0x143432B04, kJetEnterBytes}};
constexpr platform::SlotFact kJetExitSlot{L"the chocobo jet state's exit slot (+0x20)", 0x1457C5960,
                                          {L"CharCtrlStateChocoboJet_OnExit_Vt20", 0x143433BE4, kJetExitBytes}};
using StateEnterFn = std::uintptr_t (*)(void* state, void* param);
using StateExitFn  = std::uintptr_t (*)(void* state);
constexpr std::uint32_t kStateCharacter = 0x8;


constexpr std::uint32_t kControllerActorList = 0x618;  // TArray<FWeakObjectPtr>: data, then the count at +0x8
constexpr std::uint32_t kActorEnteredFlag    = 0x2A1;  // u8, one per actor (not per player)
constexpr std::uint32_t kContextCharacter    = 0x8;
constexpr std::size_t   kMaxList             = 32;    // a longer list is logged with its count and the first 32

struct List
{
    std::int32_t                          count = 0;
    std::array<const void*, kMaxList>     actors{};

    bool operator==(const List&) const = default;

    bool Contains(const void* actor) const noexcept
    {
        const auto shown = static_cast<std::size_t>(std::clamp<std::int32_t>(count, 0, kMaxList));
        return actor != nullptr && std::find(actors.begin(), actors.begin() + shown, actor) != actors.begin() + shown;
    }
};

struct Climb
{
    const void* executer = nullptr;
    unsigned    updates = 0, inScope = 0;
};

// A controller's trigger actor, as the router needs it: Tags[0] must be the trigger tag, and its capsule must overlap
// the field-action actor (seen live: a seat's member at a climbing wall reached no router call; player 1's did).
constexpr std::uint32_t kActorRoot          = 0x138;  // an actor's root component
constexpr std::uint32_t kComponentParent    = 0xC0;   // USceneComponent::AttachParent
constexpr std::uint32_t kComponentOwner     = 0xA0;   // a component's owning actor

struct TriggerState
{
    const void*   trigger = nullptr;
    std::int32_t  tags = 0;
    bool          firstIsTriggerTag = false;
    const void*   parentOwner = nullptr;  // the actor its root is attached to
    int           reach = -1;             // its distance from the controller's pawn: 0 <1 m, 1 <3 m, 2 <10 m, 3 farther

    bool operator==(const TriggerState&) const = default;
};

// The trigger's own overlap tracker (AEndPlayerTrigger+0x230; RE-p2-trigger-overlap.md §2.3).
constexpr std::uint32_t kTriggerTracker      = 0x230;
constexpr std::uint32_t kTriggerCapsule      = 0x228;
constexpr std::uint32_t kTrackerPrevKeys     = 0x118;  // i32: keys of the previous pass
constexpr std::uint32_t kTrackerPrevActors   = 0x228;  // i32: field-action actors of the previous pass
constexpr std::uint32_t kTrackerOverlaps     = 0x338;  // i32: actors overlapped this pass
constexpr std::uint32_t kTrackerChannel      = 0x340;  // i32: trace channel (14 for players)
constexpr std::uint32_t kTrackerEnabled      = 0x375;  // u8: issued (the field tick skips it when 0)
constexpr std::uint32_t kTrackerFrame        = 0x378;  // u32: the trace handle's frame
constexpr std::uint32_t kCapsuleOverlapFlags = 0x229;  // bit 0: bGenerateOverlapEvents [ASSUMPTION]

struct TrackerState
{
    std::uint8_t enabled = 0;
    bool         issuing = false;  // its trace frame moved since the last engine tick
    std::int32_t prevKeys = 0, prevActors = 0, overlaps = 0, channel = 0;
    std::uint8_t capsuleOverlapBit = 0;

    bool operator==(const TrackerState&) const = default;
};

struct TrackerPasses
{
    unsigned     passes = 0;
    std::int32_t lastCount = -1;
};

// Set once at install.
platform::EntryRedirect            g_enter, g_leave;
platform::CallRedirect             g_notifyCall;
platform::VTableEntry              g_ladderNotify, g_execStart, g_execEnd, g_execInput;
platform::VTableEntry              g_glideEnter, g_glideExit, g_jetEnter, g_jetExit;
std::optional<platform::GameImage> g_image;
std::uintptr_t                     g_ladderVt = 0, g_doorVt = 0, g_climbVt = 0, g_jumpPadVt = 0;

// The records by player: 0 player 1's, then each seat's. One more slot counts every other controller or tracker.
constexpr std::size_t kPlayers = players::kMaxPlayers;
constexpr std::size_t kAnother = kPlayers;
// The characters followed (Tracked): each player's pawn, then each seat's member while its pawn is something else.
constexpr std::size_t kTracked = 2 * kPlayers;

// Game thread.
std::array<bool, kPlayers>                   g_listSeen{};
List                                         g_lists[kPlayers];
std::array<bool, kTracked>                   g_passSeen{};
std::array<std::tuple<const void*, const void*, std::int32_t>, kTracked> g_pass{};  // character, controller, list count
std::array<std::tuple<const void*, std::uint8_t>, kTracked>              g_offer{};  // ladder, its answer
std::array<bool, kTracked>                   g_offerSeen{};
std::array<Climb, 4>                         g_climbs{};
std::array<TriggerState, kPlayers>           g_triggers{};
std::array<TrackerState, kPlayers>           g_trackers{};
std::array<bool, kPlayers>                   g_trackerSeen{};
std::array<std::uint32_t, kPlayers>          g_trackerFrames{};
platform::CallRedirect                       g_resolveCall;
std::array<TrackerPasses, kPlayers + 1>      g_dispatches{};  // each player's tracker, then another
std::array<unsigned, kPlayers + 1>           g_resolves{};    // the key resolved to each player's controller, then another
platform::Every                              g_summary{std::chrono::seconds(10)};

/// Whose `controller` is: 0 player 1's, a seat's number, kAnother for any other (or none).
std::size_t PlayerOf(const void* controller) noexcept
{
    if (controller == nullptr)
    {
        return kAnother;
    }
    if (controller == players::generation::PlayerOneController())
    {
        return 0;
    }
    const int seat = players::generation::SeatOfController(controller);
    return players::SeatIndex(seat) ? static_cast<std::size_t>(seat) : kAnother;
}

/// The seat whose primary scope is open (its controller is the game's primary one now), or 0. Game thread.
int ScopedSeat() noexcept
{
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        if (const void* controller = players::generation::ControllerOf(seat);
            controller != nullptr && scopes::primary_controller::Is(controller))
        {
            return seat;
        }
    }
    return 0;
}

std::uintptr_t Live(const platform::GameImage& image, std::uintptr_t research) noexcept
{
    const auto at = image.Rebase(research);
    return at ? *at : 0;
}

std::uintptr_t Research(std::uintptr_t live) noexcept
{
    return g_image ? g_image->ResearchAddress(live) : live;
}

/// The characters this watch follows: p for player p's pawn (0 player 1's; a seat's is its member, or the chocobo
/// its player drives), kPlayers + s for seat s's member when the seat's pawn is something else. -1 for any other.
int Tracked(const void* character) noexcept
{
    if (character == nullptr)
    {
        return -1;
    }
    for (int p = 0; p < players::kMaxPlayers; ++p)
    {
        if (character == ReadPointer(players::generation::ControllerOf(p), facts::kControllerPawn))
        {
            return p;
        }
    }
    const int seat = players::generation::SeatOfMember(character);
    return players::SeatIndex(seat) ? static_cast<int>(kPlayers) + seat : -1;
}

std::wstring Whom(const void* actor)
{
    if (actor == nullptr)
    {
        return L"none";
    }
    if (const int rider = rides::chocobo_ride::SeatDriving(actor); rider >= 1)  // seated or getting off
    {
        return std::format(L"{}'s chocobo", players::SeatName(rider));
    }
    if (const int seat = players::generation::SeatOfMember(actor); players::SeatIndex(seat))
    {
        return std::format(L"{}'s member", players::SeatName(seat));
    }
    if (const int player = Tracked(actor); player >= 0 && player < players::kMaxPlayers)
    {
        return std::format(L"{}'s pawn", players::SeatName(player));
    }
    return std::format(L"another character {}", actor);
}

std::wstring ActorText(const void* actor)
{
    if (actor == nullptr)
    {
        return L"(gone)";
    }
    const auto vt = VTableOf(actor);
    const wchar_t* kind = vt == 0              ? L"?"
                          : vt == g_ladderVt   ? L"ladder"
                          : vt == g_doorVt     ? L"door"
                          : vt == g_climbVt    ? L"chocobo climb"
                          : vt == g_jumpPadVt  ? L"chocobo launch pad"
                                               : nullptr;
    return kind != nullptr ? std::format(L"{} ({})", players::NameOf(actor), kind)
                           : std::format(L"{} (vt 0x{:X})", players::NameOf(actor), Research(vt));
}

/// A controller's field-action list (+0x618), each weak entry resolved by the game. Game thread.
List ReadList(const void* controller) noexcept
{
    List list;
    if (controller == nullptr)
    {
        return list;
    }
    const auto* data = static_cast<const std::byte*>(ReadPointer(controller, kControllerActorList));
    list.count       = ReadField<std::int32_t>(controller, kControllerActorList + 8);
    const auto shown = static_cast<std::size_t>(std::clamp<std::int32_t>(list.count, 0, kMaxList));
    for (std::size_t i = 0; data != nullptr && i < shown; ++i)
    {
        list.actors[i] = scopes::leader_scope::ObjectAt(data + i * 8);
    }
    return list;
}

TriggerState ReadTrigger(const void* controller) noexcept
{
    TriggerState state;
    state.trigger = ReadPointer(controller, facts::kControllerTrigger);
    if (state.trigger == nullptr)
    {
        return state;
    }
    state.tags                    = ReadField<std::int32_t>(state.trigger, facts::kActorTagsCount);
    const auto first              = session::tag_counts::FirstTagOf(state.trigger);
    const auto tag                = session::tag_counts::TriggerTag();
    state.firstIsTriggerTag       = first && tag && *first == *tag;
    state.parentOwner = ReadPointer(ReadPointer(ReadPointer(state.trigger, kActorRoot), kComponentParent), kComponentOwner);
    float at[3]{}, pawnAt[3]{};
    auto* pawn = const_cast<void*>(ReadPointer(controller, facts::kControllerPawn));
    if (players::unreal::ActorLocation(state.trigger, at) && players::unreal::ActorLocation(pawn, pawnAt))
    {
        const float metres = world::charapop::Distance(at, pawnAt) / 100.0f;
        state.reach        = metres < 1.0f ? 0 : metres < 3.0f ? 1 : metres < 10.0f ? 2 : 3;
    }
    return state;
}

bool Watching() noexcept;

const void* TrackerOf(const void* controller) noexcept
{
    const auto* trigger = static_cast<const std::byte*>(ReadPointer(controller, facts::kControllerTrigger));
    return trigger != nullptr ? trigger + kTriggerTracker : nullptr;
}

TrackerState ReadTracker(int player, const void* controller) noexcept
{
    TrackerState state;
    const void*  tracker = TrackerOf(controller);
    if (tracker == nullptr)
    {
        return state;
    }
    const auto frame        = ReadField<std::uint32_t>(tracker, kTrackerFrame);
    state.issuing           = frame != g_trackerFrames[player];
    g_trackerFrames[player] = frame;
    state.enabled           = ReadField<std::uint8_t>(tracker, kTrackerEnabled);
    state.prevKeys          = ReadField<std::int32_t>(tracker, kTrackerPrevKeys);
    state.prevActors        = ReadField<std::int32_t>(tracker, kTrackerPrevActors);
    state.overlaps          = ReadField<std::int32_t>(tracker, kTrackerOverlaps);
    state.channel           = ReadField<std::int32_t>(tracker, kTrackerChannel);
    const void* capsule     = ReadPointer(ReadPointer(controller, facts::kControllerTrigger), kTriggerCapsule);
    state.capsuleOverlapBit = ReadField<std::uint8_t>(capsule, kCapsuleOverlapFlags) & 1;
    return state;
}

/// Whose trigger tracker `tracker` is: a player (0 player 1), or kAnother.
std::size_t WhoseTracker(const void* tracker) noexcept
{
    for (int p = 0; tracker != nullptr && p < players::kMaxPlayers; ++p)
    {
        if (tracker == TrackerOf(players::generation::ControllerOf(p)))
        {
            return static_cast<std::size_t>(p);
        }
    }
    return kAnother;
}

/// The field tick's resolve of the PlayerTrigger key (logging only: the game's answer is returned unchanged).
void* TriggerResolve(const void* weak) noexcept
{
    void* controller = reinterpret_cast<ResolveFn>(g_resolveCall.Original())(weak);
    if (Watching())
    {
        ++g_resolves[PlayerOf(controller)];
    }
    return controller;
}

/// The field tick's diff and dispatch of one tracker, counted (and its overlap count logged on change).
void CountDispatch(const void* tracker, std::int32_t count) noexcept
{
    if (!Watching())
    {
        return;
    }
    const std::size_t whose = WhoseTracker(tracker);
    auto&             seen  = g_dispatches[whose];
    ++seen.passes;
    if (count != seen.lastCount)
    {
        seen.lastCount = count;
        try
        {
            // Every other tracker shares the "another" count, so it changes on most calls: Trace, not Debug.
            if (whose == kAnother)
            {
                platform::LogTrace(L"traversal: the field tick dispatches another trigger tracker with {} overlap(s)", count);
            }
            else
            {
                platform::LogDebug(L"traversal: the field tick dispatches {}'s trigger tracker with {} overlap(s)",
                                   players::SeatName(static_cast<int>(whose)), count);
            }
        }
        catch (...)
        {
        }
    }
}

std::wstring ListText(const List& list)
{
    std::wstring text = std::format(L"{} actor{}", list.count, list.count == 1 ? L"" : L"s");
    const auto shown  = static_cast<std::size_t>(std::clamp<std::int32_t>(list.count, 0, kMaxList));
    for (std::size_t i = 0; i < shown; ++i)
    {
        text += (i == 0 ? L": " : L", ") + ActorText(list.actors[i]);
    }
    return text;
}

const wchar_t* ScopeText() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return L"off the game thread";
    }
    const int seat = ScopedSeat();
    return seat == 0 ? L"outside players 2 and 3's primary scopes"
                     : players::BySeat(seat, L"in player 2's primary scope", L"in player 3's primary scope");
}

/// Which player's camera manager GetEndPlayerCameraManager() would give now: the primary controller's. Game thread.
const wchar_t* PrimaryCameraText() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return L"? (off the game thread)";
    }
    const void* primary = scopes::primary_controller::Current();
    if (primary == players::generation::PlayerOneController())
    {
        return L"player 1's";
    }
    const int seat = players::generation::SeatOfController(primary);
    return players::SeatIndex(seat) ? players::BySeat(seat, L"player 2's", L"player 3's") : L"another controller's";
}

bool Watching() noexcept
{
    return players::generation::AnyActive() && players::generation::OnGameThread();
}

// --- trigger enter / leave ---

std::uintptr_t Trigger(bool enter, void* actor, void* key, void* other, void* fourth) noexcept
{
    const auto original = reinterpret_cast<TriggerFn>((enter ? g_enter : g_leave).Original());
    if (!Watching())
    {
        return original(actor, key, other, fourth);
    }
    // Whose trigger `other` is: a player's (0 player 1), or -1.
    const void* controllers[kPlayers]{};
    int         whose = -1;
    for (int p = 0; p < players::kMaxPlayers; ++p)
    {
        controllers[p] = p == 0 || players::generation::Active(p) ? players::generation::ControllerOf(p) : nullptr;
        if (other != nullptr && controllers[p] != nullptr && other == ReadPointer(controllers[p], facts::kControllerTrigger))
        {
            whose = p;
        }
    }
    if (whose < 0)
    {
        return original(actor, key, other, fourth);
    }
    const auto flagBefore = ReadField<std::uint8_t>(actor, kActorEnteredFlag);
    bool       before[kPlayers]{};
    for (std::size_t p = 0; p < kPlayers; ++p)
    {
        before[p] = controllers[p] != nullptr && ReadList(controllers[p]).Contains(actor);
    }
    const wchar_t* const scope  = ScopeText();
    const auto           result = original(actor, key, other, fourth);
    try
    {
        std::wstring lists;  // the actor on every player's list, before and after
        for (std::size_t p = 0; p < kPlayers; ++p)
        {
            if (controllers[p] != nullptr)
            {
                lists += std::format(L"{}on {}'s list {} -> {}", lists.empty() ? L"" : L", ",
                                     players::SeatName(static_cast<int>(p)), players::YesNo(before[p]),
                                     players::YesNo(ReadList(controllers[p]).Contains(actor)));
            }
        }
        platform::LogDebug(L"traversal: {}'s trigger {} {}: entered flag {} -> {}; {} ({})", players::SeatName(whose),
                           enter ? L"entered" : L"left", ActorText(actor), flagBefore,
                           ReadField<std::uint8_t>(actor, kActorEnteredFlag), lists, scope);
    }
    catch (...)
    {
    }
    return result;
}

std::uintptr_t TriggerEnter(void* actor, void* key, void* other, void* fourth) noexcept
{
    return Trigger(true, actor, key, other, fourth);
}

std::uintptr_t TriggerLeave(void* actor, void* key, void* other, void* fourth) noexcept
{
    return Trigger(false, actor, key, other, fourth);
}

// --- the notify pass ---

void NotifyPass(void* character, float dt) noexcept
{
    if (Watching())
    {
        const int slot = Tracked(character);
        if (slot >= 0)
        {
            const void* controller = ReadPointer(character, facts::kPawnController);
            const auto  now = std::make_tuple(static_cast<const void*>(character), controller,
                                              ReadField<std::int32_t>(controller, kControllerActorList + 8));
            if (!g_passSeen[slot] || now != g_pass[slot])
            {
                g_passSeen[slot] = true;
                g_pass[slot]     = now;
                try
                {
                    platform::LogDebug(L"traversal: {}'s notify pass walks {}'s list ({} actors)", Whom(character),
                                       players::Who(controller), std::get<2>(now));
                }
                catch (...)
                {
                }
            }
        }
    }
    reinterpret_cast<NotifyFn>(g_notifyCall.Original())(character, dt);
}

// --- ladders ---

std::uintptr_t LadderNotify(void* ladder, void* context) noexcept
{
    const auto result = reinterpret_cast<LadderNotifyFn>(g_ladderNotify.Original())(ladder, context);
    if (!Watching())
    {
        return result;
    }
    const void* character = ReadPointer(context, kContextCharacter);
    const int   slot      = Tracked(character);
    if (slot < 0)
    {
        return result;
    }
    const auto answer = static_cast<std::uint8_t>(result & 0xFF);
    const auto now    = std::make_tuple(static_cast<const void*>(ladder), answer);
    try
    {
        if (!g_offerSeen[slot] || now != g_offer[slot])
        {
            g_offerSeen[slot] = true;
            g_offer[slot]     = now;
            platform::LogDebug(L"traversal: {} offered to {}: answer {} ({})", ActorText(ladder), Whom(character), answer,
                               ScopeText());
        }
        platform::LogTrace(L"traversal: ladder {} offered to {}: answer {}", ladder, Whom(character), answer);
    }
    catch (...)
    {
    }
    return result;
}

Climb* ClimbOf(const void* executer) noexcept
{
    for (auto& climb : g_climbs)
    {
        if (climb.executer == executer)
        {
            return &climb;
        }
    }
    return nullptr;
}

std::wstring ExecuterCharacter(void* executer)
{
    return players::generation::OnGameThread()
               ? Whom(scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(executer) + kExecCharacter))
               : std::wstring(L"? (off the game thread)");
}

std::uintptr_t ExecStart(void* executer) noexcept
{
    // The game's Start, through the seats' ladder camera (which passes every other climb straight through).
    const auto result = traversal_camera::LadderStart(reinterpret_cast<ExecFn>(g_execStart.Original()), executer);
    if (players::generation::AnyActive())
    {
        if (players::generation::OnGameThread())
        {
            // A record left by an executer whose End was not seen (the memory reused) would be counted as this one.
            Climb* free = ClimbOf(executer);
            free        = free != nullptr ? free : ClimbOf(nullptr);
            if (free != nullptr)
            {
                *free = Climb{executer};
            }
        }
        try
        {
            platform::LogDebug(L"traversal: a ladder climb starts for {} (executer {}, start answer {}; {})",
                               ExecuterCharacter(executer), executer, result & 0xFF, ScopeText());
        }
        catch (...)
        {
        }
    }
    return result;
}

std::uintptr_t ExecEnd(void* executer) noexcept
{
    Climb climb;
    if (players::generation::OnGameThread())
    {
        if (Climb* known = ClimbOf(executer))  // freed whether or not a seat's player is still in
        {
            climb  = *known;
            *known = Climb{};
        }
    }
    if (players::generation::AnyActive())
    {
        try
        {
            platform::LogDebug(L"traversal: a ladder climb ends for {} (executer {}; {}); {} input updates, {} of them in "
                               L"player 2's or 3's primary scope",
                               ExecuterCharacter(executer), executer, ScopeText(), climb.updates, climb.inScope);
        }
        catch (...)
        {
        }
    }
    return traversal_camera::LadderEnd(reinterpret_cast<ExecFn>(g_execEnd.Original()), executer);
}

std::uintptr_t ExecInput(void* executer, float dt) noexcept
{
    if (Watching())
    {
        if (Climb* climb = ClimbOf(executer))
        {
            const bool scoped = ScopedSeat() != 0;
            if (climb->updates++ == 0)
            {
                try
                {
                    platform::LogDebug(L"traversal: the climb's first input update for {} ran {}", ExecuterCharacter(executer),
                                       ScopeText());
                }
                catch (...)
                {
                }
            }
            climb->inScope += scoped ? 1 : 0;
        }
    }
    return traversal_camera::LadderInput(reinterpret_cast<ExecInputFn>(g_execInput.Original()), executer, dt);
}

// --- the chocobo's glide and jet states ---

void LogState(const wchar_t* state, const wchar_t* step, void* object) noexcept
{
    if (!players::generation::AnyActive())
    {
        return;
    }
    try
    {
        const std::wstring chocobo =
            players::generation::OnGameThread()
                ? Whom(scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(object) + kStateCharacter))
                : std::wstring(L"? (off the game thread)");
        platform::LogDebug(L"traversal: the {} state {} for {} ({}; the primary camera is {})", state, step, chocobo,
                           ScopeText(), PrimaryCameraText());
    }
    catch (...)
    {
    }
}

std::uintptr_t GlideEnter(void* state, void* param) noexcept
{
    LogState(L"glide", L"enters", state);
    return reinterpret_cast<StateEnterFn>(g_glideEnter.Original())(state, param);
}

std::uintptr_t GlideExit(void* state) noexcept
{
    LogState(L"glide", L"exits", state);
    return reinterpret_cast<StateExitFn>(g_glideExit.Original())(state);
}

std::uintptr_t JetEnter(void* state, void* param) noexcept
{
    LogState(L"water-jet", L"enters", state);
    return reinterpret_cast<StateEnterFn>(g_jetEnter.Original())(state, param);
}

std::uintptr_t JetExit(void* state) noexcept
{
    LogState(L"water-jet", L"exits", state);
    return reinterpret_cast<StateExitFn>(g_jetExit.Original())(state);
}

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, const platform::FunctionFact& fact,
           std::uintptr_t replacement) noexcept
{
    const auto at = platform::FindFunction(image, fact);
    return at && redirect.Install(*at, image.EntryInstruction(fact.address, fact.entryBytes.first(kFirstInstruction)),
                                  replacement);
}

bool Slot(const platform::GameImage& image, const platform::SlotFact& fact, std::uintptr_t replacement,
          platform::VTableEntry& entry) noexcept
{
    return platform::ReplaceSlot(image, fact, replacement, entry);
}

const wchar_t* Outcome(bool done) noexcept
{
    return done ? L"logged" : L"NOT logged";
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image    = image;
    g_ladderVt  = Live(image, facts::kLadderVTable);
    g_doorVt    = Live(image, facts::kDoorVTable);
    g_climbVt   = Live(image, facts::kChocoboClimbVTable);
    g_jumpPadVt = Live(image, facts::kChocoboJumpPadVTable);

    const bool enter = Entry(image, g_enter, facts::kFieldActionTriggerEnter,
                             reinterpret_cast<std::uintptr_t>(&TriggerEnter));
    const bool leave = Entry(image, g_leave, facts::kFieldActionTriggerLeave,
                             reinterpret_cast<std::uintptr_t>(&TriggerLeave));
    const auto site  = image.Rebase(kNotifyCall, 5);
    const auto pass  = platform::FindFunction(image, kNotifyPass);
    const bool notify = site && pass && g_notifyCall.Install(*site, *pass, reinterpret_cast<std::uintptr_t>(&NotifyPass));
    const bool ladder = Slot(image, kLadderNotifySlot, reinterpret_cast<std::uintptr_t>(&LadderNotify), g_ladderNotify);
    const bool climb  = Slot(image, kExecStartSlot, reinterpret_cast<std::uintptr_t>(&ExecStart), g_execStart) &
                       Slot(image, kExecEndSlot, reinterpret_cast<std::uintptr_t>(&ExecEnd), g_execEnd) &
                       Slot(image, kExecInputSlot, reinterpret_cast<std::uintptr_t>(&ExecInput), g_execInput);
    const bool glide = Slot(image, kGlideEnterSlot, reinterpret_cast<std::uintptr_t>(&GlideEnter), g_glideEnter) &
                       Slot(image, kGlideExitSlot, reinterpret_cast<std::uintptr_t>(&GlideExit), g_glideExit);
    const bool jet   = Slot(image, kJetEnterSlot, reinterpret_cast<std::uintptr_t>(&JetEnter), g_jetEnter) &
                     Slot(image, kJetExitSlot, reinterpret_cast<std::uintptr_t>(&JetExit), g_jetExit);
    const auto resolveSite  = image.Rebase(kTriggerResolveCall, 5);
    const auto weakGet      = platform::FindFunction(image, facts::kWeakObjectGet);
    const bool resolve      = resolveSite && weakGet &&
                         g_resolveCall.Install(*resolveSite, *weakGet, reinterpret_cast<std::uintptr_t>(&TriggerResolve));
    platform::Log(L"traversal watch (logging only, Debug): trigger enters {}, leaves {}; notify passes {}; ladder offers {}; "
                  L"ladder climbs {}; chocobo glide {}, water jet {}; the field tick's trigger key {} (its tracker passes come "
                  L"from the triggers module); "
                  L"every player's controller's list and trigger tracker on change; actor names: ladder {}, door {}, chocobo "
                  L"climb {}, launch pad {}",
                  Outcome(enter), Outcome(leave), Outcome(notify), Outcome(ladder), Outcome(climb), Outcome(glide),
                  Outcome(jet), Outcome(resolve), g_ladderVt != 0 ? L"yes" : L"no",
                  g_doorVt != 0 ? L"yes" : L"no", g_climbVt != 0 ? L"yes" : L"no", g_jumpPadVt != 0 ? L"yes" : L"no");
}

void Remove() noexcept
{
    const bool entries = g_enter.Restore() & g_leave.Restore() & g_notifyCall.Restore() & g_resolveCall.Restore();
    const bool slots   = g_ladderNotify.Restore() & g_execStart.Restore() & g_execEnd.Restore() & g_execInput.Restore() &
                       g_glideEnter.Restore() & g_glideExit.Restore() & g_jetEnter.Restore() & g_jetExit.Restore();
    if (!entries || !slots)
    {
        platform::Log(L"traversal watch: a hook was changed after us and left in place (it passes through)");
    }
}

void Tick() noexcept
{
    if (!Watching())
    {
        g_listSeen    = {};
        g_triggers    = {};
        g_trackerSeen = {};
        g_passSeen  = {};
        g_offerSeen = {};
        if (players::generation::OnGameThread())
        {
            g_climbs = {};  // every seat's player left: no climb's End will be counted against these records
        }
        return;
    }
    // Player 1's controller, and the controller of each seat whose player is in (a seat that is out starts afresh).
    const void* controllers[kPlayers]{};
    for (std::size_t p = 0; p < kPlayers; ++p)
    {
        const int player = static_cast<int>(p);
        if (p == 0 || players::generation::Active(player))
        {
            controllers[p] = players::generation::ControllerOf(player);
            continue;
        }
        g_listSeen[p]    = false;
        g_triggers[p]    = {};
        g_trackerSeen[p] = false;
    }
    for (std::size_t p = 0; p < kPlayers; ++p)
    {
        if (controllers[p] == nullptr)
        {
            continue;
        }
        const List now = ReadList(controllers[p]);
        if (g_listSeen[p] && now == g_lists[p])
        {
            continue;
        }
        try
        {
            platform::LogDebug(L"traversal: {}'s controller's list {}: {}", players::SeatName(static_cast<int>(p)),
                               g_listSeen[p] ? L"changed" : L"now", ListText(now));
        }
        catch (...)
        {
        }
        g_lists[p]    = now;
        g_listSeen[p] = true;
    }
    for (std::size_t p = 0; p < kPlayers; ++p)
    {
        if (controllers[p] == nullptr)
        {
            continue;
        }
        const TriggerState now = ReadTrigger(controllers[p]);
        if (now == g_triggers[p])
        {
            continue;
        }
        g_triggers[p] = now;
        try
        {
            static constexpr const wchar_t* kReach[] = {L"under 1 m", L"1-3 m", L"3-10 m", L"over 10 m"};
            platform::LogDebug(L"traversal: {}'s trigger {}: {} tag(s), the first {}the trigger tag; attached to {}; "
                               L"{} from {}",
                               players::SeatName(static_cast<int>(p)), now.trigger, now.tags,
                               now.firstIsTriggerTag ? L"" : L"NOT ",
                               now.parentOwner != nullptr ? Whom(now.parentOwner) + L" (" + players::NameOf(now.parentOwner) + L")"
                                                          : std::wstring(L"nothing"),
                               now.reach >= 0 ? kReach[now.reach] : L"unknown distance",
                               Whom(ReadPointer(controllers[p], facts::kControllerPawn)));
        }
        catch (...)
        {
        }
    }
    for (std::size_t p = 0; p < kPlayers; ++p)
    {
        if (controllers[p] == nullptr)
        {
            continue;
        }
        const TrackerState now = ReadTracker(static_cast<int>(p), controllers[p]);
        if (g_trackerSeen[p] && now == g_trackers[p])
        {
            continue;
        }
        g_trackerSeen[p] = true;
        g_trackers[p]    = now;
        try
        {
            platform::LogDebug(L"traversal: {}'s trigger tracker: enabled {}, issuing {}; overlaps {} (last pass: {} "
                               L"key(s), {} field-action actor(s)); channel {}; capsule overlap events bit {}",
                               players::SeatName(static_cast<int>(p)), now.enabled, players::YesNo(now.issuing), now.overlaps,
                               now.prevKeys, now.prevActors, now.channel, now.capsuleOverlapBit);
        }
        catch (...)
        {
        }
    }
    const auto clock = std::chrono::steady_clock::now();
    const bool first = g_summary.Never();  // the first period counts, the line comes with the second
    if (g_summary.Due(clock))
    {
        if (!first)
        {
            try
            {
                std::wstring resolved, dispatched;  // by player, then every other controller's and tracker's
                for (std::size_t p = 0; p < kPlayers; ++p)
                {
                    const wchar_t* who = players::SeatName(static_cast<int>(p));
                    resolved += std::format(L"{}'s controller {}, ", who, g_resolves[p]);
                    dispatched += std::format(L"{}'s tracker {}, ", who, g_dispatches[p].passes);
                }
                platform::LogDebug(L"traversal (10 s): the field tick resolved the PlayerTrigger key (times): {}another {}; "
                                   L"it dispatched (times): {}another {}",
                                   resolved, g_resolves[kAnother], dispatched, g_dispatches[kAnother].passes);
            }
            catch (...)
            {
            }
        }
        g_resolves = {};
        for (auto& d : g_dispatches)
        {
            d.passes = 0;
        }
    }
}

void NoteDispatch(const void* tracker, std::int32_t overlaps) noexcept
{
    CountDispatch(tracker, overlaps);
}

}  // namespace lss::traversal::traversal_watch
