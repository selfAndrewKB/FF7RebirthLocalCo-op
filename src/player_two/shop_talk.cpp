#include "player_two/shop_talk.h"

#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/vending.h"
#include "players/unreal_access.h"

#include <Unreal/NameTypes.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::player_two::shop_talk {

namespace {

// 1.004 addresses (1.005 through replaced-shop-talk.json). RE-p2-shopkeeper-talk.md §1.2, §7.2.
constexpr std::uint8_t kRequestBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                          0x24, 0x10, 0x57, 0xB8, 0x50, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRequest{L"StateTrigger_Request", 0x140F7AA58, kRequestBytes};
constexpr std::uint8_t kAiEventBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55,
                                          0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC9};
constexpr platform::FunctionFact kAiEvent{L"ShopMenu_SendLeaderTargetAIEvent5", 0x14353E9C0, kAiEventBytes};
constexpr std::uint8_t kLeaderNameBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
                                             0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::FunctionFact kLeaderName{L"ResolveNamedPoint_LEADER_BATTLELEADER", 0x1408A95B8, kLeaderNameBytes};
constexpr std::uint8_t kFindObjectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54,
                                             0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kFindObject{L"GS_FindObjectByKeyAndAttribute", 0x14087AD50, kFindObjectBytes};
constexpr std::uint8_t kAccessorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};  // push rbx; mov eax, 20h
constexpr platform::FunctionFact kStateRow{L"DataAcc_StateChange_Ctor", 0x14087B56C, kAccessorBytes};
constexpr platform::FunctionFact kActionRow{L"DataAcc_Action_Ctor", 0x1407BEB9C, kAccessorBytes};
constexpr platform::FunctionFact kGroupRow{L"DataAcc_ActionGroup_Ctor", 0x1411CB32C, kAccessorBytes};
constexpr std::size_t kFirstInstruction = 5;  // each redirected entry starts with a 5-byte mov [rsp+n], reg
// DispatchKey's return address after its StateTrigger_Request call (the object branch, §2.1 step 6).
constexpr std::uintptr_t kDispatchReturn = 0x1414A8FB3;

// bool-ish(FName trigger, FName sender, float delay, u8 flags), all by value (§7 step 2).
using RequestFn  = std::uint64_t (*)(std::uint64_t trigger, std::uint64_t sender, float delay, std::uint8_t flags);
using Fn4        = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using FindFn     = void* (*)(void* gs, std::uint64_t key, std::int32_t attribute);
using AccessorFn = void* (*)(void* accessor, std::uint64_t rowName);  // fills {vtable, row, ...}; 32 bytes

// The GS (root+0x170), the talk attribute DispatchKey asks for, and the object's current StateChange name (§2.1).
constexpr std::uint32_t kRootGs        = 0x170;
constexpr std::int32_t  kTalkAttribute = 5;
constexpr std::uint32_t kObjectState   = 0x10;
// Memory-mapped data rows (RE-statechange-row-layout.md, the game's own field-type tables): an array or string field is
// {u64 pointer, or (signed offset << 1) | 1 from the field; i32 count}; a string's count includes its terminator.
constexpr std::uint32_t kAccessorRow   = 0x08;
constexpr std::uint32_t kStateNext     = 0;    // StateChange NextStateChange_Array (16-byte strings; empty: no change)
constexpr std::uint32_t kStateSwaps    = 32;   // StateChange SwapStateChange (FNames: states entered instead, by rule)
constexpr std::uint32_t kStateActions  = 48;   // StateChange EnterActionList_Array (FNames: Action or ActionGroup rows)
constexpr std::uint32_t kGroupActions  = 0;    // ActionGroup row: its Action FNames
constexpr std::uint32_t kActionInts    = 112;  // Action IntegerArgumentList (i32 elements), count at +120
constexpr std::uint32_t kActionType    = 280;  // Action Type (i32)
constexpr std::int32_t  kShowMenu      = 43;
constexpr std::int32_t  kShopKind      = 1;    // FEndShowShopMenu
constexpr std::int32_t  kSkillTreeKind = 7;    // FEndShowSkillTreeMenu (the magic and book shops)
constexpr std::int32_t  kMaxNext       = 16;
constexpr std::int32_t  kMaxSwaps      = 8;
constexpr std::int32_t  kMaxActions    = 8;   // GSObject_PostStateEnterActions reads at most 8
constexpr std::int32_t  kMaxGroup      = 32;  // GSObject_PostActionRequest queues at most 32 of a group
constexpr std::int32_t  kMaxText       = 127;

