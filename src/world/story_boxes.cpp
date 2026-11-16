#include "world/story_boxes.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/engine.h"
#include "facts/level_state.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "session/lead_gate.h"
#include "world/data_rows.h"
#include "world/story_box_rules.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace lss::world::story_boxes {

namespace {

using players::actor_fields::FirstTag;

using platform::CallGuarded;
using players::unreal::NameText;
using RC::Unreal::UClass;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;
namespace rules          = story_box_rules;

using BoxEventFn  = void (*)(void* triggerManager, std::uint64_t box, void* collider);
using LayoutFn    = std::uint64_t* (*)(std::uint64_t* out, const std::uint64_t* box);
using FindRowFn   = void* (*)(void* registry, std::uint64_t name, void* scratch, std::uint8_t* flag);
using FindSlotFn  = void* (*)(void* stateManager, std::uint64_t object, std::int32_t attribute);
using OffCodeFn   = std::uint8_t (*)(void* slot);
using LeaderFn    = void* (*)();

constexpr std::size_t  kMaxWaiting  = 32;   // more held-back enters than this at once: the next ones are the game's at once
constexpr std::int32_t kTrackerRead = 256;  // a tracker's lists are read no further than this

platform::EntryRedirect g_redirect;
std::uintptr_t          g_at        = 0;
bool                    g_pending   = false;
bool                    g_installed = false;
std::atomic<bool>       g_on{true};  // the console's switch (game thread); Remove may clear it from any thread
std::atomic<bool>       g_placed{false};
std::atomic<std::size_t> g_waitingCount{0};  // g_waiting's size, for Remove's line on any thread

BoxEventFn            g_end         = nullptr;
LayoutFn              g_layout      = nullptr;
FindRowFn             g_findRow     = nullptr;
FindSlotFn            g_findSlot    = nullptr;
OffCodeFn             g_offCode     = nullptr;
LeaderFn              g_leader      = nullptr;
data_rows::AccessorFn g_groupRow    = nullptr;
void*                 g_layoutRows  = nullptr;  // g_DataReg_ObjectLayout
const std::uintptr_t* g_root        = nullptr;  // g_EndGameRoot
UClass*               g_boxClass    = nullptr;  // /Script/Engine.BoxComponent, found on the first hold-back
bool                  g_boxClassAsked = false;

/// An enter held back: the box, the member and its seat, the box's shape when it is a box component.
struct Waiting
{
    int           seat      = 0;
    std::uint64_t member    = 0;  // weak
    std::uint64_t boxActor  = 0;  // weak
    std::uint64_t box       = 0;  // the box's tag (its layout node name)
    std::uint64_t object    = 0;  // its layout object (the state manager's name)
    std::uint64_t memberTag = 0;
    rules::Box    shape{};
    bool          shaped    = false;
    bool          awaitsRun = false;  // brought back for the level's walk through it (AwaitRun)
    bool          ranIn     = false;  // since then the game's own enter ran for the character and the box
    bool          handSeen  = false;  // since then its AI was seen holding it (the game's hand completes asynchronously)
    float         runTo[3]  = {};     // the walk's point
    std::int64_t  awaitedAt = 0;      // when AwaitRun marked it (NowMs)
    float         lastAt[3] = {};     // where the character last stood kRunStillCm away from before
    std::int64_t  movedAt   = 0;      // when it got there (NowMs)
};
std::vector<Waiting> g_waiting;

// Counts for the reading.
std::uint32_t g_heldBack = 0, g_replayed = 0, g_replayedOff = 0, g_lost = 0, g_overflow = 0, g_taken = 0;

std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Whether the walk `w` waits on is over (story_box_rules RunOver), keeping the character's last moving place in `w`. The
/// cap holds even when the character's place cannot be read.
bool RunOverNow(Waiting& w, const void* member) noexcept
{
    const std::int64_t now   = NowMs();
    float              at[3] = {};
    if (!players::unreal::ActorLocation(member, at))
    {
        return now - w.awaitedAt >= rules::kAwaitRunLimitMs;
    }
    if (std::hypot(at[0] - w.lastAt[0], at[1] - w.lastAt[1]) >= rules::kRunStillCm)
    {
        w.lastAt[0] = at[0];
        w.lastAt[1] = at[1];
        w.lastAt[2] = at[2];
        w.movedAt   = now;
    }
    return rules::RunOver(at[0] - w.runTo[0], at[1] - w.runTo[1], now - w.awaitedAt, now - w.movedAt);
}

/// The game's own enter is about to run for `collider` and `box`: a record awaiting the character's walk through it is
/// taken by that walk (story_box_rules Step::Taken). Seen at the redirect, so only an enter the game really makes counts.
void NoteRanIn(const void* collider, std::uint64_t box) noexcept
{
    for (Waiting& w : g_waiting)
    {
        if (w.awaitsRun && w.box == box && players::weak_object::StillNames(w.member, collider))
        {
            w.ranIn = true;
        }
    }
}

std::wstring Name(std::uint64_t name)
{
    return name == 0 ? std::wstring(L"None") : NameText(name);
}

void* Root() noexcept
{
    std::uintptr_t root = 0;
    return g_root != nullptr && platform::CopyGuarded(&root, g_root, sizeof(root)) ? reinterpret_cast<void*>(root) : nullptr;
}

/// The box's layout object (0: the tag maps to none, as when its level is not loaded).
std::uint64_t LayoutObjectOf(std::uint64_t box) noexcept
{
    std::uint64_t  object = 0;
    std::uint64_t* out    = nullptr;
    return CallGuarded(g_layout, out, &object, &box) ? object : 0;
}

/// The layout object's actor group (0: none, or the row is missing).
std::uint64_t GroupOf(std::uint64_t object) noexcept
{
    alignas(8) std::uint8_t scratch[16] = {};
    std::uint8_t            flag        = 0;
    void*                   row         = nullptr;
    if (object == 0 || !CallGuarded(g_findRow, row, g_layoutRows, object, static_cast<void*>(scratch), &flag) || row == nullptr)
    {
        return 0;
    }
    return ReadField<std::uint64_t>(row, facts::kLayoutRowActorGroup);
}

/// The group's ActorList holds `tag` (as the game's send reads it: by position, the first 32).
bool GroupLists(std::uint64_t group, std::uint64_t tag)
{
    const std::byte* row = tag != 0 ? data_rows::Row(g_groupRow, group) : nullptr;
    if (row == nullptr)
    {
        return false;
    }
    for (std::int32_t i = 0; i < facts::kActorGroupActorsRead; ++i)
    {
        if (data_rows::MappedNameAt(row, i) == tag)
        {
            return true;
        }
    }
    return false;
}

/// The group names the party leader: player 1's character (the game's send compares the leader's tag for its
/// PlayerTrigger). Read outside every player's scope.
bool GroupNamesLeader(std::uint64_t group)
{
    void* leader = nullptr;
    return CallGuarded(g_leader, leader) && leader != nullptr && GroupLists(group, FirstTag(leader));
}

/// What the game's enter does for the box now (its own reads, before the replay calls it).
enum class Sends : std::uint8_t
{
    Trigger,    // its trigger is sent (when the group lists the member)
    Off,        // the box is switched off: nothing is sent
    NoTrigger,  // the object has no trigger state (or its states are not loaded)
};

Sends WouldSend(std::uint64_t object) noexcept
{
    void*        manager = players::actor_fields::RootGameState(Root());
    void*        slot    = nullptr;
    std::uint8_t off     = 0;
    if (manager == nullptr || !CallGuarded(g_findSlot, slot, manager, object, facts::kBoxSlotAttribute) || slot == nullptr ||
        ReadPointer(slot, facts::kSlotState) == nullptr)
    {
        return Sends::NoTrigger;
    }
    return CallGuarded(g_offCode, off, slot) && off == facts::kSlotOff ? Sends::Off : Sends::Trigger;
}

}  // namespace

