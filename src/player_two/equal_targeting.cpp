#include "player_two/equal_targeting.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lss::player_two::equal_targeting {

namespace {

// Facts read from the database (RE-p2-enemy-target-bias.md §10; this build). 1.005: replaced-equal-targeting.json.

// The Condition select's one read of the leader type: BuildCandidates' call of UEndPartyAPI_GetPartyLeaderType
// (PM ? PM[+0x150] : 16), once before its per-type loop; only al is used. On this build the call's five bytes cross a
// 16-byte block (1.005: inside one). The target is checked by the redirect itself (the site must call it).
constexpr std::uintptr_t kLeaderTypeCall = 0x140FA5E1F;
constexpr std::uintptr_t kLeaderType     = 0x1408070C4;

// The Condition select, bool(selector, u8 reason, UObject** out), in its vtable's slot +0x88 (the out is written on
// every path; 0 keeps the current target). RE §8, S-TGTCENSUS; 1.005 through replaced-target-census.json.
constexpr std::uint8_t kCondSelectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::SlotFact kCondSelect{L"the Condition target selector's select slot (vt+0x88)", 0x14580C260,
                                         {L"CondTargetSel_Select_Vt88", 0x140FA5230, kCondSelectBytes}};

// Called, not hooked: the game's own rule for a candidate, as BuildCandidates applies it to each player type.
constexpr std::uint8_t kCharacterByTypeBytes[] = {0x4C, 0x8B, 0x05, 0xA5, 0xA6, 0x96, 0x08, 0x44,
                                                  0x0F, 0xB6, 0xC9, 0x4D, 0x85, 0xC0, 0x0F, 0x84};
constexpr platform::FunctionFact kCharacterByType{L"PartyManager_GetCharacterByPlayerType", 0x140A31634,
                                                  kCharacterByTypeBytes};
constexpr std::uint8_t kValidTargetBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                              0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x48};
constexpr platform::FunctionFact kValidTarget{L"EnemyTargetSel_IsValidTarget", 0x140CB9B78, kValidTargetBytes};

constexpr std::uint8_t  kPartyLane = 0;  // the battle/field party's slots
// The selector's excluded player types: TArray<u8, TInlineAllocator<10>> holding 15, so the data is on the heap
// (read like the game: the heap pointer if set, else inline).
constexpr std::uint32_t kSelExcluded = 0x140, kSelExcludedHeap = 0x150, kSelExcludedNum = 0x158;
constexpr std::uint32_t kPartyLeaderType = 0x150;  // u8, the party manager's leader type
constexpr std::uint8_t  kPartyTypes      = 15;     // candidate types 0..14
constexpr std::uint32_t kNoTypes         = 0xFFFF;
constexpr int           kNoType          = -1;  // no leader-test answer (member_movement)

using Fn                = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using CharacterByTypeFn = void* (*)(std::uint8_t lane, std::uint8_t type);
using ValidTargetFn     = std::uint8_t (*)(void* selector, void* character, std::uint8_t earlyAccept);

// Set once at install.
platform::CallRedirect g_leaderTypeCall;
platform::VTableEntry  g_condSelect;
CharacterByTypeFn      g_characterByType = nullptr;
ValidTargetFn          g_validTarget     = nullptr;
std::uintptr_t         g_partyManager    = 0;      // the pointer global's live address
bool                   g_crossing        = false;  // the call crosses a block: never restored

std::atomic<bool>           g_on{false};
std::atomic<SelectObserver> g_observer{nullptr};
// Both players' types, written once per frame on the game thread: player 1's (the leader type) in bits 0-7, player
// 2's member's in bits 8-15; kNoTypes without player 2's member.
std::atomic<std::uint32_t> g_types{kNoTypes};
std::atomic<std::uint64_t> g_coin{0};  // splitmix64 state; the game's own per-thread random stream is not touched

// The answer of the select running on this thread (null outside one).
thread_local Answer* t_answer = nullptr;

bool Coin() noexcept
{
    constexpr std::uint64_t kStep = 0x9E3779B97F4A7C15ull;
    std::uint64_t           x     = g_coin.fetch_add(kStep, std::memory_order_relaxed) + kStep;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return ((x ^ (x >> 31)) >> 63) != 0;
}

