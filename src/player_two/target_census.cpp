#include "player_two/target_census.h"

#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/describe.h"
#include "player_two/equal_targeting.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/lock_group_coin.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>

namespace lss::player_two::target_census {

namespace {

// Facts read from the database (RE-p2-enemy-target-bias.md §8, S-TGTCENSUS; this build). 1.005: the
// replaced-target-census.json rows (16, read from both exes); every site fits one 16-byte block in both builds.

// Creation: EnemyChar_InitTargetSelector's call of void Create(holder*), its only caller. Init runs at every battle start;
// Create returns at once when the holder already has a selector, so a new selector is "holder+8 null before, set after".
constexpr std::uint8_t kCreateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                         0x24, 0x18, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kCreate{L"EnemyTargetSel_Create_ByCharaSpecType", 0x1414F4908, kCreateBytes};
constexpr std::uintptr_t         kCreateCall = 0x1413D8397;

// Each decision: Decide(selector, u8 reason, u8 lock-check argument); not virtual, 6 callers (two cross a block in 1.005,
// one is a tail jump), so an entry redirect. First instruction mov [rsp+10h],rbx.
constexpr std::uint8_t kDecideBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                         0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
constexpr std::uintptr_t kDecide      = 0x1407FEB68;
constexpr std::size_t    kDecideFirst = 5;

// Each kind's select, bool(selector, u8 reason, UObject** out), in its vtable's slot +0x88 (the out is written on
// every path; 0 from the Condition kind keeps the current target). The Condition kind's slot is player_two/
// equal_targeting's (it holds the answer there and tells this census); the Hate and None slots are watched here.
constexpr std::uint8_t kHateSelectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kCondSelectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kNoneSelectBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                             0x5C, 0x4E, 0x3F, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr std::uint32_t kSelectSlot = 0x88;

enum class Kind : std::uint8_t
{
    Hate,
    Condition,
    None,
    Unknown,
};
constexpr const wchar_t* kKindNames[] = {L"Hate", L"Condition", L"None", L"unknown"};
constexpr std::size_t    kKinds       = 3;  // the kinds told apart

constexpr platform::SlotFact kSelectSlots[kKinds] = {
    {L"the Hate target selector's select slot (vt+0x88)", 0x145CB40B0,
     {L"HateTargetSel_Select_Vt88", 0x140FA729C, kHateSelectBytes}},
    {L"the Condition target selector's select slot (vt+0x88)", 0x14580C260,
     {L"CondTargetSel_Select_Vt88", 0x140FA5230, kCondSelectBytes}},  // its vtable only: the slot is equal_targeting's
    {L"the None target selector's select slot (vt+0x88)", 0x14580C318,
     {L"NoneTargetSel_Select_Vt88", 0x140CBA0C8, kNoneSelectBytes}},
};

// The Condition select's one call of ApplyExclusion(unused, candidates, &entry->exclusions): once per priority group that
// draws; the drawn element is *(Element**)(r8 - 0x10). The function's own entry (3 bytes) is not hookable.
constexpr std::uint8_t kExclusionBytes[] = {0x45, 0x33, 0xD2, 0x45, 0x33, 0xC9, 0x49, 0x83,
                                            0x78, 0x10, 0x00, 0x49, 0x8B, 0xC0, 0x49, 0x0F};
constexpr platform::FunctionFact kExclusion{L"CondTargetSel_ApplyExclusion", 0x140FA5FE0, kExclusionBytes};
constexpr std::uintptr_t         kExclusionCall = 0x140FA5570;

// Called, not hooked: the player type hate is credited to (u8; 14 guest, 16 none), and the target the enemy's control
// component holds, which is what ApplyTarget wrote (after the Cait Sith decoy).
constexpr std::uint8_t kHateTypeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                           0x00, 0x00, 0x00, 0xE8, 0x40, 0x34, 0x22, 0x01};
constexpr platform::FunctionFact kHateType{L"Character_GetHatePlayerType", 0x140E8BAE0, kHateTypeBytes};
constexpr std::uint8_t kAppliedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                          0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
constexpr platform::FunctionFact kApplied{L"EnemyCtrl_GetCtrlCompTarget", 0x140A5F41C, kAppliedBytes};

// Holder (ctrl+0xBF8 points to it; the creation call hands it over): +0 the enemy AI controller, +8 the selector.
constexpr std::uint32_t kHolderSelector = 0x8;
// Enemy AI controller (same offsets in both builds).
constexpr std::uint32_t kCtrlEnemy      = facts::kEnemyControllerCharacter;  // FWeakObjectPtr: the enemy character
constexpr std::uint32_t kCtrlConditions = 0xB68;  // TMap<u8 Category, {TArray<Element>}> TargetSelectConditions
// Selector (base layout, both builds).
constexpr std::uint32_t kSelController   = 0x08;
constexpr std::uint32_t kSelLocks        = 0xB8;   // TArray<12-B slot, inline 10>
constexpr std::uint32_t kSelLocksHeap    = 0x130;
constexpr std::uint32_t kSelLocksNum     = 0x138;
constexpr std::uint32_t kLockSlotSize    = 12;     // +0 FWeakObjectPtr target, +8 u8 force, +9 u8 validity
constexpr std::uint32_t kSelRedirectFrom = 0x160;  // FWeakObjectPtr pair: from +0x160 to +0x168
constexpr std::uint32_t kSelRedirectTo   = 0x168;
constexpr std::uint32_t kSelSelected     = 0x174;  // FWeakObjectPtr: the select's result (or the starting target)
constexpr std::uint32_t kSelFinal        = 0x17C;  // FWeakObjectPtr: after locks and the redirect, before the decoy
// Hate selector: TSparseArray of 72-B pairs, inline 15; +0 u8 type, +8 int hate (-1 not engaged), +0x10 TArray<u8,
// inline 8> flags (heap +0x18); the chosen type afterwards at +0x620.
constexpr std::uint32_t kHateInline = 0x188, kHateHeap = 0x5C0, kHateNum = 0x5C8, kHatePair = 72, kHateChosen = 0x620;
// Condition selector: the history of picks, u8 last kind (0 UCPC, 1 AIPC, 3 none) and int run count.
constexpr std::uint32_t kCondLastKind = 0x1E0, kCondRunCount = 0x1E4;
// Party manager: the leader (FWeakObjectPtr), player 1's character.
constexpr std::uint32_t kPartyLeader = 0x148;
// Lock group (a TMap value; its FName key is 8 bytes before it). +0x25 is bIgnoreProvocation: the strong lock slot (7)
// when set, else the weak one (1); each member's priority is in the member (RE §18.4).
constexpr std::int32_t  kGroupName = -8;
constexpr std::uint32_t kGroupMembersNum = 0x08, kGroupMaxEach = 0x20, kGroupPrioritize = 0x24, kGroupStrong = 0x25;

constexpr std::uint8_t kLastPartyType = 13;  // party types 0..13; 14 guest, 16 none
constexpr std::int32_t kListCap       = 64;  // more entries than this in a read list: not the layout we know
constexpr const wchar_t* kLockNames[] = {L"Dispersion",    L"LockGroupWeak", L"BluePrint",       L"AbilityLockLine",
                                         L"AbilityLock",   L"AbilityLockCheck", L"HateLockLine", L"LockGroupStrong",
                                         L"ForceLock",     L"ForceLockCheck"};
constexpr const wchar_t* kCharacterTypes[] = {L"UCPC", L"AIPC", L"ALL"};
constexpr auto           kSummaryEvery     = std::chrono::seconds(60);

// Every wrapped function takes at most four integer arguments and no float (§8 listings); all four are passed on.
using Fn         = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using HateTypeFn = std::uint8_t (*)(void* character);
using AppliedFn  = void* (*)(void* controller);

// Set once at install.
platform::CallRedirect  g_createCall, g_exclusionCall;
platform::EntryRedirect g_decide;
platform::VTableEntry   g_selectSlots[kKinds];
std::uintptr_t          g_vtables[kKinds]{};  // each kind's live vtable (its select slot - 0x88)
std::uintptr_t          g_partyManager = 0;   // the pointer global's live address
HateTypeFn              g_hateType     = nullptr;
AppliedFn               g_applied      = nullptr;

/// Whose character a target is.
enum class Whose : std::uint8_t
{
    PlayerOne,
    PlayerTwo,
    AiParty,
    Other,
    Nobody,
};
constexpr const wchar_t* kWhoseNames[] = {L"player 1's character", L"player 2's member", L"an AI party member",
                                          L"another actor", L"nobody"};
constexpr std::size_t kWhose = std::size(kWhoseNames);

/// What chose a decision's target.
enum class Stage : std::uint8_t
{
    Select,
    Lock,
    Redirect,
    Decoy,
    Kept,
};
constexpr const wchar_t* kStageNames[] = {L"select", L"lock", L"redirect", L"decoy", L"kept"};
constexpr std::size_t    kStages       = std::size(kStageNames);

/// The counts for one summary window (any thread: relaxed atomics, read with exchange).
struct Tally
{
    std::atomic<std::uint32_t> decisions{0}, offGameThread{0};
    std::atomic<std::uint32_t> whose[kWhose]{};
    std::atomic<std::uint32_t> stages[kStages]{};
    std::atomic<std::uint32_t> selectWhose[kKinds][kWhose]{};  // picks chosen by each kind's select
    std::atomic<std::uint32_t> draws[3]{};                     // Condition draws by element class: UCPC, AIPC, ALL
    std::atomic<std::uint32_t> groupPicks{0}, prioritizedWhose[kWhose]{};
    std::atomic<std::uint32_t> selectors[kKinds + 1]{};  // new selectors by kind
    std::atomic<std::uint32_t> ucpc[3][4]{};  // Condition selects that read the UCPC: by ucpc_choice::Ucpc and Why
};
Tally                     g_tally;
std::atomic<std::int64_t> g_windowStart{0};  // steady_clock ticks; 0: no window yet

/// One decision in progress on this thread: what its select reported. Decide can nest (ApplyTarget's events can
/// request a reselect), so a small stack, matched by selector.
struct Frame
{
    void*        selector     = nullptr;
    Kind         kind         = Kind::Unknown;
    bool         selected     = false;  // the select ran
    std::uint8_t selectResult = 0;
    void*        selectOut    = nullptr;
    equal_targeting::Answer ucpc;      // Condition: who it counted as "the UCPC"
    std::uint8_t draws        = 0;
    std::uint8_t drawClass[4]{};       // Condition: each drawn element's CharacterType, ConditionType
    std::uint8_t drawCondition[4]{};
};
constexpr int          kDepth = 4;
thread_local Frame     t_frames[kDepth];
thread_local int       t_depth = 0;

Frame* FrameFor(const void* selector) noexcept
{
    for (int i = (t_depth < kDepth ? t_depth : kDepth) - 1; i >= 0; --i)
    {
        if (t_frames[i].selector == selector)
        {
            return &t_frames[i];
        }
    }
    return nullptr;
}

void* PartyManager() noexcept
{
    return ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0);
}

