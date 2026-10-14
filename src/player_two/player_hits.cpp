#include "player_two/player_hits.h"

#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/member_movement.h"
#include "player_two/party_member.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lss::player_two::player_hits {

namespace {

// Facts read from the database (RE-p2-enemy-target-bias.md §13-§14; this build). 1.005: replaced-player-hits.json (the
// damage slot is +0x388 there).

// The seven damage functions, each OnTakeDamage(component, attacker, hit, damage, a5): five integer arguments, no float
// (all seven prototypes, both builds), so all five are passed on.
constexpr std::uint8_t kBaseBytes[]   = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                         0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
constexpr std::uint8_t kEnemyBytes[]  = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                         0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
constexpr std::uint8_t kPlayerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                         0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr std::uint8_t kMoogleBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0x28,
                                         0xC9, 0xFE, 0x48, 0x2B, 0xE0, 0x41, 0x80, 0x79};
constexpr std::uint8_t kSummonBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0x76, 0xCE,
                                         0xC8, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x83, 0xC4};
constexpr std::uint8_t kCondorBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                         0x5C, 0xCE, 0xC8, 0xFE, 0x48, 0x2B, 0xE0, 0x48};
constexpr std::uint8_t kGatedBytes[]  = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x7C, 0x02, 0x00,
                                         0x00, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08};

constexpr platform::FunctionFact kBase{L"BattleCtrlComp_ApplyTakeDamage_FireAIEvents", 0x1418E6AC8, kBaseBytes};
constexpr platform::FunctionFact kEnemy{L"EnemyBattleComp_Vt380_OnTakeDamage", 0x140CB40D4, kEnemyBytes};
constexpr platform::FunctionFact kPlayer{L"BattlePlayerComp_Vt380_OnTakeDamage", 0x140CB27B4, kPlayerBytes};
constexpr platform::FunctionFact kMoogle{L"FatMoogleComp_Vt380_OnTakeDamage_IfNotHeal", 0x14341C6AC, kMoogleBytes};
constexpr platform::FunctionFact kSummon{L"SummonComp_Vt380_OnTakeDamage", 0x1434220B0, kSummonBytes};
constexpr platform::FunctionFact kCondor{L"CondorFortComp_Vt380_OnTakeDamage", 0x1434220C8, kCondorBytes};
constexpr platform::FunctionFact kGated{L"BattleComp2_Vt380_OnTakeDamage_Gated", 0x143422120, kGatedBytes};

// Every vtable entry holding one of them (the only references to the six overrides; the base is otherwise called only
// by the overrides as their parent, so it runs inside a wrapped call).
constexpr std::size_t        kSlots      = 10;
constexpr std::size_t        kPlayerSlot = 2;  // player 2's own component: the one slot where player 2 is the victim
constexpr platform::SlotFact kSlotFacts[kSlots] = {
    {L"the Enemy battle component's damage slot", 0x1457F1040, kEnemy},
    {L"the EnemyEN2228 battle component's damage slot", 0x1466B7678, kEnemy},
    {L"the BattlePlayer component's damage slot", 0x145796258, kPlayer},
    {L"the PlayerBase battle component's damage slot", 0x145795B40, kBase},
    {L"the Friend battle component's damage slot", 0x14580B078, kBase},
    {L"the Battle battle component's damage slot", 0x145CE3E58, kBase},
    {L"the PlayerBase2 battle component's damage slot", 0x14580BA48, kMoogle},
    {L"the Summon battle component's damage slot", 0x145902A48, kSummon},
    {L"the CondorFort battle component's damage slot", 0x1466B87F8, kCondor},
    {L"the Battle2 battle component's damage slot", 0x1466B7F08, kGated},
};
constexpr const wchar_t* kSlotNames[kSlots] = {L"Enemy",    L"EnemyEN2228", L"BattlePlayer", L"PlayerBase",
                                               L"Friend",   L"Battle",      L"PlayerBase2",  L"Summon",
                                               L"CondorFort", L"Battle2"};

// The summon gauge's one leader read (EnemyBattleComp_SummonGauge_AddOnHit_LeaderPtr: attacker == leader → the
// "Player" charge scale, else "Guest"), called from the Enemy override after the base. Inside one 16-byte block in both
// builds; no branch lands inside it.
constexpr std::uintptr_t kLeaderReadCall = 0x140CB3D51;

using DamageFn = std::uint64_t (*)(std::uint64_t component, std::uint64_t attacker, std::uint64_t hit,
                                   std::uint64_t damage, std::uint64_t e);
using LeaderFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

// Set once at install.
platform::VTableEntry  g_slots[kSlots];
platform::CallRedirect g_leaderRead;

std::atomic<bool> g_on{false};

// Player 2's character while its hit is applied on this thread (the summon gauge's leader read answers it), else null.
thread_local const void* t_attacker = nullptr;

enum class Role : std::uint8_t
{
    None,
    Attacker,
    Victim,
};

struct Involvement
{
    Role        role   = Role::None;
    int         type   = -1;  // player 2's member's player type
    const void* member = nullptr;
};

