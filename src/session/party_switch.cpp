#include "session/party_switch.h"

#include "battle/command_menu_tick.h"
#include "facts/controller.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "session/party_switch_rules.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>

namespace lss::session::party_switch {

namespace {

// Facts read from the database (RE-p2-party-switch.md §Q1, §Q4, recipe; this build).
// The two component getters the switch asks per member, and their calls in the cycle and CanSwitch.
constexpr std::uint8_t kPlayerComponentBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x08, 0xF7, 0x6B, 0x01};
constexpr platform::FunctionFact kPlayerComponent{L"EndCharacter_GetBattlePlayerControllerComponent", 0x1409EF81C,
                                                  kPlayerComponentBytes};
using facts::kAiControllerComponent;
constexpr std::uintptr_t kCycleCall     = 0x14158BAB0;  // in BattleLeader_CycleSwitch: rcx = the candidate
constexpr std::uintptr_t kCanSwitchCall = 0x14158BD82;  // in CanSwitch: rcx = the member counted

// The switch widget's "cannot switch" test, bool(u8 player type), and its two calls; the game's
// lookup of a character by player type (a leaf), PartyManager_GetCharacterByPlayerType(0, type).
constexpr std::uint8_t kDisabledBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x80,
                                           0x00, 0x00, 0x00, 0xE8, 0x38, 0xD3, 0x17, 0x01};
constexpr platform::FunctionFact kDisabled{L"CommandSwitchHud_IsMemberSwitchDisabled", 0x140F31BE8, kDisabledBytes};
using facts::kCharacterByPlayerType;
constexpr std::uintptr_t kDisabledCalls[] = {0x140F318C3, 0x140F31BB5};

// The forced switches (RE-ko-stay.md §2, §4): the KO tick's call to the cycle every frame while player 1's character
// is down, and the petrify tick's once when its wait runs out. The cycle itself, and those two calls.
constexpr std::uint8_t kCycleSwitchBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x44, 0x88, 0x48,
                                              0x20, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56};
constexpr platform::FunctionFact kCycleSwitch{L"BattleLeader_CycleSwitch", 0x14158B9B8, kCycleSwitchBytes};
constexpr std::uintptr_t         kKnockedOutCall = 0x142A2170F;  // in the dead state's tick
constexpr std::uintptr_t         kPetrifiedCall  = 0x142A7EE7D;  // in the battle component's tick, at the petrify wait's end
constexpr auto                   kForcedQuietMs  = std::chrono::milliseconds(1000);  // a refusal after this starts a new spell

// The player's request (RE-p2-own-battle-switch.md §1.1): FieldCommandCtrl_RequestLeaderCycleSwitch, the d-pad's
// path, refuses while the game's two event counters hold, then jumps into the cycle with "require CanSwitch" set.
// That tail jump (its five bytes inside one block in both builds) is redirected.
constexpr std::uintptr_t kRequestJump = 0x14158B95B;

using ComponentFn  = void* (*)(void* character);
using DisabledFn   = std::uint8_t (*)(std::uint8_t type);
using ByTypeFn     = void* (*)(void* unused, std::uint8_t type);
using CycleFn      = std::uint8_t (*)(void* system, std::uint8_t next, std::uint8_t requireCanSwitch, std::uint8_t handoff);
using LeaderTypeFn = std::uint8_t (*)();  // al only
using DeadFn       = bool (*)(void* unused, std::uint8_t type);
using HasStateFn   = bool (*)(std::uint8_t type, std::uint8_t state);

// Set once at install.
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall;
platform::CallRedirect g_disabledCalls[2];
platform::CallRedirect g_knockedOutCall;
platform::CallRedirect g_petrifiedCall;
platform::CallRedirect g_requestJump;
ByTypeFn               g_byType     = nullptr;
LeaderTypeFn           g_leaderType = nullptr;
DeadFn                 g_dead       = nullptr;
HasStateFn             g_hasState   = nullptr;
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t player = 0, ai = 0, cycle = 0, count = 0;  // 0: not found (logged by the finder)
    std::uintptr_t disabled = 0, byType = 0, disabledSites[2]{};
    std::uintptr_t cycleSwitch = 0, knockedOut = 0, petrified = 0;
    std::uintptr_t request = 0, leaderType = 0, dead = 0, hasState = 0;
};
Sites                   g_sites;
platform::DeferredSites g_placement;  // 1.005: the sites placed from the game thread (platform/deferred_sites.h)

// Game thread.
std::uint32_t g_refusals       = 0;  // candidates the switch was told to skip
std::uint32_t g_loggedRefusals = 0;
bool          g_greyLogged     = false;