Kind KindOf(const void* selector) noexcept
{
    const auto vtable = VTableOf(selector);
    for (std::size_t i = 0; i < kKinds; ++i)
    {
        if (vtable != 0 && vtable == g_vtables[i])
        {
            return static_cast<Kind>(i);
        }
    }
    return Kind::Unknown;
}

void* Weak(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? nullptr : leader_scope::ObjectAt(static_cast<const std::byte*>(object) + offset);
}

/// The player type hate is credited to (16 if it cannot be read).
std::uint8_t TypeOf(void* character) noexcept
{
    std::uint8_t type = 16;
    if (character == nullptr || g_hateType == nullptr || !platform::CallGuarded(g_hateType, type, character))
    {
        return 16;
    }
    return type;
}

Whose WhoseIs(void* target, const void* leader) noexcept
{
    if (target == nullptr)
    {
        return Whose::Nobody;
    }
    const void* member = generation::Member();
    if (member != nullptr && target == member)
    {
        return Whose::PlayerTwo;
    }
    if (target == leader)
    {
        return Whose::PlayerOne;
    }
    return TypeOf(target) <= kLastPartyType ? Whose::AiParty : Whose::Other;
}

void Count(std::atomic<std::uint32_t>& count) noexcept
{
    count.fetch_add(1, std::memory_order_relaxed);
}

