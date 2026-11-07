#include "battle/equal_targeting.h"

#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_answer.h"
#include "scopes/thread_hold.h"
#include "session/party_member.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::battle::equal_targeting {

namespace {

// Facts read from the database (RE-p2-enemy-target-bias.md §10; this build). 1.005: replaced-equal-targeting.json.

// The Condition select's one read of the leader type: BuildCandidates' call of UEndPartyAPI_GetPartyLeaderType
// (PM ? PM[+0x150] : 16), once before its per-type loop; only al is used. On this build the call's five bytes cross a
// 16-byte block (1.005: inside one). The target is checked by the redirect itself (the site must call it).
constexpr std::uintptr_t kLeaderTypeCall = 0x140FA5E1F;
using facts::kGetPartyLeaderType;

// The Condition select, bool(selector, u8 reason, UObject** out), in its vtable's slot +0x88 (the out is written on
// every path; 0 keeps the current target). RE §8, S-TGTCENSUS; 1.005 through replaced-target-census.json.
using facts::kCondTargetSelect;

// Called, not hooked: the game's own rule for a candidate, as BuildCandidates applies it to each player type.
using facts::kCharacterByPlayerType;
constexpr std::uint8_t kValidTargetBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                              0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x48};
constexpr platform::FunctionFact kValidTarget{L"EnemyTargetSel_IsValidTarget", 0x140CB9B78, kValidTargetBytes};

constexpr std::uint8_t  kPartyLane = 0;  // the battle/field party's slots
// The selector's excluded player types: TArray<u8, TInlineAllocator<10>> holding 15, so the data is on the heap
// (read like the game: the heap pointer if set, else inline).
constexpr std::uint32_t kSelExcluded = 0x140, kSelExcludedHeap = 0x150, kSelExcludedNum = 0x158;
using facts::kPartyManagerLeaderType;
constexpr std::uint8_t  kPartyTypes      = 15;     // candidate types 0..14
constexpr std::uint32_t kNoTypes         = 0xFFFFFFFF;
constexpr std::uint8_t  kNoByte          = 0xFF;  // a player's byte in g_types without a type
constexpr int           kNoType          = -1;  // no leader-test answer (leader_answer)
static_assert(players::kMaxPlayers <= 4, "every player's type fits in one 32-bit word");

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
// Every player's type, one word written once per frame on the game thread so a reader on any thread sees one frame's
// set: player p's in byte p (player 1's, the leader type, in bits 0-7; seat s's member's in byte s; kNoByte for a seat
// without a member); kNoTypes while no seat has one.
std::atomic<std::uint32_t> g_types{kNoTypes};
std::atomic<std::uint64_t> g_coin{0};  // splitmix64 state; the game's own per-thread random stream is not touched

// The answer of the select running on this thread (null outside one).
thread_local Answer* t_answer = nullptr;

constexpr std::uint8_t TypeByte(std::uint32_t types, int player) noexcept
{
    return static_cast<std::uint8_t>(types >> (8 * player));
}

/// One uniform 64-bit coin (ucpc_choice reads its high word; two eligible: its top bit).
std::uint64_t Coin() noexcept
{
    constexpr std::uint64_t kStep = 0x9E3779B97F4A7C15ull;
    std::uint64_t           x     = g_coin.fetch_add(kStep, std::memory_order_relaxed) + kStep;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
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
        !scopes::leader_answer::Replaced())
    {
        return answer;  // untouched: the game's own answers
    }
    // Player 1 first, then the seats in seat order.
    ucpc_choice::Players in       = ucpc_choice::Bit(0);
    ucpc_choice::Players eligible = Eligible(selector, TypeByte(types, 0), nullptr) ? ucpc_choice::Bit(0) : 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const std::uint8_t type = TypeByte(types, seat);
        if (type == kNoByte)
        {
            continue;
        }
        in |= ucpc_choice::Bit(seat);
        const void* const member = players::generation::MemberOf(seat);  // null once the seat left (until the next Tick)
        if (member != nullptr && Eligible(selector, type, member))
        {
            eligible |= ucpc_choice::Bit(seat);
        }
    }
    answer.choice = ucpc_choice::Choose(in, eligible, ucpc_choice::NeedsCoin(in, eligible) ? Coin() : 0);
    answer.type   = answer.choice.player >= 0 ? TypeByte(types, answer.choice.player) : kNoPlayerType;
    return answer;
}

/// The game's select with the leader test answering `onlyType` alone on this thread (kNoType: none), put back
/// afterwards (scopes/thread_hold.h).
std::uint64_t SelectWithOnlyLeader(Fn select, std::uint64_t selector, std::uint64_t reason, std::uint64_t out,
                                   std::uint64_t d, int onlyType) noexcept
{
    return scopes::thread_hold::HoldingExchange(&scopes::leader_answer::ExchangeThreadOnly, onlyType, select, selector, reason,
                                        out, d);
}