/// The game's candidate rule for `type` in this select: its party character (which must be `mustBe` when given), a
/// valid target for this enemy, and not excluded by the selector.
bool Eligible(void* selector, std::uint8_t type, const void* mustBe) noexcept
{
    void* character = nullptr;
    if (!platform::CallGuarded(g_characterByType, character, kPartyLane, type) || character == nullptr ||
        (mustBe != nullptr && character != mustBe))
    {
        return false;
    }
    std::uint8_t valid = 0;
    if (!platform::CallGuarded(g_validTarget, valid, selector, character, std::uint8_t{0}) || valid == 0)
    {
        return false;
    }
    const auto* heap     = ReadPointer(selector, kSelExcludedHeap);
    const auto* excluded = heap != nullptr ? static_cast<const std::byte*>(heap)
                                           : static_cast<const std::byte*>(selector) + kSelExcluded;
    std::uint8_t flag = 0;
    return type >= ReadField<std::int32_t>(selector, kSelExcludedNum) ||
           (platform::CopyGuarded(&flag, excluded + type, sizeof(flag)) && flag == 0);
}

/// BuildCandidates' leader-type read: the answer of this thread's select, else the game's.
std::uint64_t LeaderType(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (Answer* const answer = t_answer; answer != nullptr)
    {
        answer->read = true;
        if (answer->choice.ucpc != ucpc_choice::Ucpc::Untouched)
        {
            return answer->type;
        }
    }
    return reinterpret_cast<Fn>(g_leaderTypeCall.Original())(a, b, c, d);
}

/// Who this select counts as the UCPC (not yet read).
Answer AnswerFor(void* selector) noexcept
{
    Answer     answer;
    const auto types = g_types.load(std::memory_order_relaxed);
    if (selector == nullptr || types == kNoTypes || !g_on.load(std::memory_order_acquire) ||
        !member_movement::LeaderTestReplaced())
    {
        return answer;  // untouched: the game's own answers
    }
    const auto  one         = static_cast<std::uint8_t>(types & 0xFF);
    const auto  two         = static_cast<std::uint8_t>(types >> 8);
    const void* member      = generation::Member();  // null once player 2 left (until the next Tick)
    const bool  oneEligible = Eligible(selector, one, nullptr);
    const bool  twoEligible = member != nullptr && Eligible(selector, two, member);
    answer.choice = ucpc_choice::Choose(true, oneEligible, twoEligible, oneEligible && twoEligible && Coin());
    answer.type   = answer.choice.ucpc == ucpc_choice::Ucpc::PlayerTwo ? two : one;
    return answer;
}

/// The game's select with this thread's two answers held: `answer` for the leader-type read, `onlyType` for the
/// leader test (kNoType: none). Both are put back in the __finally, which also runs when a structured exception
/// unwinds through here (destructors would not, under /EHsc), so a thread never keeps a stale answer. No object here
/// needs unwinding (the __try rule).
std::uint64_t CallHolding(Fn select, std::uint64_t selector, std::uint64_t reason, std::uint64_t out, std::uint64_t d,
                          Answer* answer, int onlyType) noexcept
{
    // Both set unconditionally: an untouched select nested in an answered one must not inherit the outer's leader test.
    Answer* const outer     = t_answer;
    const int     outerOnly = member_movement::ExchangeThreadOnlyLeader(onlyType);
    t_answer                = answer;
    std::uint64_t result    = 0;
    __try
    {
        result = select(selector, reason, out, d);
    }
    __finally
    {
        t_answer = outer;
        member_movement::ExchangeThreadOnlyLeader(outerOnly);
    }
    return result;
}