using Clock = std::chrono::steady_clock;
constexpr auto        kRecheck      = std::chrono::seconds(5);   // a key that is not a shop is read again after this
constexpr auto        kShopRecheck  = std::chrono::seconds(60);  // a shop's verdict is confirmed again after this
constexpr std::size_t kQueueSize    = 256;
constexpr int         kWalksPerTick = 32;
constexpr int         kLeaderLogs   = 6;  // LEADER_ resolutions logged per pending press

enum class Verdict : std::uint8_t
{
    NoObject,    // no talk object (most points)
    NotShop,     // its rows read, no shop action
    Shop,        // a type-43 action of kind 1 or 7
    ByName,      // the rows could not be read; its name contains "Shop" (the interim test, §1.2)
    Unreadable,  // the rows could not be read, no "Shop" in its name
};

struct Known
{
    Verdict           verdict = Verdict::NoObject;
    Clock::time_point queuedAt{};  // when it was last queued or read (a recheck waits kRecheck from it)
    Clock::time_point readAt{};    // when Tick last read it (queued copies within kDuplicate are skipped)
    bool              logged = false;
};
constexpr auto kDuplicate = std::chrono::seconds(1);

platform::EntryRedirect g_request, g_aiEvent, g_leaderName;
FindFn                  g_findObject = nullptr;
AccessorFn              g_stateRow   = nullptr;
AccessorFn              g_actionRow  = nullptr;
AccessorFn              g_groupRow   = nullptr;  // optional: without it a name is read as an Action row only
const std::uintptr_t*   g_gameRoot   = nullptr;
std::optional<platform::GameImage> g_image;
bool                    g_admitting  = false;  // the walk's functions were found

std::unordered_map<std::uint64_t, Known> g_known;  // by point key (game thread)
std::vector<std::uint64_t>               g_queue;  // reserved: Admits runs under the field manager's lock

std::uint64_t g_pressKey      = 0;  // the pending press, for the LEADER_ log
int           g_leaderLogged  = 0;
unsigned      g_leaderCalls   = 0;

bool IsAdmitted(Verdict verdict) noexcept
{
    return verdict == Verdict::Shop || verdict == Verdict::ByName;
}

const wchar_t* Text(Verdict verdict) noexcept
{
    switch (verdict)
    {
    case Verdict::NoObject: return L"no talk object";
    case Verdict::NotShop: return L"not a shop";
    case Verdict::Shop: return L"a shop";
    case Verdict::ByName: return L"a shop by its name (its rows could not be read)";
    case Verdict::Unreadable: return L"unreadable rows, not a shop by its name";
    }
    return L"?";
}

/// A mapped field's data (pointer, or a signed offset from the field when the low bit is set: shared strings point back).
const std::byte* MappedData(const std::byte* field) noexcept
{
    const auto value = ReadField<std::uint64_t>(field, 0);
    if (value == 0)
    {
        return nullptr;
    }
    return (value & 1) != 0 ? field + (static_cast<std::int64_t>(value) >> 1) : reinterpret_cast<const std::byte*>(value);
}

/// A mapped array's element count, at most `max` (0 if unreadable).
std::int32_t MappedCount(const std::byte* field, std::int32_t max) noexcept
{
    const auto count = ReadField<std::int32_t>(field, 8);
    return count > 0 ? std::min(count, max) : 0;
}

/// A mapped string (a 16-byte element): its text, or empty if it does not read as one.
std::wstring MappedText(const std::byte* element)
{
    const auto  count = ReadField<std::int32_t>(element, 8);
    const auto* data  = MappedData(element);
    if (data == nullptr || count < 2 || count > kMaxText + 1)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(count - 1), L'\0');
    if (!platform::CopyGuarded(text.data(), data, text.size() * sizeof(wchar_t)))
    {
        return {};
    }
    for (const wchar_t c : text)
    {
        if (c < 0x20 || c > 0x7E)
        {
            return {};  // row names are ASCII: anything else means the offset is wrong
        }
    }
    return text;
}

/// A mapped array of FNames (8-byte elements): at most `max`, up to the first None (as the game reads them).
std::vector<std::uint64_t> MappedNames(const std::byte* field, std::int32_t max)
{
    std::vector<std::uint64_t> names;
    const auto* data  = MappedData(field);
    const auto  count = MappedCount(field, max);
    for (std::int32_t i = 0; i < count && data != nullptr; ++i)
    {
        const auto name = ReadField<std::uint64_t>(data, 8 * static_cast<std::uint32_t>(i));
        if (name == 0)
        {
            break;
        }
        names.push_back(name);
    }
    return names;
}