/// Whether player 2's character attacks in this damage call, or (on its own component's slot) takes it.
Involvement Involved(std::uint64_t component, std::uint64_t attacker, bool victimSlot) noexcept
{
    if (!g_on.load(std::memory_order_acquire) || !generation::OnGameThread() || !generation::Active())
    {
        return {};
    }
    const void* const member = generation::Member();
    if (member == nullptr)
    {
        return {};
    }
    const bool attacks = reinterpret_cast<const void*>(attacker) == member;
    if (!attacks && !victimSlot)
    {
        return {};
    }
    const auto type = party_member::PlayerTypeOf(member);
    if (!type)
    {
        return {};
    }
    if (attacks)
    {
        return {Role::Attacker, *type, member};
    }
    return member_movement::ComponentPlayerType(reinterpret_cast<const void*>(component)) == *type
               ? Involvement{Role::Victim, *type, member}
               : Involvement{};
}

/// The game's damage call with player 2's answers held on this thread: the leader test's yes for player 2's type, and
/// player 2's character for the summon gauge's leader read when it attacks. Both are put back in the __finally, which
/// also runs when a structured exception unwinds through here (destructors would not, under /EHsc), so a thread never
/// keeps a stale answer. No object here needs unwinding (the __try rule).
std::uint64_t CallAsPlayer(DamageFn original, std::uint64_t component, std::uint64_t attacker, std::uint64_t hit,
                           std::uint64_t damage, std::uint64_t e, int type, const void* attackerAnswer) noexcept
{
    const int         outerType     = member_movement::ExchangeThreadLeaderAnswer(type);
    const void* const outerAttacker = t_attacker;
    t_attacker                      = attackerAnswer;  // null when player 2 is the victim (a nested call's own)
    std::uint64_t result            = 0;
    __try
    {
        result = original(component, attacker, hit, damage, e);
    }
    __finally
    {
        t_attacker = outerAttacker;
        member_movement::ExchangeThreadLeaderAnswer(outerType);
    }
    return result;
}

template <std::size_t I>
std::uint64_t Damage(std::uint64_t component, std::uint64_t attacker, std::uint64_t hit, std::uint64_t damage,
                     std::uint64_t e) noexcept
{
    const auto        original = reinterpret_cast<DamageFn>(g_slots[I].Original());
    const Involvement who      = Involved(component, attacker, I == kPlayerSlot);
    if (who.role == Role::None)
    {
        return original(component, attacker, hit, damage, e);
    }
    platform::LogDebug(L"player hits: player 2's character {} on {} {:#x}; player 2 counts as the player in this call",
                       who.role == Role::Attacker ? L"hits" : L"is hit", kSlotNames[I], component);
    return CallAsPlayer(original, component, attacker, hit, damage, e, who.type,
                        who.role == Role::Attacker ? who.member : nullptr);
}

/// The summon gauge's leader read: player 2's character while its hit is applied, else the game's leader.
std::uint64_t LeaderRead(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    if (const void* const attacker = t_attacker; attacker != nullptr)
    {
        platform::LogDebug(L"player hits: the summon gauge charges at the player's rate for player 2's hit");
        return reinterpret_cast<std::uint64_t>(attacker);
    }
    return reinterpret_cast<LeaderFn>(g_leaderRead.Original())(a, b, c, d);
}

constexpr DamageFn kWrappers[kSlots] = {&Damage<0>, &Damage<1>, &Damage<2>, &Damage<3>, &Damage<4>,
                                        &Damage<5>, &Damage<6>, &Damage<7>, &Damage<8>, &Damage<9>};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!member_movement::LeaderTestReplaced())
    {
        platform::Log(L"player hits: UNAVAILABLE (the leader-test slot is not the mod's); player 2's hits count as an "
                      L"AI ally's");
        return false;
    }
    // The leader read first: it answers only inside a wrapped call, so it changes nothing until a slot is in place.
    const auto site     = image.Rebase(kLeaderReadCall, 5);
    const auto resolver = platform::FindFunction(image, facts::kResolvePartyLeader);
    const bool gauge    = site && resolver &&
                       g_leaderRead.Install(*site, *resolver, reinterpret_cast<std::uintptr_t>(&LeaderRead));
    g_on.store(true, std::memory_order_release);
    std::size_t slots = 0;
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (platform::ReplaceSlot(image, kSlotFacts[i], reinterpret_cast<std::uintptr_t>(kWrappers[i]), g_slots[i]))
        {
            ++slots;
        }
        else
        {
            platform::Log(L"player hits: the {} damage slot keeps the game's own answers", kSlotNames[i]);
        }
    }
    if (slots == 0)
    {
        g_on.store(false, std::memory_order_release);
        platform::Log(L"player hits: UNAVAILABLE (no damage slot could be replaced); player 2's hits count as an AI "
                      L"ally's");
        return false;
    }
    platform::Log(L"player hits: player 2's hits and the hits player 2 takes count as the player's ({} of {} damage "
                  L"slots); summon gauge at the player's rate {}",
                  slots, kSlots, gauge ? L"on" : L"UNAVAILABLE (the leader read differs)");
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // every damage call from now on keeps the game's own answers
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        if (g_slots[i].Original() != 0 && !g_slots[i].Restore())
        {
            platform::Log(L"player hits: the {} damage slot was changed after us; left in place (it passes through)",
                          kSlotNames[i]);
        }
    }
    if (g_leaderRead.IsInstalled() && !g_leaderRead.Restore())
    {
        platform::Log(L"player hits: the summon gauge's leader read was changed after us; left in place (it passes "
                      L"through)");
    }
}

}  // namespace lss::player_two::player_hits
