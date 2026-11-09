#include "session/party_switch.h"

#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>

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

using ComponentFn = void* (*)(void* character);
using DisabledFn  = std::uint8_t (*)(std::uint8_t type);
using ByTypeFn    = void* (*)(void* unused, std::uint8_t type);
using CycleFn     = std::uint8_t (*)(void* system, std::uint8_t next, std::uint8_t requireCanSwitch, std::uint8_t handoff);

// Set once at install.
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall;
platform::CallRedirect g_disabledCalls[2];
platform::CallRedirect g_knockedOutCall;
platform::CallRedirect g_petrifiedCall;
ByTypeFn               g_byType = nullptr;
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t player = 0, ai = 0, cycle = 0, count = 0;  // 0: not found (logged by the finder)
    std::uintptr_t disabled = 0, byType = 0, disabledSites[2]{};
    std::uintptr_t cycleSwitch = 0, knockedOut = 0, petrified = 0;
};
Sites                   g_sites;
platform::DeferredSites g_placement;  // 1.005: the sites placed from the game thread (platform/deferred_sites.h)

// Game thread.
std::uint32_t g_refusals       = 0;  // candidates the switch was told to skip
std::uint32_t g_loggedRefusals = 0;
bool          g_greyLogged     = false;

/// The seat whose player plays `character` (1..), or 0 for none: player 1 never switches onto a seat's member.
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

/// Logs the first refusals, then every 600th (a downed leader retries the switch every frame).
void NoteRefusal(const wchar_t* where, int seat) noexcept
{
    ++g_refusals;
    if (g_refusals <= 10 || g_refusals - g_loggedRefusals >= 600)
    {
        g_loggedRefusals = g_refusals;
        platform::Log(L"party switch: {} skipped {}'s member ({} time(s) so far)", where, players::SeatName(seat),
                      g_refusals);
    }
}

/// Replaces the cycle's component call: none for a seat's member (the game skips it).
void* CycleComponent(void* character) noexcept
{
    if (const int seat = SeatPlaying(character); seat != 0)
    {
        NoteRefusal(L"the switch", seat);
        return nullptr;
    }
    return reinterpret_cast<ComponentFn>(g_cycleCall.Original())(character);
}

/// Replaces CanSwitch's component call: a seat's member is not counted.
void* CountedComponent(void* character) noexcept
{
    if (SeatPlaying(character) != 0)
    {
        return nullptr;
    }
    return reinterpret_cast<ComponentFn>(g_canSwitchCall.Original())(character);
}

/// Replaces the widget's test: a seat's member's portrait reads as not switchable.
std::uint8_t SwitchDisabled(std::uint8_t type) noexcept
{
    void* character = nullptr;
    const int seat  = players::generation::OnGameThread() && players::generation::AnyActive() &&
                             platform::CallGuarded(g_byType, character, static_cast<void*>(nullptr), type)
                          ? SeatPlaying(character)
                          : 0;
    if (seat != 0)
    {
        if (!g_greyLogged)
        {
            g_greyLogged = true;
            platform::Log(L"party switch: the switch widget greys {}'s member (type {})", players::SeatName(seat),
                          type);
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
/// as every other player does (no retry is queued, and game over still waits for every member).
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
        platform::Log(L"party switch: player 1's character is {}; player 1 stays on it until {} (co-op: no forced switch)", why,
                      &spell == &g_petrified ? L"it is cured" : L"a revive");
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
    // The widget greys a seat's member only when the switch really skips it.
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
    platform::Log(L"party switch: {}; the switch widget {}",
                  switching ? L"player 1's switch skips every other player's member"
                            : L"UNAVAILABLE (a check failed): player 1 can switch onto player 2's or 3's member",
                  widget ? L"greys player 2's or 3's member"
                         : L"shows player 2's or 3's member as switchable (a check failed)");
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
               find(kCycleSwitch),     site(kKnockedOutCall), site(kPetrifiedCall)};
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
    for (auto* call : {&g_petrifiedCall, &g_knockedOutCall, &g_disabledCalls[1], &g_disabledCalls[0], &g_canSwitchCall,
                       &g_cycleCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"party switch: a switch call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::session::party_switch