std::uint64_t TriggerStateRow(std::uint64_t object) noexcept
{
    void* manager = g_installed ? players::actor_fields::RootGameState(Root()) : nullptr;
    void* slot    = nullptr;
    return manager != nullptr && CallGuarded(g_findSlot, slot, manager, object, facts::kBoxSlotAttribute) && slot != nullptr
               ? ReadField<std::uint64_t>(slot, facts::kSlotState)
               : 0;
}

namespace {

const wchar_t* SendsText(Sends sends, bool inside) noexcept
{
    switch (sends)
    {
    case Sends::Trigger:
        return L"its trigger sent";
    case Sends::Off:
        return inside ? L"the box is switched off: nothing sent now; the character is recorded inside, so the box fires for "
                        L"it when it is switched on, as the game does for anyone inside"
                      : L"the box is switched off and the character is outside: nothing sent (POSSIBLY LOST: a line it would "
                        L"start when switched on will not, as for a companion that passed it while off)";
    case Sends::NoTrigger:
        return L"the box has no trigger state now: nothing to send (POSSIBLY LOST if its states were not loaded)";
    }
    return L"?";
}

/// A tracker list of the character's: its elements (inline, or on the heap when its pointer is set) and count.
template <class F>
bool ForEachTracked(const void* character, std::uint32_t inlineAt, std::uint32_t heapAt, std::uint32_t numAt, F&& each)
{
    const auto* tracker = static_cast<const std::uint8_t*>(character) + facts::kCharacterTracker;
    const void* heap    = ReadPointer(tracker, heapAt);
    const auto* data    = static_cast<const std::uint64_t*>(heap != nullptr ? heap : tracker + inlineAt);
    const auto  count   = ReadField<std::int32_t>(tracker, numAt);
    const auto  limit   = heap != nullptr ? kTrackerRead : facts::kTrackerInline;  // inline: never past its own room
    for (std::int32_t i = 0; i < count && i < limit; ++i)
    {
        std::uint64_t value = 0;
        if (platform::Read(data + i, value) && each(value))
        {
            return true;
        }
    }
    return false;
}

/// The character's tracker held the box at its last pass (it is inside now).
bool Inside(const void* character, std::uint64_t box)
{
    return ForEachTracked(character, facts::kTrackerKeys, facts::kTrackerKeysHeap, facts::kTrackerKeysNum,
                          [box](std::uint64_t key) { return key == box; });
}

/// The box actor this pass's sweep of the character overlaps under `box` (null when none).
void* BoxActorOf(const void* character, std::uint64_t box)
{
    void* found = nullptr;
    ForEachTracked(character, facts::kTrackerOverlaps, facts::kTrackerOverlapsHeap, facts::kTrackerOverlapsNum,
                   [&found, box](std::uint64_t weak) {
                       void* actor = players::weak_object::ObjectOf(weak);
                       if (actor != nullptr && FirstTag(actor) == box)
                       {
                           found = actor;
                           return true;
                       }
                       return false;
                   });
    return found;
}

bool IsBoxComponentRaw(void* component)
{
    return static_cast<UObject*>(component)->IsA(g_boxClass);
}

/// The box actor's collision component as a placed box (its world transform and half-size). False when it is not a
/// box component or cannot be read.
bool ShapeOf(void* boxActor, rules::Box& out) noexcept
{
    if (!g_boxClassAsked)
    {
        g_boxClassAsked = true;
        try
        {
            g_boxClass = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.BoxComponent"));
        }
        catch (...)
        {
            g_boxClass = nullptr;
        }
        if (g_boxClass == nullptr)
        {
            platform::LogWarning(L"story boxes: the box component class was not found; a held-back box fires when the level "
                                 L"moves the character or its player leaves, not when player 1 comes to it");
        }
    }
    void* component = ReadPointer(boxActor, facts::kTriggerCollisionComponent);
    bool  isBox     = false;
    if (g_boxClass == nullptr || component == nullptr || !CallGuarded(&IsBoxComponentRaw, isBox, component) || !isBox)
    {
        return false;
    }
    const auto* c = static_cast<const std::uint8_t*>(component);
    return platform::CopyGuarded(out.rotation, c + facts::kComponentWorldRotation, sizeof(out.rotation)) &&
           platform::CopyGuarded(out.at, c + facts::kComponentWorldLocation, sizeof(out.at)) &&
           platform::CopyGuarded(out.scale, c + facts::kComponentWorldScale, sizeof(out.scale)) &&
           platform::CopyGuarded(out.extent, c + facts::kBoxComponentExtent, sizeof(out.extent));
}

/// The character's controller is its seat's player's (not its AI's).
bool HeldByPlayer(const void* member, int seat) noexcept
{
    const void* controller = players::actor_fields::Controller(member);
    return controller != nullptr && controller == players::generation::ControllerOf(seat);
}

bool Remembered(const void* member, std::uint64_t box) noexcept
{
    for (const Waiting& w : g_waiting)
    {
        if (w.box == box && players::weak_object::StillNames(w.member, member))
        {
            return true;
        }
    }
    return false;
}

/// The redirected enter (game thread, the field tick's tracker dispatch).
void Begin(void* manager, std::uint64_t box, void* collider)
{
    const auto original = reinterpret_cast<BoxEventFn>(g_redirect.Original());
    if (!g_on.load() || collider == nullptr || !players::generation::OnGameThread())
    {
        return original(manager, box, collider);
    }
    const int seat = players::generation::SeatOfMember(collider);
    if (seat < 1 || seat > players::kSeatsBuilt)
    {
        NoteRanIn(collider, box);  // a character riding along walks into the box on its AI
        return original(manager, box, collider);
    }
    try
    {
        const std::uint64_t tag = FirstTag(collider);
        if (Remembered(collider, box))
        {
            // Walked out and in again, or swept again by a move, while its first enter waits: one enter is remembered.
            platform::LogDebug(L"story boxes: {}'s {} walked into {} again; its first enter still waits", players::SeatName(seat),
                               Name(tag), Name(box));
            return;
        }
        const std::uint64_t object = LayoutObjectOf(box);
        const std::uint64_t group  = GroupOf(object);
        const rules::Entry  entry{.heldByPlayer     = HeldByPlayer(collider, seat),
                                  .levelSentIt      = session::lead_gate::OnLevelMove(collider),
                                  .groupSet         = group != 0,
                                  .groupNamesLeader = group != 0 && GroupNamesLeader(group)};
        if (!rules::HoldBack(entry) || !GroupLists(group, tag) || WouldSend(object) != Sends::Trigger)
        {
            NoteRanIn(collider, box);
            return original(manager, box, collider);  // the game's: a party box, on its AI or sent, not its box, off
        }
        if (g_waiting.size() >= kMaxWaiting)
        {
            ++g_overflow;
            platform::Log(L"story boxes: {} enters already wait; {}'s {} enters {} now, as the game would", g_waiting.size(),
                          players::SeatName(seat), Name(tag), Name(box));
            return original(manager, box, collider);
        }
        Waiting w;
        w.seat      = seat;
        w.member    = players::weak_object::WeakOf(collider);
        w.box       = box;
        w.object    = object;
        w.memberTag = tag;
        if (void* boxActor = BoxActorOf(collider, box); boxActor != nullptr)
        {
            w.boxActor = players::weak_object::WeakOf(boxActor);
            w.shaped   = ShapeOf(boxActor, w.shape);
        }
        if (w.member == 0 || w.boxActor == 0)
        {
            return original(manager, box, collider);  // cannot be followed between ticks: the game's now
        }
        platform::Log(L"story boxes: {}'s {} walked into {} (group {}, object {}) ahead of the story; it waits until the level "
                      L"moves the character or hands it to its AI, its player leaves, or player 1 comes to the box{}",
                      players::SeatName(seat), Name(tag), Name(box), Name(group), Name(object),
                      w.shaped ? L"" : L" (no box shape read: player 1's arrival is not counted)");
        g_waiting.push_back(w);  // last: nothing after it can throw, so the catch below never enters a remembered box
        g_waitingCount.store(g_waiting.size());
        ++g_heldBack;
    }
    catch (...)
    {
        return original(manager, box, collider);
    }
}

/// The character enters the box now: the game's own enter, then its leave when the character is no longer inside.
void Replay(const Waiting& w, void* member, const wchar_t* why)
{
    void*       manager = players::actor_fields::RootTriggerManager(Root());
    const auto  begin   = reinterpret_cast<BoxEventFn>(g_redirect.Original());
    const Sends sends   = WouldSend(w.object);
    const bool  began   = manager != nullptr && CallGuarded(begin, manager, w.box, member);
    const bool  inside  = Inside(member, w.box);
    const bool  ended   = began && !inside && CallGuarded(g_end, manager, w.box, member);
    ++g_replayed;
    if (sends == Sends::Off)
    {
        ++g_replayedOff;
    }
    platform::Log(L"story boxes: {}'s {} enters {} now ({}): {}; {}", players::SeatName(w.seat), Name(w.memberTag), Name(w.box),
                  why, began ? SendsText(sends, inside) : L"the game's enter could not be called (LOST: its line may not play)",
                  !began ? L"nothing recorded" : inside ? L"still inside" : ended ? L"outside again: the game's leave followed"
                                                                                  : L"outside again: the leave could not be called");
}

const wchar_t* WhyReplay(const rules::Waiting& w) noexcept
{
    if (w.awaitsRun)
    {
        return !w.seated            ? L"its player left"
               : w.playerOneReached ? L"player 1 came to the box"
               : !w.handedToAi      ? L"back with its player before its walk took the box"
                                    : L"its walk is over without taking the box (at the walk's point, stopped for 2 s, or 20 s gone)";
    }
    return !w.seated ? L"its player left" : w.handedToAi ? L"handed to its AI" : w.levelMoves ? L"the level sent it on a move"
                                                                                           : L"player 1 came to the box";
}

std::wstring Reading()
{
    std::wstring text = std::format(L"redirect {}{}; held back {}, replayed {} ({} with the box off), taken by the character's own "
                                    L"walk {}, lost {}, overflow {}; waiting {}",
                                    g_placed.load() ? L"placed" : L"not placed", g_on.load() ? L"" : L" (switched off)", g_heldBack,
                                    g_replayed, g_replayedOff, g_taken, g_lost, g_overflow, g_waiting.size());
    for (const Waiting& w : g_waiting)
    {
        text += std::format(L"; {}'s {} in {}{}", players::SeatName(w.seat), Name(w.memberTag), Name(w.box),
                            w.shaped ? L"" : L" (no shape)");
    }
    return text;
}

bool InstallChecked(const platform::GameImage& image)
{
    const auto begin    = platform::FindFunction(image, facts::kBoxBegin);
    const auto end      = platform::FindFunction(image, facts::kBoxEnd);
    const auto layout   = platform::FindFunction(image, facts::kBoxLayoutObject);
    const auto findRow  = platform::FindFunction(image, facts::kFindRowByName);
    const auto findSlot = platform::FindFunction(image, facts::kFindObjectByKeyAndAttribute);
    const auto offCode  = platform::FindFunction(image, facts::kSlotOffCode);
    const auto groupRow = platform::FindFunction(image, facts::kActorGroupRow);
    const auto leader   = platform::FindFunction(image, facts::kResolvePartyLeader);
    const auto rows     = image.Rebase(facts::kObjectLayoutRegistry, sizeof(std::uintptr_t));
    const auto root     = players::game_globals::Address(players::game_globals::Global::GameRoot);
    if (!begin || !end || !layout || !findRow || !findSlot || !offCode || !groupRow || !leader || !rows || !root ||
        !players::weak_object::Known())
    {
        platform::Log(L"story boxes: an engine check failed; player 2's or 3's character fires its own story boxes as it walks "
                      L"into them, as before");
        return false;
    }
    g_at         = *begin;
    g_end        = reinterpret_cast<BoxEventFn>(*end);
    g_layout     = reinterpret_cast<LayoutFn>(*layout);
    g_findRow    = reinterpret_cast<FindRowFn>(*findRow);
    g_findSlot   = reinterpret_cast<FindSlotFn>(*findSlot);
    g_offCode    = reinterpret_cast<OffCodeFn>(*offCode);
    g_groupRow   = reinterpret_cast<data_rows::AccessorFn>(*groupRow);
    g_leader     = reinterpret_cast<LeaderFn>(*leader);
    g_layoutRows = reinterpret_cast<void*>(*rows);
    g_root       = reinterpret_cast<const std::uintptr_t*>(root);
    g_pending    = true;  // placed by the game thread's tick, outside the field tick
    g_installed  = true;
#if LSS_DEV
    control::Register({.name  = L"story-box-hold",
                       .shape = control::Shape::OnOff,
                       .help  = L"player 2's or 3's character walking into its own story box ahead of the story waits, and fires "
                                L"it when the level moves it, its player leaves or player 1 comes to the box; off: it fires at "
                                L"once, as before (on at start; switching off fires every waiting one at the next tick)",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value));
                           platform::Log(L"story boxes: {}", g_on.load() ? L"on" : L"off: waiting enters fire at the next tick");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load()}; }});
    control::Register({.name  = L"world.story-boxes",
                       .shape = control::Shape::None,
                       .help  = L"(read only) the story-box hold: what it held back, replayed and lost, and what waits",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
#endif
    return true;
}

}  // namespace