/// A row by FName through the game's accessor (its row pointer, or null).
const std::byte* Row(AccessorFn accessor, std::uint64_t name)
{
    if (accessor == nullptr || name == 0)
    {
        return nullptr;
    }
    alignas(16) std::array<std::byte, 64> buffer{};
    void* filled = nullptr;
    if (!platform::CallGuarded(accessor, filled, static_cast<void*>(buffer.data()), name))
    {
        return nullptr;
    }
    return static_cast<const std::byte*>(ReadPointer(buffer.data(), kAccessorRow));
}

/// A row by its name's text (next states are named by strings): the FName is looked up, never added.
const std::byte* Row(AccessorFn accessor, const std::wstring& name)
{
    std::uint64_t value = 0;
    try
    {
        const RC::Unreal::FName fname(name.c_str(), RC::Unreal::FNAME_Find);
        value = fname.GetComparisonIndex().ToUnstableInt() | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fname.GetNumber())) << 32);
    }
    catch (...)
    {
        return nullptr;
    }
    return Row(accessor, value);
}

void* Gs() noexcept
{
    std::uintptr_t root = 0;
    if (g_gameRoot == nullptr || !platform::CopyGuarded(&root, g_gameRoot, sizeof(root)) || root == 0)
    {
        return nullptr;
    }
    return ReadPointer(reinterpret_cast<const void*>(root), kRootGs);
}

/// The game's data for `key` (§1.2), with what was read for the log.
Verdict Walk(std::uint64_t key, std::wstring& seen)
{
    void* gs     = Gs();
    void* object = nullptr;
    if (gs == nullptr || !platform::CallGuarded(g_findObject, object, gs, key, kTalkAttribute) || object == nullptr)
    {
        return Verdict::NoObject;
    }
    const auto state = ReadField<std::uint64_t>(object, kObjectState);
    if (state == 0)
    {
        return Verdict::NoObject;
    }
    const std::wstring keyText = players::unreal::NameText(key);
    const bool         named   = keyText.find(L"Shop") != std::wstring::npos;
    const auto unreadable      = [named] { return named ? Verdict::ByName : Verdict::Unreadable; };
    const std::byte*   row     = Row(g_stateRow, state);
    seen = std::format(L"state {}", players::unreal::NameText(state));
    if (row == nullptr)
    {
        seen += L" (no row)";
        return unreadable();
    }
    // Each next state (GS_ApplyTriggerToObject_NextState), with the states its swap rules may enter instead
    // (StateChange_ResolveSwapState); each one's enter actions (GSObject_PostStateEnterActions), a name standing for an
    // ActionGroup row's actions when it has one (GSObject_PostActionRequest).
    const auto* next  = MappedData(row + kStateNext);
    const auto  nexts = MappedCount(row + kStateNext, kMaxNext);
    for (std::int32_t n = 0; n < nexts && next != nullptr; ++n)
    {
        const std::byte* element = next + 16 * n;
        if (ReadField<std::int32_t>(element, 8) <= 1)
        {
            continue;  // an empty name: no change on that trigger
        }
        const std::wstring nextText = MappedText(element);
        const std::byte*   nextRow  = nextText.empty() ? nullptr : Row(g_stateRow, nextText);
        if (nextRow == nullptr)
        {
            seen += std::format(L", next {} unreadable", n);
            return unreadable();
        }
        seen += std::format(L", next {}:", nextText);
        std::vector<const std::byte*> states{nextRow};
        for (const auto swap : MappedNames(nextRow + kStateSwaps, kMaxSwaps))
        {
            if (const std::byte* swapRow = Row(g_stateRow, swap))
            {
                states.push_back(swapRow);
                seen += std::format(L" (or {}:)", players::unreal::NameText(swap));
            }
        }
        for (const std::byte* stateRow : states)
        {
            for (const auto name : MappedNames(stateRow + kStateActions, kMaxActions))
            {
                const std::byte* group   = Row(g_groupRow, name);
                const auto       actions = group != nullptr ? MappedNames(group + kGroupActions, kMaxGroup)
                                                            : std::vector<std::uint64_t>{name};
                for (const auto action : actions)
                {
                    const std::byte* actionRow = Row(g_actionRow, action);
                    if (actionRow == nullptr)
                    {
                        continue;  // no Action row: nothing runs for it
                    }
                    const auto  type  = ReadField<std::int32_t>(actionRow, kActionType);
                    const auto* ints  = MappedData(actionRow + kActionInts);
                    const auto  kinds = MappedCount(actionRow + kActionInts, 64);
                    const auto  kind  = kinds > 0 && ints != nullptr ? ReadField<std::int32_t>(ints, 0) : -1;
                    seen += std::format(L" {}({}, {})", players::unreal::NameText(action), type, kind);
                    if (type == kShowMenu && (kind == kShopKind || kind == kSkillTreeKind))
                    {
                        return Verdict::Shop;
                    }
                }
            }
        }
    }
    return Verdict::NotShop;
}