/// The seat whose player plays `character` (1..), or 0 for none.
int SeatPlaying(const void* character) noexcept
{
    if (character == nullptr || !players::generation::OnGameThread())
    {
        return 0;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        // The member the player drives, or rides along with while a script has it on its AI: theirs either way.
        if (players::generation::Active(seat) && players::generation::CameraMemberOf(seat) == character)
        {
            return seat;
        }
    }
    return 0;
}

/// `character` is player 1's: the leader player 1's controller names, not who possesses it (a synergy hands player
/// 1's character to its AI for the approach, and it stays player 1's).
bool PlayerOnes(const void* character) noexcept
{
    const void* one = players::generation::PlayerOneController();
    return character != nullptr && one != nullptr && ReadPointer(one, facts::kControllerLeader) == character;
}

/// Whether the switch in flight must pass over `character` (session/party_switch_rules.h), and whose it is.
struct Refusal
{
    bool refused    = false;
    int  seat       = 0;      // the seat that plays it, 0 for none
    bool playerOnes = false;  // player 1's character, refused in a seat's pass
};

Refusal RefusalOf(const void* character) noexcept
{
    if (character == nullptr || !players::generation::OnGameThread())
    {
        return {};
    }
    const int  pass = battle::command_menu_tick::PassSeat();  // 0: player 1's own switch (or a forced one)
    const int  seat = SeatPlaying(character);
    const bool one  = pass != 0 && PlayerOnes(character);
    return {party_switch_rules::SwitchRefuses(pass, seat, one), seat, one};
}

std::wstring Whose(const Refusal& refusal)
{
    return refusal.playerOnes ? L"player 1's character" : std::format(L"{}'s member", players::SeatName(refusal.seat));
}

/// Logs the first refusals, then every 600th (a downed leader retries the switch every frame).
void NoteRefusal(const wchar_t* where, const Refusal& refusal) noexcept
{
    ++g_refusals;
    if (g_refusals <= 10 || g_refusals - g_loggedRefusals >= 600)
    {
        g_loggedRefusals = g_refusals;
        try
        {
            platform::Log(L"party switch: {} passed over {} ({} time(s) so far)", where, Whose(refusal), g_refusals);
        }
        catch (...)
        {
        }
    }
}

/// Replaces the cycle's component call: none for a member the pass may not take (the game skips it).
void* CycleComponent(void* character) noexcept
{
    if (const Refusal refusal = RefusalOf(character); refusal.refused)
    {
        NoteRefusal(L"the switch", refusal);
        return nullptr;
    }
    return reinterpret_cast<ComponentFn>(g_cycleCall.Original())(character);
}

/// Replaces CanSwitch's component call: a member the pass may not take is not counted.
void* CountedComponent(void* character) noexcept
{
    if (RefusalOf(character).refused)
    {
        return nullptr;
    }
    return reinterpret_cast<ComponentFn>(g_canSwitchCall.Original())(character);
}

/// Replaces the widget's test: the portrait of a member the pass may not take reads as not switchable.
std::uint8_t SwitchDisabled(std::uint8_t type) noexcept
{
    void* character = nullptr;
    const Refusal refusal = players::generation::OnGameThread() && players::generation::AnyActive() &&
                                    platform::CallGuarded(g_byType, character, static_cast<void*>(nullptr), type)
                                ? RefusalOf(character)
                                : Refusal{};
    if (refusal.refused)
    {
        if (!g_greyLogged)
        {
            g_greyLogged = true;
            try
            {
                platform::Log(L"party switch: the switch widget greys {} (type {})", Whose(refusal), type);
            }
            catch (...)
            {
            }
        }
        return 1;
    }
    return reinterpret_cast<DisabledFn>(g_disabledCalls[0].Original())(type);
}

}  // namespace

bool AnotherPlayerPlays() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && players::generation::CameraMemberOf(seat) != nullptr)
        {
            return true;  // a member driven, or ridden along with
        }
    }
    return false;
}

