#include "session/lead_gate.h"

#include "facts/engine.h"
#include "facts/level_state.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "session/field_hold.h"
#include "session/state_path.h"
#include "world/data_rows.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::session::lead_gate {

namespace {

using lead_keep_rules::Verdict;
using players::unreal::NameText;
using world::data_rows::AccessorFn;
using world::data_rows::MappedCount;
using world::data_rows::MappedData;
using world::data_rows::MappedNameAt;
using world::data_rows::MappedNames;
using world::data_rows::MappedText;
using world::data_rows::Row;

using CollectFn   = void* (*)(void* out);
using SlotFn      = void* (*)(void* stateManager, std::int32_t key);
using AiSystemFn  = void* (*)();
using FreeArrayFn = void (*)(void* array);

// The hold check calls every 6 ticks: a staging move's waiters are read again every 30 calls (about 3 s; each census
// makes the game's slot collector reserve 1 MB), the marker every call.
constexpr std::uint32_t kCensusEveryCalls = 30;
constexpr std::size_t   kPathLinks        = 16;
constexpr std::int32_t  kNextStatesRead   = 16;
constexpr std::int32_t  kIconBitsMax      = 16384;  // the field's icon objects: far above the whole table's rows
constexpr std::int32_t  kIconSlotsMax     = 16384;
constexpr std::int32_t  kGroupArg         = 0;      // a setting group row's group name

AccessorFn            g_stateRow   = nullptr;
AccessorFn            g_triggerRow = nullptr;
CollectFn             g_collect    = nullptr;
SlotFn                g_slot       = nullptr;
AiSystemFn            g_aiSystem   = nullptr;
FreeArrayFn           g_freeArray  = nullptr;
CRITICAL_SECTION*     g_lock       = nullptr;
const std::uintptr_t* g_root       = nullptr;
const std::uintptr_t* g_icons      = nullptr;
bool                  g_installed  = false;

/// A level's walk or lead on a member, as its AI state holds it.
struct Move
{
    std::uint8_t  state   = 0;
    std::uint64_t object  = 0;  // the setting's level object
    std::uint64_t row     = 0;  // its state row
    std::uint64_t trigger = 0;  // its arrival trigger (0: the row's string argument 2)
};

/// A walk or lead judged, and the verdict that holds for it. The default verdict is the AI's: a judgment interrupted
/// before its verdict leaves the walk to the AI.
struct Judgment
{
    std::uint64_t object = 0, row = 0, trigger = 0;
    Verdict       verdict    = Verdict::Guide;
    bool          waiter     = false;  // another waiter, a direct set on another object, or a further move in the chain
    bool          marker     = false;  // the story's marker rides on the member
    std::uint32_t censusCall = 0;
    std::uint32_t calls      = 0;
    std::uint32_t touched    = 0;      // the clock at the last call (the least recently used slot is reused)
};
/// One judgment per level object; four covers players 2 and 3 with room to spare.
std::array<Judgment, 4>                       g_judged;
std::array<std::uint64_t, players::kMaxPlayers> g_seatObject{};  // the level object each seat's member was last judged on
std::uint32_t                                 g_clock   = 0;
std::wstring                                  g_reading = L"none yet";

Judgment& JudgmentOf(std::uint64_t object) noexcept
{
    Judgment* slot = nullptr;
    for (Judgment& j : g_judged)
    {
        if (j.object == object)
        {
            slot = &j;
            break;
        }
        if (slot == nullptr || j.touched < slot->touched)
        {
            slot = &j;
        }
    }
    slot->touched = ++g_clock;
    return *slot;
}

std::wstring Name(std::uint64_t name)
{
    return name == 0 ? std::wstring(L"None") : NameText(name);
}

/// The member's walk or lead on the level's brains, from its active AI state.
bool MoveOf(const void* member, Move& out) noexcept
{
    const void* brain = ReadPointer(ReadPointer(member, facts::kCharacterRetainedAi), facts::kAiActiveBrain);
    const void* state = ReadPointer(brain, facts::kBrainState);
    if (state == nullptr || !field_hold::LevelBrain(ReadField<std::uint8_t>(brain, facts::kBrainIndex)))
    {
        return false;
    }
    out.state = ReadField<std::uint8_t>(state, facts::kAiStateType);
    if (out.state == facts::kAiStateMoveTo)
    {
        out.object  = ReadField<std::uint64_t>(state, facts::kMoveToObject);
        out.row     = ReadField<std::uint64_t>(state, facts::kMoveToRow);
        out.trigger = ReadField<std::uint64_t>(state, facts::kMoveToTrigger);
        return true;
    }
    if (out.state == facts::kAiStateLead)
    {
        out.object  = ReadField<std::uint64_t>(state, facts::kLeadObject);
        out.row     = ReadField<std::uint64_t>(state, facts::kLeadRow);
        out.trigger = ReadField<std::uint64_t>(state, facts::kLeadTrigger);
        return true;
    }
    return false;
}

void* StateManager() noexcept
{
    std::uintptr_t root = 0;
    if (g_root == nullptr || !platform::CopyGuarded(&root, g_root, sizeof(root)) || root == 0)
    {
        return nullptr;
    }
    return ReadPointer(reinterpret_cast<const void*>(root), facts::kRootGameState);
}

/// Whether `lock` reads as an initialised Win32 critical section (a wrong address must not reach EnterCriticalSection).
bool LooksLikeCriticalSection(const CRITICAL_SECTION* lock) noexcept
{
    CRITICAL_SECTION copy{};
    return lock != nullptr && platform::CopyGuarded(&copy, lock, sizeof(copy)) && copy.DebugInfo != nullptr &&
           copy.RecursionCount >= 0 && copy.RecursionCount < 4096;
}

struct Slot
{
    std::uint64_t object = 0;  // the owner object
    std::uint64_t state  = 0;  // its current state row
};

/// Every active slot of the level's state manager (its owner and current row), through the game's own collector under
/// the state-trigger lock, its key array freed as the game frees it. Nullopt when the collector cannot be run.
std::optional<std::vector<Slot>> ActiveSlots(void* stateManager)
{
    alignas(8) std::array<std::byte, 24> keys{};
    std::vector<std::int32_t>            copied;
    {
        EnterCriticalSection(g_lock);
        void*      filled    = nullptr;
        const bool collected = platform::CallGuarded(g_collect, filled, static_cast<void*>(keys.data()));
        const auto* data     = static_cast<const std::int32_t*>(ReadPointer(keys.data(), 0));
        const auto  count    = std::clamp(ReadField<std::int32_t>(keys.data(), 8), 0, facts::kMaxActiveSlots);
        bool sized = true;
        if (collected && data != nullptr && count > 0)
        {
            try
            {
                copied.resize(static_cast<std::size_t>(count));
            }
            catch (...)
            {
                sized = false;  // nothing may leave this block before the free and the unlock
            }
        }
        const bool read = sized && !copied.empty() &&
                          platform::CopyGuarded(copied.data(), data, copied.size() * sizeof(std::int32_t));
        platform::CallGuarded(g_freeArray, static_cast<void*>(keys.data()));  // a null array frees nothing
        LeaveCriticalSection(g_lock);
        if (!collected || !sized || (count > 0 && data == nullptr) || (!copied.empty() && !read))
        {
            return std::nullopt;
        }
    }
    std::vector<Slot> slots;
    slots.reserve(copied.size());
    for (const std::int32_t key : copied)
    {
        void* slot = nullptr;
        if (platform::CallGuarded(g_slot, slot, stateManager, key) && slot != nullptr)
        {
            slots.push_back({ReadField<std::uint64_t>(slot, facts::kSlotOwner), ReadField<std::uint64_t>(slot, facts::kSlotState)});
        }
    }
    return slots;
}

/// A setting group's states, from the AI system's group map (field_hold::GroupState), its template rows skipped as the
/// game's list builder skips them.
std::vector<field_hold::GroupState> GroupStatesOf(std::uint64_t group)
{
    std::vector<field_hold::GroupState> states;
    void*                               ai = nullptr;
    if (group == 0 || !platform::CallGuarded(g_aiSystem, ai) || ai == nullptr)
    {
        return states;
    }
    const auto map = reinterpret_cast<std::uintptr_t>(ai) + facts::kAiSystemGroups;
    platform::ForEachSparseElement(map, facts::kGroupPairStride, [&](std::uintptr_t pair) {
        const auto* element = reinterpret_cast<const void*>(pair);
        if (ReadField<std::uint64_t>(element, 0) != group)
        {
            return true;
        }
        const auto* entries = static_cast<const std::byte*>(ReadPointer(element, facts::kGroupPairData));
        const auto  count   = std::min(ReadField<std::int32_t>(element, facts::kGroupPairCount), facts::kGroupStatesRead);
        for (std::int32_t i = 0; entries != nullptr && i < count; ++i)
        {
            const auto* row = static_cast<const std::byte*>(
                ReadPointer(entries + static_cast<std::size_t>(i) * facts::kGroupEntryStride, facts::kGroupEntryRow));
            const auto state = row != nullptr ? ReadField<std::uint8_t>(row, facts::kGroupRowState) : std::uint8_t{0};
            if (state == 0)
            {
                continue;
            }
            // The rep index of string slot 2, as the game's getter reads it (absent: 0).
            const auto* rep      = MappedData(row + facts::kGroupRowRepIndex);
            const auto  repCount = MappedCount(row + facts::kGroupRowRepIndex, 3);
            states.push_back({state, rep != nullptr && repCount > 2 ? ReadField<std::int8_t>(rep, 2) : std::int8_t{0}});
        }
        return false;
    });
    return states;
}

/// A state row as the walk needs it, through the game's own lookup (state_path::Row); nullopt when unknown.
std::optional<state_path::Row> PathRow(const std::wstring& name)
{
    const std::byte* row = Row(g_stateRow, name);
    if (row == nullptr)
    {
        return std::nullopt;
    }
    state_path::Row out;
    out.status        = MappedText(row + facts::kStateRowStatus);
    const auto* next  = MappedData(row + facts::kStateRowNext);
    const auto  nexts = MappedCount(row + facts::kStateRowNext, kNextStatesRead);
    for (std::int32_t i = 0; i < nexts && next != nullptr; ++i)
    {
        // Empty: no change on that trigger.
        out.next.push_back(MappedText(next + 16 * static_cast<std::size_t>(i), world::data_rows::kMaxNameChars));
    }
    for (const std::uint64_t swap : MappedNames(row + facts::kStateRowSwaps, facts::kStateRowSwapsRead))
    {
        out.next.push_back(NameText(swap));
    }
    if (field_hold::SameIgnoringCase(out.status, field_hold::kGroupStatus))
    {
        const auto states = GroupStatesOf(MappedNameAt(row + facts::kStateRowStringArgs, kGroupArg));
        out.groupMoves    = field_hold::GroupMoves(states.data(), states.size());
        out.groupFollows  = field_hold::GroupFollows(states.data(), states.size());
    }
    else
    {
        out.destination = MappedNameAt(row + facts::kStateRowStringArgs, facts::kStateRowDestinationArg);
    }
    return out;
}

struct Path
{
    bool         move = false;  // a further move among the states ahead, before the member stands
    std::wstring text;
};

/// The member's own states ahead of `first`: a further move among them (or a chain past the limit) makes the chain the
/// level's path for the member.
Path PathAhead(const std::wstring& first)
{
    Path path;
    if (first.empty())
    {
        path.text = L"no next state";
        return path;
    }
    const state_path::Walk walk   = state_path::WalkFrom(first, &PathRow, kPathLinks);
    const auto             status = [&walk](std::size_t i) -> std::wstring {
        const state_path::Link& link = walk.links[i];
        return !link.read ? L"status unread" : link.status.empty() ? L"no status" : link.status;
    };
    path.move = state_path::FurtherMove(walk);
    if (walk.firstMove)
    {
        const std::vector<std::size_t> trail = state_path::Trail(walk, *walk.firstMove);
        for (std::size_t k = 0; k < trail.size(); ++k)
        {
            path.text += std::format(L"{}{} ({})", k == 0 ? L"" : L" -> ", walk.links[trail[k]].name, status(trail[k]));
        }
        path.text += L": a further move ahead, the level walks the member on";
    }
    else
    {
        for (std::size_t i = 0; i < walk.links.size(); ++i)
        {
            path.text += std::format(L"{}{} ({})", i == 0 ? L"" : L", ", walk.links[i].name, status(i));
        }
        path.text += walk.truncated ? L": the walk's limit with the chain still going, read as a further move"
                                    : L": the path's end, no further move";
    }
    for (const state_path::Link& link : walk.links)
    {
        if (link.read && link.move && link.destination != 0)
        {
            path.text += std::format(L"; {} moves to {}", link.name, Name(link.destination));
        }
    }
    return path;
}

struct Census
{
    bool         read    = false;  // the census ran whole (unread: the walk is the AI's)
    bool         chain   = false;  // a further move ahead in the member's own chain
    int          others  = 0;      // other waiters (in scope) plus direct sets on other objects
    std::wstring text;
};

/// The waiters on the move's arrival trigger: the slots whose current state lists it, in the trigger's scope (Self:
/// the member's own object's slots; SpecificObject: the trigger's SendObject list and the member's own; else every
/// active slot), the trigger row's direct sets on other objects, and the member's own path from the state its arrival
/// leads to.
Census Waiters(const Move& move)
{
    Census           census;
    const std::byte* self = Row(g_stateRow, move.row);
    if (self == nullptr)
    {
        census.text = std::format(L"its state row {} not found", Name(move.row));
        return census;
    }
    const std::uint64_t trigger =
        move.trigger != 0 ? move.trigger : MappedNameAt(self + facts::kStateRowStringArgs, facts::kStateRowTriggerArg);
    if (trigger == 0)
    {
        census.read = true;
        census.text = L"no arrival trigger (nothing can wait)";
        return census;
    }
    // The member's own step: its row's trigger list pairs the arrival with the next state.
    std::wstring ownNext;
    {
        const std::vector<std::uint64_t> triggers = MappedNames(self + facts::kStateRowTriggers, facts::kStateRowTriggersRead);
        const auto*                      next     = MappedData(self + facts::kStateRowNext);
        const auto                       nexts    = MappedCount(self + facts::kStateRowNext, kNextStatesRead);
        for (std::size_t i = 0; i < triggers.size(); ++i)
        {
            if (triggers[i] == trigger && next != nullptr && static_cast<std::int32_t>(i) < nexts)
            {
                ownNext = MappedText(next + 16 * i, world::data_rows::kMaxNameChars);
                break;
            }
        }
    }
    const Path path = PathAhead(ownNext);
    census.chain    = path.move;
    // The trigger row: its scope and direct sets.
    const std::byte*           triggerRow = Row(g_triggerRow, trigger);
    std::uint8_t               scope      = 0xFF;
    std::vector<std::uint64_t> sendObjects;
    std::wstring               sets;
    if (triggerRow != nullptr)
    {
        scope       = ReadField<std::uint8_t>(triggerRow, facts::kTriggerRowScope);
        sendObjects = MappedNames(triggerRow + facts::kTriggerRowSendObjects, facts::kTriggerRowSendObjectsMax);
        for (const std::uint64_t object : MappedNames(triggerRow + facts::kTriggerRowObjects, facts::kTriggerRowDirectSetsMax))
        {
            if (object != move.object)
            {
                ++census.others;
                sets += std::format(L"; sets {}", Name(object));
            }
        }
    }
    void* stateManager = StateManager();
    if (stateManager == nullptr)
    {
        census.text = L"the level's state manager is not up";
        return census;
    }
    const std::optional<std::vector<Slot>> slots = ActiveSlots(stateManager);
    if (!slots)
    {
        census.text = L"the level's active slots could not be read";
        return census;
    }
    int          listed = 0;
    std::wstring waiting;
    for (const Slot& s : *slots)
    {
        const bool own = s.object == move.object;
        if (own && s.state == move.row)
        {
            continue;  // the member's own step (its path is read above)
        }
        if (scope == facts::kTriggerScopeSelf && !own)
        {
            continue;
        }
        if (scope == facts::kTriggerScopeSpecific && !own &&
            std::find(sendObjects.begin(), sendObjects.end(), s.object) == sendObjects.end())
        {
            continue;
        }
        const std::byte* row = Row(g_stateRow, s.state);
        if (row == nullptr)
        {
            continue;
        }
        const std::vector<std::uint64_t> triggers = MappedNames(row + facts::kStateRowTriggers, facts::kStateRowTriggersRead);
        if (std::find(triggers.begin(), triggers.end(), trigger) != triggers.end())
        {
            ++census.others;
            ++listed;
            waiting += std::format(L"; {} {} waits", Name(s.object), Name(s.state));
        }
    }
    census.read = true;
    census.text = std::format(L"trigger {} (row {}, scope {}), {} waiting of {} active{}{}; self -> {}", Name(trigger),
                              triggerRow != nullptr ? L"found" : L"NOT found",
                              scope == 0xFF ? std::wstring(L"?") : std::to_wstring(scope), listed, slots->size(), sets, waiting,
                              path.text);
    return census;
}

struct Marker
{
    bool         onMember = false;
    std::wstring text;
};

/// The story's objective markers shown now and what they ride on (the field icon manager's objects: a story icon on a
/// level object, kind main story or its intermediate, shown this update). On the member when one rides on its level
/// object, the name the game itself follows it by. The text only when `describe`.
Marker Markers(std::uint64_t object, bool describe)
{
    Marker         marker;
    std::uintptr_t system = 0;
    if (g_icons == nullptr || !platform::CopyGuarded(&system, g_icons, sizeof(system)) || system == 0)
    {
        marker.text = L"no field icons";
        return marker;
    }
    const void* manager = ReadPointer(reinterpret_cast<const void*>(system), facts::kIconSystemManager);
    const auto* slots   = static_cast<const std::byte*>(ReadPointer(manager, facts::kIconManagerSlots));
    if (manager == nullptr || slots == nullptr)
    {
        marker.text = L"no field icons";
        return marker;
    }
    int shown = 0;
    platform::ForEachSparseElement(
        reinterpret_cast<std::uintptr_t>(manager) + facts::kIconManagerKeys, facts::kIconKeyStride,
        [&](std::uintptr_t key) {
            const auto index = ReadField<std::int32_t>(reinterpret_cast<const void*>(key), facts::kIconKeyIndex);
            if (index < 0 || index >= kIconSlotsMax)
            {
                return true;
            }
            const void* icon = ReadPointer(slots + static_cast<std::size_t>(index) * facts::kIconSlotStride, facts::kIconSlotObject);
            const auto  kind = ReadField<std::uint8_t>(icon, facts::kIconObjectKind);
            if (icon == nullptr || ReadField<std::uint8_t>(icon, facts::kIconObjectShown) != 1 ||
                (kind != facts::kIconKindMainStory && kind != facts::kIconKindMainStoryIntermediate))
            {
                return true;
            }
            const void* row = ReadPointer(icon, facts::kIconObjectRow);
            if (row == nullptr || ReadField<std::uint8_t>(row, facts::kIconRowStateType) != facts::kIconStateTypeStory ||
                ReadField<std::uint8_t>(row, facts::kIconRowObjectType) != facts::kIconObjectTypeLevelObject)
            {
                return true;
            }
            const auto target = ReadField<std::uint64_t>(row, facts::kIconRowObjectId);
            ++shown;
            marker.onMember = marker.onMember || (object != 0 && target == object);
            if (describe)
            {
                marker.text += std::format(L"{}{} on {}", shown == 1 ? L"" : L", ",
                                           Name(ReadField<std::uint64_t>(reinterpret_cast<const void*>(key), 0)), Name(target));
            }
            return true;
        },
        kIconBitsMax);
    if (shown == 0)
    {
        marker.text = L"no story marker on a level object";
    }
    return marker;
}

bool InstallChecked(const platform::GameImage& image)
{
    const auto stateRow   = platform::FindFunction(image, facts::kStateChangeRow);
    const auto triggerRow = platform::FindFunction(image, facts::kStateTriggerRow);
    const auto collect    = platform::FindFunction(image, facts::kCollectActiveSlotKeys);
    const auto slot       = platform::FindFunction(image, facts::kObjectSlotByKey);
    const auto aiSystem   = platform::FindFunction(image, facts::kAiSystem);
    const auto freeArray  = platform::FindFunction(image, facts::kFreeArrayData);
    const auto lock       = image.Rebase(facts::kStateTriggerLock, sizeof(CRITICAL_SECTION));
    const auto root       = image.Rebase(facts::kEndGameRoot, sizeof(std::uintptr_t));
    const auto icons      = image.Rebase(facts::kFieldIconSystem, sizeof(std::uintptr_t));
    if (!stateRow || !triggerRow || !collect || !slot || !aiSystem || !freeArray || !lock || !root || !icons ||
        !LooksLikeCriticalSection(reinterpret_cast<const CRITICAL_SECTION*>(*lock)))
    {
        platform::Log(L"lead gate: an engine check failed; every walk or lead a level writes on player 2's or 3's character "
                      L"goes to its AI");
        return false;
    }
    g_stateRow   = reinterpret_cast<AccessorFn>(*stateRow);
    g_triggerRow = reinterpret_cast<AccessorFn>(*triggerRow);
    g_collect    = reinterpret_cast<CollectFn>(*collect);
    g_slot       = reinterpret_cast<SlotFn>(*slot);
    g_aiSystem   = reinterpret_cast<AiSystemFn>(*aiSystem);
    g_freeArray  = reinterpret_cast<FreeArrayFn>(*freeArray);
    g_lock       = reinterpret_cast<CRITICAL_SECTION*>(*lock);
    g_root       = reinterpret_cast<const std::uintptr_t*>(*root);
    g_icons      = reinterpret_cast<const std::uintptr_t*>(*icons);
#if LSS_DEV
    control::Register({.name  = L"lead.gate",
                       .shape = control::Shape::None,
                       .help  = L"(read only) the last walk or lead a level wrote on player 2's or 3's character, judged: its arrival "
                                L"trigger, the level's waiters, the story's marker, the character's own states ahead, and the "
                                L"verdict (guide: to its AI; staging: the player's)",
                       .get   = [] { return control::Value{Reading()}; }});
    control::Register({.name  = L"lead.path",
                       .shape = control::Shape::Text,
                       .help  = L"walks a level state's chain as the lead gate does, from the state named (read only)",
                       .set   = [](const control::Value& value) {
                           const std::wstring* state = std::get_if<std::wstring>(&value);
                           if (state == nullptr || state->empty())
                           {
                               return control::Outcome{false, L"name a level state"};
                           }
                           try
                           {
                               const std::wstring text = PathAhead(*state).text;
                               platform::Log(L"lead gate: lead.path {}: {}", *state, text);
                               return control::Outcome{true, text};
                           }
                           catch (...)
                           {
                               return control::Outcome{false, L"the walk raised an exception"};
                           }
                       }});
#endif
    g_installed = true;
    platform::Log(L"lead gate: installed; a walk or lead a level writes on player 2's or 3's character goes to its AI when a level "
                  L"step waits on its arrival, the story's marker rides on the character, or a further move is ahead in its own "
                  L"chain of states; otherwise the player keeps the character");
    return true;
}

}  // namespace

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