/// The summary line, once per window with decisions (whichever thread decides first after the window ends).
void MaybeSummarize() noexcept
{
    const auto now   = std::chrono::steady_clock::now().time_since_epoch().count();
    auto       start = g_windowStart.load(std::memory_order_relaxed);
    if (start == 0)
    {
        g_windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    const auto every = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kSummaryEvery).count();
    if (now - start < every || !g_windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed))
    {
        return;
    }
    Tally& t = g_tally;
    std::uint32_t whose[kWhose]{}, stages[kStages]{}, kinds[kKinds][kWhose]{}, prioritized[kWhose]{};
    for (std::size_t i = 0; i < kWhose; ++i)
    {
        whose[i]       = platform::Take(t.whose[i]);
        prioritized[i] = platform::Take(t.prioritizedWhose[i]);
        for (std::size_t k = 0; k < kKinds; ++k)
        {
            kinds[k][i] = platform::Take(t.selectWhose[k][i]);
        }
    }
    for (std::size_t i = 0; i < kStages; ++i)
    {
        stages[i] = platform::Take(t.stages[i]);
    }
    const auto decisions = platform::Take(t.decisions), off = platform::Take(t.offGameThread),
               picks = platform::Take(t.groupPicks);
    const auto ucpcDraws = platform::Take(t.draws[0]), aipc = platform::Take(t.draws[1]),
               all = platform::Take(t.draws[2]);
    const auto newHate = platform::Take(t.selectors[0]), newCond = platform::Take(t.selectors[1]),
               newNone = platform::Take(t.selectors[2]),
               newOther = platform::Take(t.selectors[3]);
    std::uint32_t ucpc[3][4]{};
    for (std::size_t u = 0; u < 3; ++u)
    {
        for (std::size_t w = 0; w < 4; ++w)
        {
            ucpc[u][w] = platform::Take(t.ucpc[u][w]);
        }
    }
    // Rows: Untouched, PlayerOne, PlayerTwo; columns: NoPlayerTwo, Coin, OnlyEligible, NeitherEligible.
    const auto untouched = ucpc[0][0] + ucpc[0][1] + ucpc[0][2] + ucpc[0][3];
    platform::Log(L"target census (last {} s): {} decision(s) ({} off the game thread); targets: player 1's character {}, "
                  L"player 2's member {}, AI party {}, other {}, none {}; chosen by select {}, lock {}, redirect {}, "
                  L"decoy {}, kept {}; by kind (P1/P2/AI): Hate {}/{}/{}, Condition {}/{}/{}, None {}/{}/{}; Condition "
                  L"draws UCPC {}, AIPC {}, ALL {}; Condition UCPC: player 1 by coin {}, alone eligible {}, neither "
                  L"eligible {}, player 2 by coin {}, alone eligible {}, game's own {}; lock-group picks {} (prioritising "
                  L"the UCPC: P1 {}, P2 {}, AI {}); new selectors Hate {}, Condition {}, None {}, unknown {}",
                  std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration(now - start)).count(),
                  decisions, off, whose[0], whose[1], whose[2], whose[3], whose[4], stages[0], stages[1], stages[2],
                  stages[3], stages[4], kinds[0][0], kinds[0][1], kinds[0][2], kinds[1][0], kinds[1][1], kinds[1][2],
                  kinds[2][0], kinds[2][1], kinds[2][2], ucpcDraws, aipc, all, ucpc[1][1], ucpc[1][2], ucpc[1][3],
                  ucpc[2][1], ucpc[2][2], untouched, picks, prioritized[0], prioritized[1],
                  prioritized[2], newHate, newCond, newNone, newOther);
}