/// The press: StateTrigger_Request. Player 2's press of a shopkeeper (DispatchKey's object branch inside player 2's
/// pass) is kept for the shop it opens. Every call passes on unchanged.
std::uint64_t Request(std::uint64_t trigger, std::uint64_t sender, float delay, std::uint8_t flags) noexcept
{
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (generation::OnGameThread() && generation::Active() && field_pickup::InPass() && g_image &&
        g_image->ResearchAddress(returnAddress) == kDispatchReturn)
    {
        const auto known = g_known.find(sender);
        const bool shop  = known != g_known.end() && IsAdmitted(known->second.verdict);
        try
        {
            if (shop)
            {
                vending::PressedShopkeeper(sender);
                g_pressKey     = sender;
                g_leaderLogged = 0;
                g_leaderCalls  = 0;
            }
            platform::Log(L"shop talk: player 2 pressed {} (trigger {}): {}", players::unreal::NameText(sender),
                          players::unreal::NameText(trigger),
                          shop ? L"the shop it opens is player 2's" : L"not a known shopkeeper (left to the game)");
        }
        catch (...)
        {
        }
    }
    return reinterpret_cast<RequestFn>(g_request.Original())(trigger, sender, delay, flags);
}

/// The shop's AI event at its Begin (the NPC and the leader): player 2's member as the leader for player 2's shop.
std::uint64_t AiEvent(std::uint64_t a, std::uint64_t target, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto call = reinterpret_cast<Fn4>(g_aiEvent.Original());
    if (!generation::OnGameThread() || !generation::Active() || !vending::PlayerTwosShop())
    {
        return call(a, target, c, d);
    }
    std::uint64_t result = 0;
    bool          held   = false;
    {
        const leader_scope::LeaderScope scope(leader_scope::WeakOf(generation::Member()));
        held   = scope.Held();
        result = call(a, target, c, d);
    }
    try
    {
        platform::Log(L"shop talk: the shop's AI event (target {}) ran with {} as the leader",
                      players::unreal::NameText(target), held ? L"player 2's member" : L"player 1's character (the scope was refused)");
    }
    catch (...)
    {
    }
    return result;
}

/// Logging only: the game's LEADER_ resolver while player 2's shopkeeper press is pending (who the greeting looks at).
std::uint64_t LeaderName(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto result        = reinterpret_cast<Fn4>(g_leaderName.Original())(a, b, c, d);
    if (generation::OnGameThread() && g_pressKey != 0 && vending::ShopkeeperPending())
    {
        ++g_leaderCalls;
        if (g_leaderLogged < kLeaderLogs)
        {
            ++g_leaderLogged;
            platform::LogDebug(L"shop talk: the game resolved a named point (LEADER_ or BATTLE_LEADER_, result {}) while "
                               L"player 2's press of {} is pending, from 0x{:X}; the leader is player 1's character",
                               result & 0xFF, players::unreal::NameText(g_pressKey),
                               g_image ? g_image->ResearchAddress(returnAddress) : returnAddress);
        }
    }
    return result;
}

bool Place(const platform::GameImage& image, platform::EntryRedirect& redirect, const platform::FunctionFact& fact,
           std::uintptr_t replacement) noexcept
{
    const auto at = platform::FindFunction(image, fact);
    return at && redirect.Install(*at, image.EntryInstruction(fact.address, fact.entryBytes.first(kFirstInstruction)),
                                  replacement);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image          = image;
    const auto find  = platform::FindFunction(image, kFindObject);
    const auto state = platform::FindFunction(image, kStateRow);
    const auto act   = platform::FindFunction(image, kActionRow);
    const auto group = platform::FindFunction(image, kGroupRow);
    const auto root  = image.Rebase(charapop::kGameRoot, sizeof(std::uintptr_t));
    g_findObject     = find ? reinterpret_cast<FindFn>(*find) : nullptr;
    g_stateRow       = state ? reinterpret_cast<AccessorFn>(*state) : nullptr;
    g_actionRow      = act ? reinterpret_cast<AccessorFn>(*act) : nullptr;
    g_groupRow       = group ? reinterpret_cast<AccessorFn>(*group) : nullptr;
    g_gameRoot       = root ? reinterpret_cast<const std::uintptr_t*>(*root) : nullptr;
    g_queue.reserve(kQueueSize);
    const bool walk    = g_findObject != nullptr && g_stateRow != nullptr && g_actionRow != nullptr && g_gameRoot != nullptr;
    const bool presses = walk && Place(image, g_request, kRequest, reinterpret_cast<std::uintptr_t>(&Request));
    const bool events  = Place(image, g_aiEvent, kAiEvent, reinterpret_cast<std::uintptr_t>(&AiEvent));
    const bool leaders = Place(image, g_leaderName, kLeaderName, reinterpret_cast<std::uintptr_t>(&LeaderName));
    g_admitting        = walk && presses;
    platform::Log(L"shop talk: {} (the data walk {}{}, presses {}, the shop's AI event {}, the LEADER_ log {})",
                  g_admitting ? L"ready (player 2 uses shopkeepers through the game's own field prompt and shop)"
                              : L"UNAVAILABLE for player 2 (a check failed; shopkeepers stay player 1's)",
                  walk ? L"found" : L"NOT found",
                  g_groupRow != nullptr ? L"" : L" (action groups NOT found: names read as actions only)",
                  presses ? L"watched" : L"NOT watched",
                  events ? L"for player 2's shop" : L"NOT placed (the NPC reacts to player 1's character)",
                  leaders ? L"placed" : L"NOT placed");
}