/// The game's select with this thread's two answers held: `answer` for the leader-type read, `onlyType` for the
/// leader test. Both set unconditionally: an untouched select nested in an answered one must not inherit the
/// outer's leader test. Both put back afterwards, also when a structured exception unwinds through the select.
std::uint64_t CallHolding(Fn select, std::uint64_t selector, std::uint64_t reason, std::uint64_t out, std::uint64_t d,
                          Answer* answer, int onlyType) noexcept
{
    return scopes::thread_hold::Holding(t_answer, answer, &SelectWithOnlyLeader, select, selector, reason, out, d, onlyType);
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
    if (!scopes::leader_answer::Replaced())
    {
        platform::Log(L"equal targeting: UNAVAILABLE (the leader-test slot is not the mod's); Condition enemies keep "
                      L"the game's own answer");
        return false;
    }
    const auto characterByType = platform::FindFunction(image, kCharacterByPlayerType);
    const auto validTarget     = platform::FindFunction(image, kValidTarget);
    const auto manager         = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto site            = image.Rebase(kLeaderTypeCall, 5);
    const auto target          = image.Rebase(kGetPartyLeaderType.address);
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
    if (!platform::ReplaceSlot(image, kCondTargetSelect, reinterpret_cast<std::uintptr_t>(&CondSelect), g_condSelect))
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
    platform::Log(L"equal targeting: Condition enemies (story bosses, summons, VR) count player 1's, player 2's or player 3's "
                  L"character as the player-controlled one, a coin per select when more than one of them can be targeted");
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
    if (!g_on.load(std::memory_order_relaxed) || !players::generation::OnGameThread())
    {
        return;
    }
    void* members[players::kMaxPlayers] = {};  // by seat; index 0 unused
    bool  anyMember                     = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        members[seat] = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        anyMember     = anyMember || members[seat] != nullptr;
    }
    std::uint32_t types   = kNoTypes;
    void*         manager = nullptr;
    std::uint8_t  leader  = 16;
    if (anyMember &&
        platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_partyManager), sizeof(manager)) &&
        manager != nullptr &&
        platform::CopyGuarded(&leader, static_cast<const std::byte*>(manager) + kPartyManagerLeaderType,
                              sizeof(leader)))
    {
        // A seat's member counts only with a type of its own: not the leader's, nor an earlier seat's. Each member's type
        // is read whatever the leader type; a leader type that is no party type then counts no seat.
        std::uint32_t built = leader;
        bool          any   = false;
        for (int seat = 1; seat < players::kMaxPlayers; ++seat)
        {
            std::uint8_t byte = kNoByte;
            if (members[seat] != nullptr)
            {
                const auto memberType = session::party_member::PlayerTypeOf(members[seat]);
                bool       own        = memberType && *memberType < kPartyTypes;
                for (int other = 0; own && other < seat; ++other)
                {
                    own = *memberType != TypeByte(built, other);
                }
                byte = own ? *memberType : kNoByte;
                any  = any || own;
            }
            built |= static_cast<std::uint32_t>(byte) << (8 * seat);
        }
        types = any && leader < kPartyTypes ? built : kNoTypes;
    }
    const auto before = g_types.exchange(types, std::memory_order_relaxed);
    if (before != types && platform::Enabled(platform::Level::Debug))
    {
        if (types == kNoTypes)
        {
            platform::LogDebug(L"equal targeting: neither player 2 nor 3 has a member; Condition enemies keep the game's own UCPC");
        }
        else
        {
            try
            {
                std::wstring seats;
                for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
                {
                    if (TypeByte(types, seat) != kNoByte)
                    {
                        seats += std::format(L" or {}'s member's type {}", players::SeatName(seat),
                                             TypeByte(types, seat));
                    }
                }
                platform::LogDebug(L"equal targeting: the UCPC is player 1's type {}{}", TypeByte(types, 0), seats);
            }
            catch (...)
            {
            }
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
    PlayerTypes found;
    for (int player = 0; player < players::kMaxPlayers; ++player)
    {
        const std::uint8_t byte = TypeByte(types, player);
        found.type[player]      = byte == kNoByte ? kNoPlayerType : byte;
    }
    return found;
}

Answer ChooseFor(void* selector) noexcept
{
    return AnswerFor(selector);
}

bool SeatEligible(int seat, void* selector) noexcept
{
    const auto  types  = Types();
    const void* member = players::generation::MemberOf(seat);
    return selector != nullptr && types && types->Has(seat) && member != nullptr &&
           Eligible(selector, types->type[seat], member);
}

}  // namespace lss::battle::equal_targeting