// ---- Creation: the census of one selector ----

/// One TargetSelectConditions element (0x14 bytes, both builds).
struct Element
{
    std::uint8_t  enable;
    std::uint8_t  pad0[3];
    std::int32_t  priority;
    std::uint8_t  characterType;  // 0 UCPC, 1 AIPC, 2 ALL
    std::uint8_t  conditionType;
    std::uint8_t  matchExclusion;
    std::uint8_t  pad1;
    std::int32_t  ratio;
    std::int32_t  continuousLimit;
};
static_assert(sizeof(Element) == 0x14);

/// An element's CharacterType: UCPC, AIPC, ALL, or its number.
std::wstring ClassName(std::uint8_t type)
{
    return type < std::size(kCharacterTypes) ? kCharacterTypes[type] : L"class " + std::to_wstring(type);
}

/// The controller's TargetSelectConditions: element counts by class (enabled only), each element at Debug.
std::wstring Conditions(const void* controller, const std::wstring& enemy) noexcept
{
    const auto* map   = static_cast<const std::byte*>(controller) + kCtrlConditions;
    const auto* pairs = ReadPointer(map, 0x0);
    const auto  num   = ReadField<std::int32_t>(map, 0x8);
    if (pairs == nullptr || num <= 0)
    {
        return L"no TargetSelectConditions";
    }
    if (num > kListCap)
    {
        return L"TargetSelectConditions unread (" + std::to_wstring(num) + L" pairs)";
    }
    // Allocation bits: inline at +0x10, or the heap pointer at +0x20.
    const auto*   heapBits = ReadPointer(map, 0x20);
    std::uint32_t counts[3]{}, disabled = 0, total = 0;
    for (std::int32_t p = 0; p < num; ++p)
    {
        const auto* words = heapBits != nullptr ? static_cast<const std::byte*>(heapBits) : map + 0x10;
        const auto  word  = ReadField<std::uint32_t>(words, static_cast<std::uint32_t>(p / 32) * 4);
        if ((word >> (p % 32) & 1) == 0)
        {
            continue;  // a free slot
        }
        const auto* pair     = static_cast<const std::byte*>(pairs) + static_cast<std::size_t>(p) * 32;
        const auto  category = ReadField<std::uint8_t>(pair, 0x0);
        const auto* elements = ReadPointer(pair, 0x8);
        const auto  count    = ReadField<std::int32_t>(pair, 0x10);
        for (std::int32_t e = 0; elements != nullptr && e < count && e < kListCap; ++e)
        {
            Element element{};
            if (!platform::CopyGuarded(&element, static_cast<const std::byte*>(elements) + e * sizeof(Element),
                                       sizeof(Element)))
            {
                break;
            }
            ++total;
            if (element.enable == 0)
            {
                ++disabled;
            }
            else if (element.characterType < 3)
            {
                ++counts[element.characterType];
            }
            platform::LogDebug(L"target census: {} condition (category {}): {}enabled, priority {}, {}, condition {}, "
                               L"match exclusion {}, ratio {}, continuous limit {}",
                               enemy, category, element.enable != 0 ? L"" : L"not ", element.priority,
                               ClassName(element.characterType), element.conditionType,
                               YesNo(element.matchExclusion != 0), element.ratio, element.continuousLimit);
        }
    }
    return std::to_wstring(total) + L" condition(s): UCPC " + std::to_wstring(counts[0]) + L", AIPC " +
           std::to_wstring(counts[1]) + L", ALL " + std::to_wstring(counts[2]) + L", disabled " + std::to_wstring(disabled);
}