bool Admits(std::uint64_t key) noexcept
{
    if (!g_admitting || key == 0 || !generation::OnGameThread() || !vending::ShopIdle())
    {
        return false;
    }
    const auto found = g_known.find(key);
    if (found == g_known.end())
    {
        if (g_queue.size() < g_queue.capacity())
        {
            g_queue.push_back(key);  // within the reserved capacity: no allocation under the lock
        }
        return false;
    }
    // Read again later: a shop's verdict is confirmed now and then (admitted meanwhile); any other is read again soon
    // (its GS object may come after its point, or its talk state may be mid-talk).
    Known&     known    = found->second;
    const bool admitted = IsAdmitted(known.verdict);
    const auto now      = Clock::now();
    if (now - known.queuedAt > (known.verdict == Verdict::Shop ? kShopRecheck : kRecheck) &&
        g_queue.size() < g_queue.capacity())
    {
        known.queuedAt = now;
        g_queue.push_back(key);
    }
    return admitted;
}

void Tick() noexcept
{
    if (!g_admitting || !generation::OnGameThread())
    {
        return;
    }
    if (g_pressKey != 0 && !vending::ShopkeeperPending())
    {
        if (g_leaderCalls != 0)
        {
            platform::LogDebug(L"shop talk: {} named-point resolution(s) while player 2's press of {} was pending",
                               g_leaderCalls, players::unreal::NameText(g_pressKey));
        }
        g_pressKey = 0;
    }
    if (!generation::Active())
    {
        g_known.clear();  // a new session (or map) reads every key again
        g_queue.clear();  // keeps its reserved capacity
        return;
    }
    if (g_queue.empty())
    {
        return;
    }
    const auto  now   = Clock::now();
    int         walks = 0;
    std::size_t done  = 0;
    while (done < g_queue.size() && walks < kWalksPerTick)
    {
        const std::uint64_t key = g_queue[done++];  // counted before the read: a key that throws is not read again here
        try
        {
            auto known = g_known.find(key);
            if (known != g_known.end() && now - known->second.readAt < kDuplicate)
            {
                continue;  // queued again before its read (an unknown key is queued on every pass until then)
            }
            ++walks;
            std::wstring seen;
            Verdict      verdict = Verdict::Unreadable;
            try
            {
                verdict = Walk(key, seen);
            }
            catch (...)
            {
                seen = L"the read threw";
            }
            if (known == g_known.end())
            {
                known = g_known.emplace(key, Known{verdict, now, now, false}).first;
            }
            const bool changed     = known->second.verdict != verdict;
            known->second.verdict  = verdict;
            known->second.queuedAt = now;
            known->second.readAt   = now;
            if (verdict != Verdict::NoObject && (!known->second.logged || changed))
            {
                known->second.logged = true;
                const bool odd = verdict == Verdict::ByName || verdict == Verdict::Unreadable;
                const std::wstring line = std::format(L"shop talk: {}: {} ({})", players::unreal::NameText(key),
                                                      Text(verdict), seen);
                if (odd)
                {
                    platform::Log(L"{}", line);  // the rows did not read as the research says: rare and worth seeing
                }
                else
                {
                    platform::LogDebug(L"{}", line);
                }
            }
        }
        catch (...)
        {
        }
    }
    g_queue.erase(g_queue.begin(), g_queue.begin() + static_cast<std::ptrdiff_t>(done));
}

}  // namespace lss::player_two::shop_talk