bool ArmedOwnBox(std::uint64_t object, const void* member) noexcept
try
{
    if (!g_installed || object == 0 || member == nullptr)
    {
        return false;
    }
    const std::uint64_t group = GroupOf(object);
    return group != 0 && GroupLists(group, FirstTag(member)) && !GroupNamesLeader(group) && WouldSend(object) == Sends::Trigger;
}
catch (...)
{
    return false;
}

bool HeldShape(const void* member, std::uint64_t object, story_box_rules::Box& out) noexcept
try
{
    for (const Waiting& w : g_waiting)
    {
        if (w.object == object && w.shaped && players::weak_object::StillNames(w.member, member))
        {
            out = w.shape;
            return true;
        }
    }
    return false;
}
catch (...)
{
    return false;
}

int AwaitRun(const void* member, const float from[3], const float to[3]) noexcept
try
{
    int marked = 0;
    for (Waiting& w : g_waiting)
    {
        if (w.shaped && players::weak_object::StillNames(w.member, member) && rules::RunCrosses(w.shape, from, to))
        {
            w.awaitsRun = true;
            w.ranIn     = false;
            w.handSeen  = false;
            w.runTo[0]  = to[0];
            w.runTo[1]  = to[1];
            w.runTo[2]  = to[2];
            w.awaitedAt = NowMs();
            w.movedAt   = w.awaitedAt;
            w.lastAt[0] = from[0];
            w.lastAt[1] = from[1];
            w.lastAt[2] = from[2];
            ++marked;
            platform::LogDebug(L"story boxes: {}'s {} enter of {} waits for its own walk through it", players::SeatName(w.seat),
                               Name(w.memberTag), Name(w.box));
        }
    }
    return marked;
}
catch (...)
{
    return 0;
}