std::uint64_t Create(std::uint64_t holder, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto* box    = reinterpret_cast<const void*>(holder);
    const bool  had    = ReadPointer(box, kHolderSelector) != nullptr;
    const auto  result = reinterpret_cast<Fn>(g_createCall.Original())(holder, b, c, d);
    const void* selector = had ? nullptr : ReadPointer(box, kHolderSelector);
    if (selector != nullptr)
    {
        const Kind kind = KindOf(selector);
        Count(g_tally.selectors[static_cast<std::size_t>(kind)]);
        const void*        controller = ReadPointer(box, 0);
        const std::wstring enemy      = NameOf(Weak(controller, kCtrlEnemy));
        platform::Log(L"target census: {} has a {} target selector; {}", enemy, kKindNames[static_cast<std::size_t>(kind)],
                      controller != nullptr ? Conditions(controller, enemy) : L"no controller");
    }
    return result;
}

// ---- Selects ----

/// The Hate table at Trace: type -> hate (flagged types marked).
void LogHateTable(const void* selector) noexcept
{
    if (!platform::Enabled(platform::Level::Trace))
    {
        return;
    }
    const auto* heap  = ReadPointer(selector, kHateHeap);
    const auto* pairs = heap != nullptr ? static_cast<const std::byte*>(heap) : static_cast<const std::byte*>(selector) + kHateInline;
    const auto  num   = ReadField<std::int32_t>(selector, kHateNum);
    std::wstring table;
    for (std::int32_t i = 0; i < num && i < 16; ++i)
    {
        const auto* pair = pairs + static_cast<std::size_t>(i) * kHatePair;
        const auto  hate = ReadField<std::int32_t>(pair, 0x8);
        if (hate < 0)
        {
            continue;  // not engaged
        }
        const auto* flags = ReadPointer(pair, 0x18);
        const auto  flag  = ReadField<std::uint8_t>(flags != nullptr ? flags : pair + 0x10, 0);
        table += std::format(L" {}:{}{}", ReadField<std::uint8_t>(pair, 0), hate, flag != 0 ? L"!" : L"");
    }
    platform::LogTrace(L"target census: hate table (type:hate, ! flagged):{}", table.empty() ? L" none engaged" : table);
}