/// The Condition select's slot: the game's select with who it counts as the UCPC answered on this thread, for that
/// call only; then the observer hears what was answered. Any thread.
std::uint64_t CondSelect(std::uint64_t selector, std::uint64_t reason, std::uint64_t out, std::uint64_t d) noexcept
{
    Answer     answer = AnswerFor(reinterpret_cast<void*>(selector));
    const auto result = CallHolding(reinterpret_cast<Fn>(g_condSelect.Original()), selector, reason, out, d, &answer,
                                    answer.choice.ucpc == ucpc_choice::Ucpc::Untouched ? kNoType : answer.type);
    if (const SelectObserver observer = g_observer.load(std::memory_order_relaxed); observer != nullptr)
    {
        observer(reinterpret_cast<void*>(selector), reason, out, result, answer);
    }
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!member_movement::LeaderTestReplaced())
    {
        platform::Log(L"equal targeting: UNAVAILABLE (the leader-test slot is not the mod's); Condition enemies keep "
                      L"the game's own answer");
        return false;
    }
    const auto characterByType = platform::FindFunction(image, kCharacterByType);
    const auto validTarget     = platform::FindFunction(image, kValidTarget);
    const auto manager         = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto site            = image.Rebase(kLeaderTypeCall, 5);
    const auto target          = image.Rebase(kLeaderType);
    if (!characterByType || !validTarget || !manager || !site || !target)
    {
        platform::Log(L"equal targeting: UNAVAILABLE (a check failed); Condition enemies keep the game's own answer");
        return false;
    }
    g_characterByType = reinterpret_cast<CharacterByTypeFn>(*characterByType);
    g_validTarget     = reinterpret_cast<ValidTargetFn>(*validTarget);
    g_partyManager    = *manager;
    g_crossing        = (*site & ~std::uintptr_t{0xF}) != ((*site + 4) & ~std::uintptr_t{0xF});
    // This runs in on_unreal_init: no world and no enemy exist yet, so no thread can be at the call. That is what
    // InstallFromOwnThread needs for a call crossing two blocks (the research build's).
    const auto replacement = reinterpret_cast<std::uintptr_t>(&LeaderType);
    if (!(g_crossing ? g_leaderTypeCall.InstallFromOwnThread(*site, *target, replacement)
                     : g_leaderTypeCall.Install(*site, *target, replacement)))
    {
        platform::Log(L"equal targeting: UNAVAILABLE (the leader-type call differs); Condition enemies keep the game's "
                      L"own answer");
        return false;
    }
    // The select slot: the one place the answer is held. Without it the leader-type redirect passes through (no
    // answer is ever on a thread).
    if (!platform::ReplaceSlot(image, kCondSelect, reinterpret_cast<std::uintptr_t>(&CondSelect), g_condSelect))
    {
        platform::Log(L"equal targeting: UNAVAILABLE (the Condition select's slot differs); Condition enemies keep the "
                      L"game's own answer");
        if (!g_crossing)
        {
            g_leaderTypeCall.Restore();
        }
        return false;
    }
    g_coin.store(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
                 std::memory_order_relaxed);
    g_on.store(true, std::memory_order_release);
    platform::Log(L"equal targeting: Condition enemies (story bosses, summons, VR) count player 1's or player 2's "
                  L"character as the player-controlled one, a coin per select when both can be targeted");
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // every select from now on keeps the game's own answer
    g_observer.store(nullptr, std::memory_order_relaxed);
    if (g_condSelect.Original() != 0 && !g_condSelect.Restore())
    {
        platform::Log(L"equal targeting: the Condition select's slot was changed after us; left in place (it passes "
                      L"through)");
    }
    if (!g_leaderTypeCall.IsInstalled())
    {
        return;
    }
    if (g_crossing)
    {
        platform::Log(L"equal targeting: the leader-type call crosses a block on this build; left in place (it passes "
                      L"through)");
    }
    else if (!g_leaderTypeCall.Restore())
    {
        platform::Log(L"equal targeting: the leader-type call was changed after us; left in place (it passes through)");
    }
}

bool Installed() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void Tick() noexcept
{
    if (!g_on.load(std::memory_order_relaxed) || !generation::OnGameThread())
    {
        return;
    }
    std::uint32_t types   = kNoTypes;
    void* const   member  = generation::Active() ? generation::Member() : nullptr;
    void*         manager = nullptr;
    std::uint8_t  leader  = 16;
    if (member != nullptr &&
        platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_partyManager), sizeof(manager)) &&
        manager != nullptr &&
        platform::CopyGuarded(&leader, static_cast<const std::byte*>(manager) + kPartyLeaderType, sizeof(leader)))
    {
        const auto memberType = party_member::PlayerTypeOf(member);
        if (memberType && *memberType < kPartyTypes && leader < kPartyTypes && *memberType != leader)
        {
            types = static_cast<std::uint32_t>(leader) | static_cast<std::uint32_t>(*memberType) << 8;
        }
    }
    const auto before = g_types.exchange(types, std::memory_order_relaxed);
    if (before != types)
    {
        if (types == kNoTypes)
        {
            platform::LogDebug(L"equal targeting: no player 2 member; Condition enemies keep the game's own UCPC");
        }
        else
        {
            platform::LogDebug(L"equal targeting: the UCPC is player 1's type {} or player 2's member's type {}",
                               types & 0xFF, types >> 8);
        }
    }
}

void SetSelectObserver(SelectObserver observer) noexcept
{
    g_observer.store(observer, std::memory_order_relaxed);
}

std::optional<PlayerTypes> Types() noexcept
{
    const auto types = g_types.load(std::memory_order_relaxed);
    if (types == kNoTypes || !g_on.load(std::memory_order_acquire))
    {
        return std::nullopt;
    }
    return PlayerTypes{static_cast<std::uint8_t>(types & 0xFF), static_cast<std::uint8_t>(types >> 8)};
}

Answer ChooseFor(void* selector) noexcept
{
    return AnswerFor(selector);
}

bool PlayerTwoEligible(void* selector) noexcept
{
    const auto  types  = Types();
    const void* member = generation::Member();
    return selector != nullptr && types && member != nullptr && Eligible(selector, types->two, member);
}

}  // namespace lss::player_two::equal_targeting