namespace {

/// One forced switch's refusals: a spell starts at the first refusal after a quiet second (logged), and its count goes
/// on (Trace per call).
struct ForcedSpell
{
    std::chrono::steady_clock::time_point last{};
    std::uint32_t                         calls = 0;
};
ForcedSpell g_knockedOut, g_petrified;

/// The game's forced switch for player 1's character, answered "nobody can take over" (its own answer when no member
/// can) while another player plays: player 1 stays on their downed or petrified character until it is revived or cured,
/// as every other player does (no retry is queued, and game over still waits for every member). The player's own
/// press switches them meanwhile (RequestedCycle).
std::uint8_t RefuseForced(ForcedSpell& spell, const platform::CallRedirect& call, const wchar_t* why, void* system,
                          std::uint8_t next, std::uint8_t requireCanSwitch, std::uint8_t handoff) noexcept
{
    if (!players::generation::OnGameThread() || !AnotherPlayerPlays())
    {
        return reinterpret_cast<CycleFn>(call.Original())(system, next, requireCanSwitch, handoff);
    }
    const auto now = std::chrono::steady_clock::now();
    if (spell.calls == 0 || now - spell.last > kForcedQuietMs)
    {
        spell.calls = 0;
        platform::Log(L"party switch: player 1's character is {}; player 1 stays on it until {} or their own switch "
                      L"(co-op: no forced switch)",
                      why, &spell == &g_petrified ? L"it is cured" : L"a revive");
    }
    ++spell.calls;
    spell.last = now;
    platform::LogTrace(L"party switch: the game's forced switch refused ({} call(s) this time)", spell.calls);
    return 0;
}

std::uint8_t KnockedOutCycle(void* system, std::uint8_t next, std::uint8_t requireCanSwitch, std::uint8_t handoff) noexcept
{
    return RefuseForced(g_knockedOut, g_knockedOutCall, L"down", system, next, requireCanSwitch, handoff);
}

std::uint8_t PetrifiedCycle(void* system, std::uint8_t next, std::uint8_t requireCanSwitch, std::uint8_t handoff) noexcept
{
    return RefuseForced(g_petrified, g_petrifiedCall, L"petrified", system, next, requireCanSwitch, handoff);
}

/// The leader as the game answers it here (a seat's member in that seat's pass): down, petrified, or able to act.
/// Null when it cannot be read.
const wchar_t* LeaderCannotAct() noexcept
{
    std::uint8_t leader = facts::kNoPlayerType;
    if (!platform::CallGuarded(g_leaderType, leader) || leader >= facts::kNoPlayerType)
    {
        return nullptr;
    }
    bool dead = false, petrified = false;
    if (platform::CallGuarded(g_dead, dead, static_cast<void*>(nullptr), leader) && dead)
    {
        return L"down";
    }
    if (platform::CallGuarded(g_hasState, petrified, leader, facts::kPetrifyStateId) && petrified)
    {
        return L"petrified";
    }
    return nullptr;
}

/// Replaces the request's jump into the cycle (the d-pad's path, for player 1 and, in its pass, each seat): the game's
/// cycle, with its two-standing gate dropped in co-op for a leader that cannot act (party_switch_rules.h
/// RequiresCanSwitch: the game's own switch for a downed leader runs so, and the mod refuses that one), and without the
/// targeted hand-over in a seat's pass (a written exception: its executor runs outside every pass).
std::uint8_t RequestedCycle(void* system, std::uint8_t next, std::uint8_t requireCanSwitch, std::uint8_t handoff) noexcept
{
    const auto original = reinterpret_cast<CycleFn>(g_requestJump.Original());
    if (!players::generation::OnGameThread())
    {
        return original(system, next, requireCanSwitch, handoff);
    }
    const int      pass      = battle::command_menu_tick::PassSeat();
    const bool     coop      = pass != 0 || AnotherPlayerPlays();
    const wchar_t* cannotAct = coop ? LeaderCannotAct() : nullptr;
    const bool     gated     = party_switch_rules::RequiresCanSwitch(requireCanSwitch != 0, coop, cannotAct != nullptr);
    const std::uint8_t hand  = pass != 0 ? 0 : handoff;
    const std::uint8_t taken = original(system, next, gated ? 1 : 0, hand);
    if (coop)
    {
        const wchar_t* player = pass != 0 ? players::SeatName(pass) : L"player 1";
        if (requireCanSwitch != 0 && !gated)
        {
            platform::Log(L"party switch: {}'s character is {}; their own switch {} (the game's two-standing gate dropped)",
                          player, cannotAct, taken != 0 ? L"takes another member" : L"finds nobody to take");
        }
        else
        {
            platform::LogDebug(L"party switch: {} asked to switch to the {} member: {}{}", player,
                               next != 0 ? L"next" : L"previous", taken != 0 ? L"taken" : L"refused by the game's cycle",
                               pass != 0 && handoff != 0 ? L" (the hand-over not asked: a seat's switch never hands its command over)"
                                                         : L"");
        }
    }
    return taken;
}

/// The redirects, from what Install found.
bool PlaceSites(const platform::GameImage&) noexcept
{
    const Sites& s = g_sites;
    // The forced switches stand on their own: each answered while another player plays, or the game's as before.
    const bool knockedOut = s.cycleSwitch != 0 && s.knockedOut != 0 &&
                            g_placement.Place(g_knockedOutCall, s.knockedOut, s.cycleSwitch,
                                              reinterpret_cast<std::uintptr_t>(&KnockedOutCycle));
    const bool petrified = s.cycleSwitch != 0 && s.petrified != 0 &&
                           g_placement.Place(g_petrifiedCall, s.petrified, s.cycleSwitch,
                                             reinterpret_cast<std::uintptr_t>(&PetrifiedCycle));
    platform::Log(L"party switch: in co-op player 1 stays on a downed character {} and on a petrified one {}",
                  knockedOut ? L"(the game's forced switch refused)" : L"UNAVAILABLE (a check failed: the game switches)",
                  petrified ? L"(refused too)" : L"UNAVAILABLE (a check failed: the game switches)");
    // The cycle and its count together, or neither: a count that disagrees with the cycle would let
    // the switch be offered with no one to switch to.
    bool switching = s.player != 0 && s.ai != 0 && s.cycle != 0 && s.count != 0 &&
                     g_placement.Place(g_cycleCall, s.cycle, s.player,
                                       reinterpret_cast<std::uintptr_t>(&CycleComponent));
    if (switching &&
        !g_placement.Place(g_canSwitchCall, s.count, s.ai, reinterpret_cast<std::uintptr_t>(&CountedComponent)))
    {
        if (!g_cycleCall.Restore())
        {
            platform::Log(L"party switch: the cycle's call could not be put back; it stays (it passes through)");
        }
        switching = false;
    }
    // The widget greys a member only when the switch really skips it.
    bool widget = switching && s.disabled != 0 && s.byType != 0;
    if (widget)
    {
        g_byType = reinterpret_cast<ByTypeFn>(s.byType);
        for (std::size_t i = 0; widget && i < std::size(s.disabledSites); ++i)
        {
            widget = s.disabledSites[i] != 0 && g_placement.Place(g_disabledCalls[i], s.disabledSites[i], s.disabled,
                                                                  reinterpret_cast<std::uintptr_t>(&SwitchDisabled));
        }
        if (!widget)
        {
            for (auto& call : g_disabledCalls)
            {
                if (call.IsInstalled() && !call.Restore())
                {
                    platform::Log(L"party switch: a widget call could not be put back; it stays (it passes through)");
                }
            }
        }
    }
    // The request's jump: only with the refusals in place (a seat's switch could take player 1's character otherwise).
    // Its five bytes lie in one block in both builds, so it is placed from any thread.
    bool request = switching && s.request != 0 && s.leaderType != 0 && s.dead != 0 && s.hasState != 0;
    if (request)
    {
        g_leaderType = reinterpret_cast<LeaderTypeFn>(s.leaderType);
        g_dead       = reinterpret_cast<DeadFn>(s.dead);
        g_hasState   = reinterpret_cast<HasStateFn>(s.hasState);
        request      = g_requestJump.InstallTailJump(s.request, s.cycleSwitch, reinterpret_cast<std::uintptr_t>(&RequestedCycle));
    }
    platform::Log(L"party switch: {}; the switch widget {}; {}",
                  switching ? L"every player's switch takes a free AI member only (never another player's character, "
                              L"and players 2 and 3 never player 1's)"
                            : L"UNAVAILABLE (a check failed): player 1 can switch onto player 2's or 3's member",
                  widget ? L"greys the members a player may not take"
                         : L"shows player 2's or 3's member as switchable (a check failed)",
                  request ? L"a downed or petrified player's own switch runs (the game's two-standing gate dropped in "
                            L"co-op); players 2 and 3's switch never hands a command over"
                          : L"a downed player's own switch stays the game's refusal (a check failed)");
    return switching || knockedOut || petrified;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact).value_or(0); };
    const auto site = [&image](std::uintptr_t address) { return image.Rebase(address, 5).value_or(0); };
    g_sites = {find(kPlayerComponent), find(kAiControllerComponent), site(kCycleCall), site(kCanSwitchCall),
               find(kDisabled),        find(kCharacterByPlayerType),
               {site(kDisabledCalls[0]), site(kDisabledCalls[1])},
               find(kCycleSwitch),     site(kKnockedOutCall), site(kPetrifiedCall),
               site(kRequestJump),     find(facts::kGetPartyLeaderType), find(facts::kIsTypeDead), find(facts::kTypeHasStateId)};
    // The cycle's call crosses a 16-byte block in 1.005: the sites are placed from the game thread, between its
    // ticks (platform/deferred_sites.h). The placement reads only the addresses found here.
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"party switch: {}",
                      placed ? L"its calls are placed from the game thread (1.005: one crosses a 16-byte block)"
                             : L"UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    for (auto* call : {&g_requestJump, &g_petrifiedCall, &g_knockedOutCall, &g_disabledCalls[1], &g_disabledCalls[0],
                       &g_canSwitchCall, &g_cycleCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"party switch: a switch call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::session::party_switch