/// What a select reported, into the decision's frame (if one is open for the selector on this thread).
void NoteSelect(void* selector, Kind kind, std::uint64_t out, std::uint64_t result) noexcept
{
    if (Frame* const frame = FrameFor(selector); frame != nullptr)
    {
        frame->kind         = kind;
        frame->selected     = true;
        frame->selectResult = static_cast<std::uint8_t>(result);
        frame->selectOut    = out != 0 ? ReadPointer(reinterpret_cast<const void*>(out), 0) : nullptr;
    }
}

/// The Hate and None selects' slots: the game's select, passed through, and what it reported.
template <Kind K>
std::uint64_t Select(std::uint64_t selector, std::uint64_t reason, std::uint64_t out, std::uint64_t d) noexcept
{
    auto* const self     = reinterpret_cast<void*>(selector);
    const auto  original = reinterpret_cast<Fn>(g_selectSlots[static_cast<std::size_t>(K)].Original());
    if constexpr (K == Kind::Hate)
    {
        LogHateTable(self);
    }
    const std::uint64_t result = original(selector, reason, out, d);
    NoteSelect(self, K, out, result);
    return result;
}

/// The Condition select, as equal_targeting ran it: who it counted as the UCPC, held for the game's select only.
void OnConditionSelect(void* selector, std::uint64_t, std::uint64_t out, std::uint64_t result,
                       const equal_targeting::Answer& ucpc) noexcept
{
    if (ucpc.read)
    {
        Count(g_tally.ucpc[static_cast<std::size_t>(ucpc.choice.ucpc)][static_cast<std::size_t>(ucpc.choice.why)]);
    }
    if (Frame* const frame = FrameFor(selector); frame != nullptr)
    {
        frame->ucpc = ucpc;
    }
    NoteSelect(selector, Kind::Condition, out, result);
}

/// The Condition select's draw: the element whose exclusions narrow the candidates.
std::uint64_t Exclusion(std::uint64_t a, std::uint64_t candidates, std::uint64_t exclusions, std::uint64_t d) noexcept
{
    const auto* element = exclusions > 0x10 ? ReadPointer(reinterpret_cast<const void*>(exclusions - 0x10), 0) : nullptr;
    if (element != nullptr && t_depth > 0 && t_depth <= kDepth)
    {
        Frame&     frame = t_frames[t_depth - 1];
        const auto type  = ReadField<std::uint8_t>(element, 0x8);
        if (type < 3)
        {
            Count(g_tally.draws[type]);
        }
        if (frame.draws < std::size(frame.drawClass))
        {
            frame.drawClass[frame.draws]     = type;
            frame.drawCondition[frame.draws] = ReadField<std::uint8_t>(element, 0x9);
            ++frame.draws;
        }
    }
    return reinterpret_cast<Fn>(g_exclusionCall.Original())(a, candidates, exclusions, d);
}

// ---- Each decision ----

/// The highest lock slot whose target is `target` (-1 if none).
int LockHolding(const void* selector, const void* target) noexcept
{
    const auto* heap  = ReadPointer(selector, kSelLocksHeap);
    const auto* slots = heap != nullptr ? static_cast<const std::byte*>(heap) : static_cast<const std::byte*>(selector) + kSelLocks;
    const auto  num   = ReadField<std::int32_t>(selector, kSelLocksNum);
    for (int i = (num < 10 ? num : 10) - 1; i >= 0; --i)
    {
        if (Weak(slots, static_cast<std::uint32_t>(i) * kLockSlotSize) == target)
        {
            return i;
        }
    }
    return -1;
}

/// Who a Condition select counted as the UCPC, for the decision's line.
std::wstring UcpcText(const equal_targeting::Answer& answer)
{
    using ucpc_choice::Ucpc;
    using ucpc_choice::Why;
    if (!answer.read)
    {
        return L"no candidates built";
    }
    if (answer.choice.ucpc == Ucpc::Untouched)
    {
        return L"the game's own (no player 2 member)";
    }
    constexpr const wchar_t* kWhy[] = {L"", L"coin", L"only one eligible", L"neither eligible"};
    return std::format(L"{} type {} ({})", answer.choice.ucpc == Ucpc::PlayerTwo ? L"player 2's member" : L"player 1's",
                       answer.type, kWhy[static_cast<std::size_t>(answer.choice.why)]);
}

