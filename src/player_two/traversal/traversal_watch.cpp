#include "player_two/traversal/traversal_watch.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/traversal.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/rides/chocobo_ride.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/session/tag_counts.h"
#include "player_two/traversal/traversal_camera.h"
#include "player_two/world/charapop_facts.h"

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

namespace lss::player_two::traversal_watch {

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
// E8 inside one block in both builds). The tracker's dispatch call that follows belongs to player_two/trigger_two,
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
// the field-action actor (live 2026-10-07: player 2's member at a climbing wall reached no router call; Cloud's did).
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

// Game thread.
bool                                         g_listSeen = false;
List                                         g_lists[2];
std::array<bool, 3>                          g_passSeen{};
std::array<std::tuple<const void*, const void*, std::int32_t>, 3> g_pass{};  // character, controller, list count
std::array<std::tuple<const void*, std::uint8_t>, 3>              g_offer{};  // ladder, its answer
std::array<bool, 3>                          g_offerSeen{};
std::array<Climb, 4>                         g_climbs{};
std::array<TriggerState, 2>                  g_triggers{};
std::array<TrackerState, 2>                  g_trackers{};
std::array<bool, 2>                          g_trackerSeen{};
std::array<std::uint32_t, 2>                 g_trackerFrames{};
platform::CallRedirect                       g_resolveCall;
std::array<TrackerPasses, 3>                 g_dispatches{};  // player 1's tracker, player 2's, another
std::array<unsigned, 3>                      g_resolves{};    // the key resolved to player 1's controller, 2's, another
platform::Every                              g_summary{std::chrono::seconds(10)};

std::uintptr_t Live(const platform::GameImage& image, std::uintptr_t research) noexcept
{
    const auto at = image.Rebase(research);
    return at ? *at : 0;
}

std::uintptr_t Research(std::uintptr_t live) noexcept
{
    return g_image ? g_image->ResearchAddress(live) : live;
}

/// The characters this watch follows: 0 player 1's pawn, 1 player 2's pawn (the member, or the chocobo they drive),
/// 2 player 2's member when player 2's pawn is something else. -1 for any other.
int Tracked(const void* character) noexcept
{
    if (character == nullptr)
    {
        return -1;
    }
    const void* onePawn = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    const void* twoPawn = ReadPointer(generation::PlayerTwoController(), facts::kControllerPawn);
    if (character == onePawn)
    {
        return 0;
    }
    if (character == twoPawn)
    {
        return 1;
    }
    return character == generation::Member() ? 2 : -1;
}

std::wstring Whom(const void* actor)
{
    if (actor == nullptr)
    {
        return L"none";
    }
    if (const int rider = chocobo_ride::SeatDriving(actor); rider >= 1)  // seated or getting off
    {
        return std::format(L"{}'s chocobo", players::SeatName(rider));
    }
    if (actor == generation::Member())
    {
        return L"player 2's member";
    }
    switch (Tracked(actor))
    {
    case 0: return L"player 1's pawn";
    case 1: return L"player 2's pawn";
    default: return std::format(L"another character {}", actor);
    }
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
    return kind != nullptr ? std::format(L"{} ({})", NameOf(actor), kind)
                           : std::format(L"{} (vt 0x{:X})", NameOf(actor), Research(vt));
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
        list.actors[i] = leader_scope::ObjectAt(data + i * 8);
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
    const auto first              = tag_counts::FirstTagOf(state.trigger);
    const auto tag                = tag_counts::TriggerTag();
    state.firstIsTriggerTag       = first && tag && *first == *tag;
    state.parentOwner = ReadPointer(ReadPointer(ReadPointer(state.trigger, kActorRoot), kComponentParent), kComponentOwner);
    float at[3]{}, pawnAt[3]{};
    auto* pawn = const_cast<void*>(ReadPointer(controller, facts::kControllerPawn));
    if (charapop::ActorLocation(const_cast<void*>(state.trigger), at) && charapop::ActorLocation(pawn, pawnAt))
    {
        const float metres = charapop::Distance(at, pawnAt) / 100.0f;
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

int WhoseTracker(const void* tracker) noexcept
{
    return tracker == nullptr                                          ? 2
           : tracker == TrackerOf(generation::PlayerOneController()) ? 0
           : tracker == TrackerOf(generation::PlayerTwoController()) ? 1
                                                                      : 2;
}

/// The field tick's resolve of the PlayerTrigger key (logging only: the game's answer is returned unchanged).
void* TriggerResolve(const void* weak) noexcept
{
    void* controller = reinterpret_cast<ResolveFn>(g_resolveCall.Original())(weak);
    if (Watching())
    {
        ++g_resolves[controller == generation::PlayerOneController()   ? 0
                     : controller == generation::PlayerTwoController() ? 1
                                                                       : 2];
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
    const int whose = WhoseTracker(tracker);
    auto&     seen  = g_dispatches[whose];
    ++seen.passes;
    if (count != seen.lastCount)
    {
        seen.lastCount = count;
        try
        {
            static constexpr const wchar_t* kWhose[] = {L"player 1's", L"player 2's", L"another"};
            // Every other tracker shares the "another" count, so it changes on most calls: Trace, not Debug.
            if (whose == 2)
            {
                platform::LogTrace(L"traversal: the field tick dispatches {} trigger tracker with {} overlap(s)",
                                   kWhose[whose], count);
            }
            else
            {
                platform::LogDebug(L"traversal: the field tick dispatches {} trigger tracker with {} overlap(s)",
                                   kWhose[whose], count);
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
    if (!generation::OnGameThread())
    {
        return L"off the game thread";
    }
    return primary_controller::Is(generation::PlayerTwoController()) ? L"in player 2's primary scope"
                                                                      : L"outside player 2's scope";
}

/// Which player's camera manager GetEndPlayerCameraManager() would give now: the primary controller's. Game thread.
const wchar_t* PrimaryCameraText() noexcept
{
    if (!generation::OnGameThread())
    {
        return L"? (off the game thread)";
    }
    const void* primary = primary_controller::Current();
    if (primary == generation::PlayerOneController())
    {
        return L"player 1's";
    }
    return primary == generation::PlayerTwoController() ? L"player 2's" : L"another controller's";
}

bool Watching() noexcept
{
    return generation::Active() && generation::OnGameThread();
}

// --- trigger enter / leave ---

std::uintptr_t Trigger(bool enter, void* actor, void* key, void* other, void* fourth) noexcept
{
    const auto original = reinterpret_cast<TriggerFn>((enter ? g_enter : g_leave).Original());
    if (!Watching())
    {
        return original(actor, key, other, fourth);
    }
    const void* one  = generation::PlayerOneController();
    const void* two  = generation::PlayerTwoController();
    const int   whose = other == nullptr                                 ? 0
                        : other == ReadPointer(one, facts::kControllerTrigger) ? 1
                        : other == ReadPointer(two, facts::kControllerTrigger) ? 2
                                                                              : 0;
    if (whose == 0)
    {
        return original(actor, key, other, fourth);
    }
    const auto flagBefore = ReadField<std::uint8_t>(actor, kActorEnteredFlag);
    const bool oneBefore  = ReadList(one).Contains(actor);
    const bool twoBefore  = ReadList(two).Contains(actor);
    const bool scoped     = primary_controller::Is(two);
    const auto result     = original(actor, key, other, fourth);
    try
    {
        const bool oneAfter = ReadList(one).Contains(actor);
        const bool twoAfter = ReadList(two).Contains(actor);
        platform::LogDebug(L"traversal: player {}'s trigger {} {}: entered flag {} -> {}; on player 1's list {} -> {}, "
                           L"on player 2's list {} -> {} ({})",
                           whose, enter ? L"entered" : L"left", ActorText(actor), flagBefore,
                           ReadField<std::uint8_t>(actor, kActorEnteredFlag), YesNo(oneBefore), YesNo(oneAfter),
                           YesNo(twoBefore), YesNo(twoAfter), scoped ? L"in player 2's primary scope" : L"outside it");
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
                                       Who(controller), std::get<2>(now));
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
    return generation::OnGameThread()
               ? Whom(leader_scope::ObjectAt(static_cast<const std::byte*>(executer) + kExecCharacter))
               : std::wstring(L"? (off the game thread)");
}

std::uintptr_t ExecStart(void* executer) noexcept
{
    // The game's Start, through player 2's ladder camera (which passes every other climb straight through).
    const auto result = traversal_camera::LadderStart(reinterpret_cast<ExecFn>(g_execStart.Original()), executer);
    if (generation::Active())
    {
        if (generation::OnGameThread())
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
    if (generation::OnGameThread())
    {
        if (Climb* known = ClimbOf(executer))  // freed whether or not player 2 is still in
        {
            climb  = *known;
            *known = Climb{};
        }
    }
    if (generation::Active())
    {
        try
        {
            platform::LogDebug(L"traversal: a ladder climb ends for {} (executer {}; {}); {} input updates, {} of them in "
                               L"player 2's primary scope",
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
    if (generation::Active() && generation::OnGameThread())
    {
        if (Climb* climb = ClimbOf(executer))
        {
            const bool scoped = primary_controller::Is(generation::PlayerTwoController());
            if (climb->updates++ == 0)
            {
                try
                {
                    platform::LogDebug(L"traversal: the climb's first input update for {} ran {}", ExecuterCharacter(executer),
                                       scoped ? L"in player 2's primary scope" : L"outside player 2's scope");
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
    if (!generation::Active())
    {
        return;
    }
    try
    {
        const std::wstring chocobo =
            generation::OnGameThread()
                ? Whom(leader_scope::ObjectAt(static_cast<const std::byte*>(object) + kStateCharacter))
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
                  L"from trigger two); "
                  L"both controllers' lists and trigger trackers on change; actor names: ladder {}, door {}, chocobo "
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
        g_listSeen  = false;
        g_triggers    = {};
        g_trackerSeen = {};
        g_passSeen  = {};
        g_offerSeen = {};
        if (generation::OnGameThread())
        {
            g_climbs = {};  // player 2 left: no climb's End will be counted against these records
        }
        return;
    }
    const void* controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    for (int p = 0; p < 2; ++p)
    {
        const List now = ReadList(controllers[p]);
        if (g_listSeen && now == g_lists[p])
        {
            continue;
        }
        try
        {
            platform::LogDebug(L"traversal: player {}'s controller's list {}: {}", p + 1, g_listSeen ? L"changed" : L"now",
                               ListText(now));
        }
        catch (...)
        {
        }
        g_lists[p] = now;
    }
    g_listSeen = true;
    for (int p = 0; p < 2; ++p)
    {
        const TriggerState now = ReadTrigger(controllers[p]);
        if (now == g_triggers[p])
        {
            continue;
        }
        g_triggers[p] = now;
        try
        {
            static constexpr const wchar_t* kReach[] = {L"under 1 m", L"1-3 m", L"3-10 m", L"over 10 m"};
            platform::LogDebug(L"traversal: player {}'s trigger {}: {} tag(s), the first {}the trigger tag; attached to {}; "
                               L"{} from {}",
                               p + 1, now.trigger, now.tags, now.firstIsTriggerTag ? L"" : L"NOT ",
                               now.parentOwner != nullptr ? Whom(now.parentOwner) + L" (" + NameOf(now.parentOwner) + L")"
                                                          : std::wstring(L"nothing"),
                               now.reach >= 0 ? kReach[now.reach] : L"unknown distance",
                               Whom(ReadPointer(controllers[p], facts::kControllerPawn)));
        }
        catch (...)
        {
        }
    }
    for (int p = 0; p < 2; ++p)
    {
        const TrackerState now = ReadTracker(p, controllers[p]);
        if (g_trackerSeen[p] && now == g_trackers[p])
        {
            continue;
        }
        g_trackerSeen[p] = true;
        g_trackers[p]    = now;
        try
        {
            platform::LogDebug(L"traversal: player {}'s trigger tracker: enabled {}, issuing {}; overlaps {} (last pass: {} "
                               L"key(s), {} field-action actor(s)); channel {}; capsule overlap events bit {}",
                               p + 1, now.enabled, YesNo(now.issuing), now.overlaps, now.prevKeys, now.prevActors,
                               now.channel, now.capsuleOverlapBit);
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
                platform::LogDebug(L"traversal (10 s): the field tick resolved the PlayerTrigger key to player 1's "
                                   L"controller {} time(s), player 2's {}, another {}; it dispatched player 1's tracker "
                                   L"{} time(s), player 2's {}, another {}",
                                   g_resolves[0], g_resolves[1], g_resolves[2], g_dispatches[0].passes,
                                   g_dispatches[1].passes, g_dispatches[2].passes);
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

}  // namespace lss::player_two::traversal_watch