bool Install(const platform::GameImage& image) noexcept
try
{
    return InstallChecked(image);
}
catch (...)
{
    g_installed = false;
    return false;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on.store(false);
    if (const std::size_t waiting = g_waitingCount.load(); waiting != 0)
    {
        // The list is the game thread's; Remove may run on any thread, so it is left to the next Tick, if any.
        platform::Log(L"story boxes: removed with {} enter(s) waiting; unless a tick still runs they are dropped (POSSIBLY "
                      L"LOST: their lines may not play)",
                      waiting);
    }
    g_placed.store(false);
    if (!g_redirect.Restore())
    {
        platform::Log(L"story boxes: the enter's entry was changed after us; left in place (it passes everything while off)");
    }
}

void Tick(bool splitShown) noexcept
try
{
    if (g_pending)
    {
        g_pending = false;
        g_placed.store(g_redirect.Install(g_at, facts::kBoxBeginFirst, reinterpret_cast<std::uintptr_t>(&Begin)));
        platform::Log(L"story boxes: {}", g_placed.load() ? L"player 2's or 3's character walking into its own story box ahead of "
                                                          L"the story waits for the story's moment"
                                                        : L"the enter could not be redirected; characters fire their own story "
                                                          L"boxes as they walk into them, as before");
    }
    if (g_waiting.empty())
    {
        return;
    }
    float       one[3]  = {};
    void*       pawnOne = nullptr;
    const bool  haveOne = CallGuarded(g_leader, pawnOne) && pawnOne != nullptr && players::unreal::ActorLocation(pawnOne, one);
    for (std::size_t i = 0; i < g_waiting.size();)
    {
        const Waiting w        = g_waiting[i];  // a copy: the game calls below must never see a reference into the list
        void*         member   = players::weak_object::ObjectOf(w.member);
        const bool    boxThere = LayoutObjectOf(w.box) == w.object &&
                              (w.boxActor == 0 || players::weak_object::ObjectOf(w.boxActor) != nullptr);
        rules::Waiting state{.boxGone = !boxThere || member == nullptr};
        if (!state.boxGone)
        {
            state.seated           = players::generation::CameraMemberOf(w.seat) == member;
            state.handedToAi       = !HeldByPlayer(member, w.seat);
            // With the player in, the split leaves the screen only for a cutscene, a menu or the map; once the player
            // has left, nothing holds the character's box any more.
            state.inCinema         = state.seated && !splitShown;
            // The level's walk into this very box is its moment only once its AI has the character (the guide's hand
            // marks the entry AwaitRun); before that hand, polled less often than this tick, it is no release.
            state.levelMoves = session::lead_gate::OnLevelMove(member) && !session::lead_gate::WalksIntoBox(member, TriggerStateRow(w.object));
            state.playerOneReached = w.shaped && haveOne && rules::Reaches(w.shape, one);
            state.awaitsRun        = w.awaitsRun;
            state.ranIn            = w.ranIn;
            if (w.awaitsRun)
            {
                state.runOver = RunOverNow(g_waiting[i], member);  // its moving place kept in the list's record
                // Marked just before the hand: "back with its player" counts only once its AI was seen holding it.
                if (state.handedToAi)
                {
                    g_waiting[i].handSeen = true;
                }
                state.handedToAi = state.handedToAi || !w.handSeen;
            }
        }
        if (!g_on.load() && !state.boxGone)
        {
            state.seated = false;  // switched off: every waiting enter fires now, as the game would have
        }
        const rules::Step step = rules::Next(state);
        if (step == rules::Step::Wait)
        {
            ++i;
            continue;
        }
        // Out of the list before the game's calls: an exception after the replay must not replay it again.
        g_waiting.erase(g_waiting.begin() + static_cast<std::ptrdiff_t>(i));
        g_waitingCount.store(g_waiting.size());
        if (step == rules::Step::Taken)
        {
            ++g_taken;
            platform::Log(L"story boxes: {}'s {} walked into {} on its AI: the game's own enter took it", players::SeatName(w.seat),
                          Name(w.memberTag), Name(w.box));
        }
        else if (step == rules::Step::Replay)
        {
            Replay(w, member, g_on.load() ? WhyReplay(state) : L"story-box-hold switched off");
        }
        else
        {
            ++g_lost;
            platform::Log(L"story boxes: LOST {}'s {} enter of {}: {} before the story's moment (a line it starts may not play)",
                          players::SeatName(w.seat), Name(w.memberTag), Name(w.box),
                          member == nullptr ? L"the character went away" : L"the box went away (its level unloaded, a load)");
        }
    }
}
catch (...)
{
    platform::LogWarning(L"story boxes: the tick raised; the enter being handled is dropped, the others wait for the next");
}

}  // namespace lss::world::story_boxes