std::wstring Draws(const Frame& frame) noexcept
{
    std::wstring text;
    for (std::uint8_t i = 0; i < frame.draws; ++i)
    {
        text += std::format(L"{}{} condition {}", i == 0 ? L"" : L", ", ClassName(frame.drawClass[i]), frame.drawCondition[i]);
    }
    return text.empty() ? L"none" : text;
}

std::uint64_t Decide(std::uint64_t selector, std::uint64_t reason, std::uint64_t arg, std::uint64_t d) noexcept
{
    auto* const self = reinterpret_cast<void*>(selector);
    const bool  push = t_depth < kDepth;
    if (push)
    {
        t_frames[t_depth] = Frame{self, KindOf(self)};
    }
    ++t_depth;
    const auto result = reinterpret_cast<Fn>(g_decide.Original())(selector, reason, arg, d);
    --t_depth;
    const Frame frame = push ? t_frames[t_depth] : Frame{self, KindOf(self)};

    void* controller = ReadPointer(self, kSelController);
    void* final      = Weak(self, kSelFinal);
    void* applied    = nullptr;
    if (controller == nullptr || g_applied == nullptr || !platform::CallGuarded(g_applied, applied, controller))
    {
        applied = final;
    }
    const void* leader = Weak(PartyManager(), kPartyLeader);
    // The stage: the decoy changed the applied target; else the select's pick stood; else the redirect pair or the
    // highest lock slot holding it; else the current target was kept (no select, reasons 3 and 4, or a Condition 0).
    Stage stage = Stage::Kept;
    int   lock  = -1;
    if (applied != final)
    {
        stage = Stage::Decoy;
    }
    else if (frame.selected && frame.selectResult != 0 && final == frame.selectOut)
    {
        stage = Stage::Select;
    }
    else if (final != nullptr && final == Weak(self, kSelRedirectTo) && Weak(self, kSelRedirectFrom) != nullptr)
    {
        stage = Stage::Redirect;
    }
    else if ((lock = LockHolding(self, final)) >= 0)
    {
        stage = Stage::Lock;
    }
    const Whose whose   = WhoseIs(applied, leader);
    const bool  onGame  = generation::OnGameThread();
    Tally&      t       = g_tally;
    Count(t.decisions);
    Count(t.whose[static_cast<std::size_t>(whose)]);
    Count(t.stages[static_cast<std::size_t>(stage)]);
    if (stage == Stage::Select && frame.kind != Kind::Unknown)
    {
        Count(t.selectWhose[static_cast<std::size_t>(frame.kind)][static_cast<std::size_t>(whose)]);
    }
    if (!onGame)
    {
        Count(t.offGameThread);
    }
    if (platform::Enabled(platform::Level::Debug))
    {
        std::wstring detail;
        if (frame.kind == Kind::Hate && frame.selected)
        {
            detail = std::format(L"; Hate chose type {}", ReadField<std::uint8_t>(self, kHateChosen));
        }
        else if (frame.kind == Kind::Condition && frame.selected)
        {
            detail = std::format(L"; Condition {} (UCPC: {}; draws: {}; history: last {} x{})",
                                 frame.selectResult != 0 ? L"picked" : L"kept the current target", UcpcText(frame.ucpc),
                                 Draws(frame), ReadField<std::uint8_t>(self, kCondLastKind),
                                 ReadField<std::int32_t>(self, kCondRunCount));
        }
        platform::LogDebug(L"target census: {} ({}) reason {}: {} (type {}) by {}{}{}; thread {}{}", NameOf(Weak(controller, kCtrlEnemy)),
                           kKindNames[static_cast<std::size_t>(frame.kind)], reason & 0xFF,
                           kWhoseNames[static_cast<std::size_t>(whose)], TypeOf(applied),
                           kStageNames[static_cast<std::size_t>(stage)],
                           stage == Stage::Lock ? std::format(L" {} ({})", lock, kLockNames[lock]) : std::wstring{},
                           detail, GetCurrentThreadId(), onGame ? L" (game)" : L"");
    }
    MaybeSummarize();
    return result;
}

// ---- Lock groups ----

