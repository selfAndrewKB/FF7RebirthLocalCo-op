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

using ComponentFn = void* (*)(void* character);
using DisabledFn  = std::uint8_t (*)(std::uint8_t type);
using ByTypeFn    = void* (*)(void* unused, std::uint8_t type);

// Set once at install.
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall;
platform::CallRedirect g_disabledCalls[2];
ByTypeFn               g_byType = nullptr;
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t player = 0, ai = 0, cycle = 0, count = 0;  // 0: not found (logged by the finder)
    std::uintptr_t disabled = 0, byType = 0, disabledSites[2]{};
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
        if (players::generation::Active(seat) && players::generation::MemberOf(seat) == character)
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

/// The redirects, from what Install found.
bool PlaceSites(const platform::GameImage&) noexcept
{
    const Sites& s = g_sites;
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
    return switching;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact).value_or(0); };
    const auto site = [&image](std::uintptr_t address) { return image.Rebase(address, 5).value_or(0); };
    g_sites = {find(kPlayerComponent), find(kAiControllerComponent), site(kCycleCall), site(kCanSwitchCall),
               find(kDisabled),        find(kCharacterByPlayerType),
               {site(kDisabledCalls[0]), site(kDisabledCalls[1])}};
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
    for (auto* call : {&g_disabledCalls[1], &g_disabledCalls[0], &g_canSwitchCall, &g_cycleCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"party switch: a switch call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::session::party_switch