Verdict Judge(int seat, const void* member) noexcept
try
{
    Move move;
    if (!g_installed || !MoveOf(member, move) || move.object == 0 || move.row == 0)
    {
        return Verdict::Guide;  // unread: the AI walks it, as without the gate
    }
    if (seat >= 0 && seat < static_cast<int>(g_seatObject.size()))
    {
        g_seatObject[static_cast<std::size_t>(seat)] = move.object;
    }
    Judgment&  last = JudgmentOf(move.object);
    const bool same = last.object == move.object && last.row == move.row && last.trigger == move.trigger;
    if (same && lead_keep_rules::ToAi(last.verdict))
    {
        return last.verdict;  // a guide holds until the setting ends (its state changes)
    }
    if (!same)
    {
        last = Judgment{.object = move.object, .row = move.row, .trigger = move.trigger, .touched = last.touched};
    }
    ++last.calls;
    const bool census = !same || last.calls - last.censusCall >= kCensusEveryCalls;
    Census     waiters;
    if (census)
    {
        const auto start = std::chrono::steady_clock::now();
        waiters          = Waiters(move);
        waiters.text += std::format(L" [read in {} us]",
                                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        last.censusCall = last.calls;
        last.waiter     = !waiters.read || waiters.others > 0 || waiters.chain;
    }
    const bool    wasMarker = last.marker;
    const Verdict was       = last.verdict;
    Marker        marker    = Markers(move.object, false);
    last.marker             = marker.onMember;
    // No marker of the mod's is shown for a kept move: every needed move is the AI's.
    last.verdict = lead_keep_rules::Decide(last.waiter || last.marker, false, false, false, false);
    if (!same || census || last.marker != wasMarker || last.verdict != was)
    {
        marker    = Markers(move.object, true);
        g_reading = std::format(L"{}'s {} on object {} state {}: {}; marker {}: {}; verdict {}", players::SeatName(seat),
                                move.state == facts::kAiStateLead ? L"LEAD" : L"MOVE_TO", Name(move.object), Name(move.row),
                                census ? waiters.text : std::wstring(L"waiters as before"),
                                last.marker ? L"on the character" : L"elsewhere", marker.text,
                                lead_keep_rules::Name(last.verdict));
        if (!same || last.verdict != was)
        {
            platform::Log(L"lead gate: {}", g_reading);
        }
    }
    return last.verdict;
}
catch (...)
{
    platform::Log(L"lead gate: the judgment raised an exception; the walk goes to the character's AI");
    return Verdict::Guide;
}

void Forget(int seat) noexcept
{
    if (seat < 0 || seat >= static_cast<int>(g_seatObject.size()))
    {
        return;
    }
    std::uint64_t& object = g_seatObject[static_cast<std::size_t>(seat)];
    for (Judgment& j : g_judged)
    {
        if (object != 0 && j.object == object)
        {
            j = Judgment{.touched = j.touched};
        }
    }
    object = 0;
}

std::wstring Reading() noexcept
{
    return g_reading;
}

}  // namespace lss::session::lead_gate