/// A lock-group pick, as lock_group_coin ran it (the picking enemy's coin named "the player"): the result logged.
void OnPick(const void* group, std::uint64_t result, std::uint64_t out, bool first) noexcept
{
    const auto* self = static_cast<const std::byte*>(group);
    if (self == nullptr || (result & 0xFF) == 0)
    {
        return;
    }
    void*       picked      = out != 0 ? leader_scope::ObjectAt(reinterpret_cast<const void*>(out)) : nullptr;
    const Whose whose       = WhoseIs(picked, Weak(PartyManager(), kPartyLeader));
    const bool  prioritized = ReadField<std::uint8_t>(self, kGroupPrioritize) != 0;
    Count(g_tally.groupPicks);
    if (prioritized)
    {
        Count(g_tally.prioritizedWhose[static_cast<std::size_t>(whose)]);
    }
    if (platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"target census: lock group {} ({}; prioritise the UCPC {}, {} member(s), at most {} each, "
                           L"strong lock {}) picked {} (type {}); thread {}",
                           players::unreal::NameText(ReadField<std::uint64_t>(self + kGroupName, 0)),
                           first ? L"first pick" : L"re-pick", YesNo(prioritized),
                           ReadField<std::int32_t>(self, kGroupMembersNum), ReadField<std::int32_t>(self, kGroupMaxEach),
                           ReadField<std::uint8_t>(self, kGroupStrong), kWhoseNames[static_cast<std::size_t>(whose)],
                           TypeOf(picked), GetCurrentThreadId());
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto find  = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact).value_or(0); };
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    g_partyManager     = manager ? *manager : 0;
    g_hateType         = reinterpret_cast<HateTypeFn>(find(kHateType));
    g_applied          = reinterpret_cast<AppliedFn>(find(kApplied));
    if (g_partyManager == 0 || g_hateType == nullptr || g_applied == nullptr || !leader_scope::Install(image))
    {
        platform::Log(L"target census: UNAVAILABLE (a check failed); enemies' targeting is not logged");
        return;
    }
    // The selects first, the decision last: a decision never runs before the selects can report to it. The Condition
    // kind's vtable is read for KindOf; its slot is equal_targeting's, which reports through the observer.
    std::size_t selects = 0;
    for (std::size_t i = 0; i < kKinds; ++i)
    {
        static constexpr Fn kSelects[kKinds] = {&Select<Kind::Hate>, nullptr, &Select<Kind::None>};
        const auto slot = image.Rebase(kSelectSlots[i].slot, sizeof(void*));
        if (!slot)
        {
            continue;
        }
        g_vtables[i] = *slot - kSelectSlot;
        if (kSelects[i] != nullptr &&
            platform::ReplaceSlot(image, kSelectSlots[i],
                                  reinterpret_cast<std::uintptr_t>(kSelects[i]), g_selectSlots[i]))
        {
            ++selects;
        }
    }
    equal_targeting::SetSelectObserver(&OnConditionSelect);
    lock_group_coin::SetPickObserver(&OnPick);
    const bool draws    = g_exclusionCall.Install(image, kExclusionCall, find(kExclusion),
                                                  reinterpret_cast<std::uintptr_t>(&Exclusion));
    const bool creation = g_createCall.Install(image, kCreateCall, find(kCreate),
                                               reinterpret_cast<std::uintptr_t>(&Create));
    const auto decide   = image.Rebase(kDecide);
    const bool decisions =
        decide && image.BytesMatch(kDecide, kDecideBytes) &&
        g_decide.Install(*decide, image.EntryInstruction(kDecide, std::span(kDecideBytes).first(kDecideFirst)),
                         reinterpret_cast<std::uintptr_t>(&Decide));
    platform::Log(L"target census: decisions {}, Hate and None selects {} of 2, Condition draws {}, new selectors {}; the "
                  L"Condition selects and the lock-group picks reported by their owners",
                  decisions ? L"watched" : L"NOT watched", selects, draws ? L"watched" : L"NOT watched",
                  creation ? L"watched" : L"NOT watched");
}

void Remove() noexcept
{
    equal_targeting::SetSelectObserver(nullptr);
    lock_group_coin::SetPickObserver(nullptr);
    if (g_decide.IsInstalled() && !g_decide.Restore())
    {
        platform::Log(L"target census: the decision's entry was changed after us; left in place (it passes through)");
    }
    for (auto* call : {&g_createCall, &g_exclusionCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"target census: a call was changed after us; left in place (it passes through)");
        }
    }
    for (auto& slot : g_selectSlots)
    {
        if (slot.Original() != 0 && !slot.Restore())
        {
            platform::Log(L"target census: a select slot was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::target_census
