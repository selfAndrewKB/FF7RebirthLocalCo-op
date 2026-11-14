#include "hud/low_health.h"

#include "battle/command_menu.h"
#include "battle/command_menu_tick.h"
#include "battle/command_menu_tick_shared.h"
#include "facts/hud.h"
#include "facts/party.h"
#include "hud/low_health_rules.h"
#include "input/pad_sticks.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "session/party_member.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>

namespace lss::hud::low_health {

namespace {

namespace f     = facts;
namespace rules = low_health_rules;
using platform::Read;

using HpFn          = std::int32_t (*)(std::uint8_t type);
using FilterOnFn    = void (*)(void* world, std::uint8_t type);
using FilterOffFn   = void (*)(std::uint8_t type);
using FilterCountFn = void (*)(void* manager, std::uint8_t type);
using DecisionFn    = void (*)(void* controller);
using FilterTickFn  = void (*)(void* slot, float dt);
using ApplyFn       = void (*)(void* functor, void* settings, float weight);

// Set at install, then only read.
bool          g_installed = false;
std::uintptr_t g_hpTarget = 0, g_onTarget = 0, g_offTarget = 0, g_tickTarget = 0;
FilterCountFn g_refInc   = nullptr;
FilterCountFn g_refDec   = nullptr;
DecisionFn    g_decide   = nullptr;
FilterTickFn  g_tick     = nullptr;

platform::CallRedirect  g_hpCall, g_dyingOnCall, g_stoneOnCall, g_dyingOffCall, g_stoneOffCall, g_tickCall;
platform::VTableEntry   g_apply;
platform::DeferredSites g_placement;  // the sites cross 16-byte blocks on both builds: placed from the game thread

// Game thread.
bool        g_perView = true;  // the switch
bool        g_split   = false;  // more than one player's view this tick
rules::Held g_held{rules::kNoType, rules::kNoType, rules::kNoType};
std::int32_t                              g_playerOneId = -1;
std::array<std::int32_t, rules::kPlayers> g_seatIds{-2, -2, -2};  // by seat, the playing seats' controller ids
int  g_deciding    = -1;     // the seat whose decision the mod runs now, outside a fight (-1: none)
bool g_fadesTicked = false;  // the players' fades ran this engine tick

/// A player's own fade: a slot of the game's layout, run by the game's tick (its weak world zero: it reaches no view).
struct alignas(16) Fade
{
    std::array<std::byte, f::kFilterSlotSize> bytes{};

    float Get(std::uint32_t offset) const noexcept
    {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    void Set(std::uint32_t offset, float value) noexcept { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }
};
std::array<Fade, rules::kPlayers>         g_fades{};
std::array<float, rules::kPlayers>        g_targets{-1.0f, -1.0f, -1.0f};  // each fade's target as last armed
std::array<std::uint8_t, rules::kPlayers> g_holds{};    // by seat ([0] unused): its counts of the shared filter
std::array<bool, rules::kPlayers>         g_differs{};  // the player's view took less than the game's at its last apply
static_assert(rules::kPlayers == 3, "the per-player initialisers above list three players");

std::uint32_t g_ticksSeen = 0, g_viewsOwn = 0, g_viewsUnknown = 0, g_decisions = 0;

/// The command controller of `player` (0 player 1's, the UI system's), or 0.
std::uintptr_t ControllerOf(int player) noexcept
{
    std::uintptr_t controller = 0;
    if (player == 0)
    {
        Read(battle::command_menu_tick::UiSystem() + f::kUICommandController, controller);
        return controller;
    }
    return reinterpret_cast<std::uintptr_t>(battle::command_menu::Controller(player));
}

std::int32_t DangerOf(int player) noexcept
{
    std::int32_t danger = 0;
    const std::uintptr_t controller = ControllerOf(player);
    return controller != 0 && Read(controller + f::kCmdDanger, danger) ? danger : 0;
}

std::uintptr_t FilterManager() noexcept
{
    std::uintptr_t manager = 0;
    Read(battle::command_menu_tick::UiSystem() + f::kUIFilterManager, manager);
    return manager;
}

/// The player whose controller's decision runs now: a seat's run by the mod, a seat's battle pass, else player 1's.
int DecidingPlayer() noexcept
{
    if (g_deciding > 0)
    {
        return g_deciding;
    }
    return battle::command_menu_tick::Pass() == 2 ? battle::command_menu_tick::PassSeat() : 0;
}

/// The decision's HP read: the deciding controller's own members only (rules::Counts), the game's read for those.
std::int32_t HpForDecidingController(std::uint8_t type) noexcept
{
    const auto game = reinterpret_cast<HpFn>(g_hpCall.Original());
    if (!g_perView)
    {
        return game(type);
    }
    return rules::Counts(DecidingPlayer(), type, g_held) ? game(type) : f::kNeverLowHp;
}

std::uint8_t TypeOf(std::uint8_t bit) noexcept
{
    return bit == rules::kHoldDying ? f::kFilterDying : f::kFilterStone;
}

/// `seat` gives back each count in `bits`. A count already at 0 (the manager's counts rebuilt under the hold) is not
/// taken below it: the game's decrement has no floor, and a count below 0 would keep the next holder's edge off.
void GiveBack(int seat, std::uint8_t bits) noexcept
{
    const std::uintptr_t manager = FilterManager();
    auto&                holds   = g_holds[static_cast<std::size_t>(seat)];
    for (const std::uint8_t bit : {rules::kHoldStone, rules::kHoldDying})
    {
        if ((bits & bit) == 0 || (holds & bit) == 0)
        {
            continue;
        }
        std::int32_t count = 0;
        const bool   read  = manager != 0 && Read(manager + f::kFilterCount + 4 * TypeOf(bit), count);
        if (read && count > 0 && !platform::CallGuarded(g_refDec, reinterpret_cast<void*>(manager), TypeOf(bit)))
        {
            continue;  // the decrement raised: the hold is kept, given back on the next try
        }
        holds &= static_cast<std::uint8_t>(~bit);
    }
}

/// The decision's filter on: player 1's controller takes the game's latch; a seat's a count of its own.
void FilterOn(void* world, std::uint8_t type) noexcept
{
    const int seat = g_perView ? DecidingPlayer() : 0;  // switched off: every controller the game's latch, as unmodded
    if (seat <= 0)
    {
        reinterpret_cast<FilterOnFn>(g_onTarget)(world, type);
        return;
    }
    auto&                 holds = g_holds[static_cast<std::size_t>(seat)];
    const rules::HoldStep step  = rules::OnEnable(holds, type == f::kFilterDying ? rules::kHoldDying : rules::kHoldStone);
    const std::uintptr_t  manager = FilterManager();
    if (step.take != 0 && manager != 0 && platform::CallGuarded(g_refInc, reinterpret_cast<void*>(manager), type))
    {
        holds = step.holds;
    }
}

/// The decision's filter off: player 1's controller drops the game's latch; a seat gives back its count (the Dying
/// off its Stone count too, as the game's does).
void FilterOff(std::uint8_t type) noexcept
{
    const int seat = g_perView ? DecidingPlayer() : 0;
    if (seat <= 0)
    {
        reinterpret_cast<FilterOffFn>(g_offTarget)(type);
        return;
    }
    GiveBack(seat, rules::OnDisable(g_holds[static_cast<std::size_t>(seat)], type == f::kFilterDying).give);
}

/// Sets `fade` going toward `target` as the game's setter sets its own: from the current value, elapsed zero, over the
/// game's length.
void Arm(Fade& fade, float target) noexcept
{
    fade.Set(f::kFilterFadeStart, fade.Get(f::kFilterIntensity));
    fade.Set(f::kFilterFadeTarget, target);
    fade.Set(f::kFilterFadeElapsed, 0.0f);
    fade.Set(f::kFilterFadeLength, f::kDyingFadeSeconds);
}

/// Each player's fade follows their controller's danger (player 1's allowed byte gates everyone's).
void ArmFades() noexcept
{
    std::uint8_t allowed = 1;
    const std::uintptr_t one = ControllerOf(0);
    if (one != 0)
    {
        Read(one + f::kCmdDangerAllowed, allowed);
    }
    for (int player = 0; player < rules::kPlayers; ++player)
    {
        const auto         at     = static_cast<std::size_t>(player);
        const std::int32_t danger = player == 0 || g_held[at] != rules::kNoType ? DangerOf(player) : 0;
        const float        want   = rules::Target(danger, allowed != 0, f::kDyingIntensity);
        if (g_targets[at] == want)
        {
            continue;
        }
        const bool first = g_targets[at] < 0.0f;
        g_targets[at]    = want;
        Arm(g_fades[at], want);
        if (first)
        {
            continue;
        }
        try
        {
            const std::wstring who = player == 0 ? std::wstring(L"player 1's character or a member no player controls")
                                                 : std::format(L"{}'s character", players::SeatName(player));
            if (want > 0.0f)
            {
                platform::Log(L"low health: {} in danger: their view fades the red edge in", who);
            }
            else
            {
                platform::Log(L"low health: {} out of danger{}: the red edge fades out of their view", who,
                              danger != 0 ? L" (an event hides the effect)" : L"");
            }
        }
        catch (...)
        {
        }
    }
}

/// The game's fade tick on the screen-filter slot, then, once per engine tick, each player's fade on the same frame time.
void FilterTick(void* slot, float dt) noexcept
{
    g_tick(slot, dt);
    if (g_fadesTicked)
    {
        return;
    }
    g_fadesTicked = true;
    ++g_ticksSeen;
    ArmFades();
    for (Fade& fade : g_fades)
    {
        g_tick(fade.bytes.data(), dt);
    }
}

/// Debug once per change of whether a player's view takes less than the game's value.
void Note(int player, bool differs, float own, float game) noexcept
{
    bool& was = g_differs[static_cast<std::size_t>(player)];
    if (was == differs)
    {
        return;
    }
    was = differs;
    platform::LogDebug(L"low health: {}'s view takes {} red edge ({:.2f}; the game's {:.2f})", players::SeatName(player),
                       differs ? L"its own" : L"the game's", own, game);
}

/// The screen filter's apply to one view: a player's view gets the entry with that player's own intensity (a copy,
/// never above the game's); any other view, or one view only, the game's entry as it is.
void Apply(void* functor, void* settings, float weight) noexcept
{
    const auto game = reinterpret_cast<ApplyFn>(g_apply.Original());
    if (!g_perView || !g_split)
    {
        game(functor, settings, weight);
        return;
    }
    std::int32_t id         = -1;
    float        gameValue  = 0.0f;
    const auto   view       = reinterpret_cast<std::uintptr_t>(settings) - f::kViewSettings;
    const auto   entry      = reinterpret_cast<std::uintptr_t>(functor);
    const int    player     = Read(view + f::kViewPlayerIndex, id) ? rules::ViewPlayer(id, g_playerOneId, g_seatIds) : -1;
    if (player < 0 || !Read(entry + f::kFunctorIntensity, gameValue))
    {
        ++g_viewsUnknown;
        game(functor, settings, weight);
        return;
    }
    const float own  = g_fades[static_cast<std::size_t>(player)].Get(f::kFilterIntensity);
    const float take = rules::ViewIntensity(own, gameValue);
    Note(player, take < gameValue, own, gameValue);
    alignas(16) std::array<std::byte, f::kFunctorSize> copy{};
    if (!(take < gameValue) || !platform::CopyGuarded(copy.data(), functor, copy.size()))
    {
        game(functor, settings, weight);
        return;
    }
    const float on = take > 0.0f ? 1.0f : 0.0f;
    std::memcpy(copy.data() + f::kFunctorIntensity, &take, sizeof(take));
    std::memcpy(copy.data() + f::kFunctorOn, &on, sizeof(on));
    ++g_viewsOwn;
    game(copy.data(), settings, weight);
}

/// A seat that stopped playing: its counts given back and its controller's danger cleared, so its next decision turns
/// the filter on again when it is in danger.
void Release(int seat, const wchar_t* why) noexcept
{
    const auto at = static_cast<std::size_t>(seat);
    if (g_holds[at] == 0 && DangerOf(seat) == 0)
    {
        return;
    }
    GiveBack(seat, g_holds[at]);
    const std::uintptr_t controller = ControllerOf(seat);
    const std::int32_t   none       = 0;
    if (controller != 0)
    {
        platform::WriteGuarded(reinterpret_cast<void*>(controller + f::kCmdDanger), &none, sizeof(none));
    }
    platform::LogDebug(L"low health: {}'s count of the red edge given back ({})", players::SeatName(seat), why);
}

bool PlaceSites(const platform::GameImage& image) noexcept
{
    const auto place = [&](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && g_placement.Place(call, *live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    // The fades first, then each seat's own count, then the decision per player: a decision per player on the game's
    // one latch would turn player 1's edge off.
    const bool placed = place(g_tickCall, f::kFilterTickCall, g_tickTarget, &FilterTick) &&
                        place(g_dyingOnCall, f::kDangerDyingOnCall, g_onTarget, &FilterOn) &&
                        place(g_stoneOnCall, f::kDangerStoneOnCall, g_onTarget, &FilterOn) &&
                        place(g_dyingOffCall, f::kDangerDyingOffCall, g_offTarget, &FilterOff) &&
                        place(g_stoneOffCall, f::kDangerStoneOffCall, g_offTarget, &FilterOff) &&
                        place(g_hpCall, f::kDangerHpCall, g_hpTarget, &HpForDecidingController);
    if (!placed)
    {
        for (auto* call : {&g_hpCall, &g_stoneOffCall, &g_dyingOffCall, &g_stoneOnCall, &g_dyingOnCall, &g_tickCall})
        {
            call->Restore();
        }
        g_apply.Restore();
        g_installed = false;
        platform::Log(L"low health: a call is not what the research read; one player's low health lights the red edge in "
                      L"every view");
        return false;
    }
    platform::Log(L"low health: installed{}; each player's view fades the red edge on that player's own danger (an AI "
                  L"member's on player 1's)",
                  g_placement.FromGameThread() ? L" from the game thread" : L"");
    return true;
}

std::wstring Reading()
{
    const std::uintptr_t manager = FilterManager();
    std::uint8_t         latch   = 0;
    std::int32_t         count   = 0;
    std::uintptr_t       slot    = 0;
    float                game    = 0.0f;
    Read(manager + f::kFilterLatch + f::kFilterDying, latch);
    Read(manager + f::kFilterCount + 4 * f::kFilterDying, count);
    Read(battle::command_menu_tick::UiSystem() + f::kUIScreenFilter, slot);
    const bool read = slot != 0 && Read(slot + f::kFilterIntensity, game);
    std::wstring text = std::format(L"{}; {}; the game's edge {}; the Dying latch {} count {}",
                                    g_perView ? L"each view its own" : L"the game's (off)",
                                    g_split ? L"several views" : L"one view",
                                    read ? std::format(L"{:.2f}", game) : std::wstring(L"unread"), latch, count);
    for (int player = 0; player < rules::kPlayers; ++player)
    {
        const auto at = static_cast<std::size_t>(player);
        text += std::format(L"; {}: danger {}, holds {}, fade {:.2f} toward {:.2f}", players::SeatName(player), DangerOf(player),
                            g_holds[at], g_fades[at].Get(f::kFilterIntensity), g_fades[at].Get(f::kFilterFadeTarget));
    }
    return text + std::format(L"; HUD ticks {}, seat decisions run {}, views given their own {}, views no player's {}",
                              g_ticksSeen, g_decisions, g_viewsOwn, g_viewsUnknown);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto hp     = platform::FindFunction(image, f::kPartyHp);
    const auto on     = platform::FindFunction(image, f::kFilterOn);
    const auto off    = platform::FindFunction(image, f::kFilterOff);
    const auto inc    = platform::FindFunction(image, f::kRefInc);
    const auto dec    = platform::FindFunction(image, f::kRefDec);
    const auto decide = platform::FindFunction(image, f::kDangerDecision);
    const auto tick   = platform::FindFunction(image, f::kFilterTick);
    const auto apply  = platform::FindFunction(image, f::kFilterApply);
    if (!hp || !on || !off || !inc || !dec || !decide || !tick || !apply || battle::command_menu_tick::UiSystem() == 0)
    {
        platform::Log(L"low health: an engine check failed; one player's low health lights the red edge in every view");
        return false;
    }
    g_hpTarget   = *hp;
    g_onTarget   = *on;
    g_offTarget  = *off;
    g_tickTarget = *tick;
    g_refInc     = reinterpret_cast<FilterCountFn>(*inc);
    g_refDec     = reinterpret_cast<FilterCountFn>(*dec);
    g_decide     = reinterpret_cast<DecisionFn>(*decide);
    g_tick       = reinterpret_cast<FilterTickFn>(*tick);
    // The view's apply first: it passes the game's entry through until the fades and the decisions run.
    if (!g_apply.Replace(image, f::kFilterApplySlot, f::kFilterApply.address, reinterpret_cast<std::uintptr_t>(&Apply)))
    {
        platform::Log(L"low health: the screen filter's apply is not what the research read; one player's low health lights "
                      L"the red edge in every view");
        return false;
    }
    g_installed = true;
    if (!g_placement.Install(image, &PlaceSites, true))
    {
        g_apply.Restore();
        g_installed = false;
        platform::Log(L"low health: the calls could not be queued; one player's low health lights the red edge in every view");
        return false;
    }
    return true;
}

void Remove() noexcept
{
    g_placement.Cancel();
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Release(seat, L"the mod unloads");  // the count calls are guarded and thread-agnostic: no edge left up
    }
    for (auto* call : {&g_hpCall, &g_stoneOffCall, &g_dyingOffCall, &g_stoneOnCall, &g_dyingOnCall, &g_tickCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"low health: a call was changed after us; left in place (it passes through)");
        }
    }
    g_apply.Restore();
    g_installed = false;
}

void Tick(const std::array<bool, players::kMaxPlayers>& plays, bool splitShown) noexcept
{
    g_fadesTicked = false;
    if (!g_installed || !g_hpCall.IsInstalled())
    {
        return;
    }
    g_playerOneId = input::pad_sticks::ControllerIdOf(players::generation::PlayerOneController());
    bool anyPlays = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto at   = static_cast<std::size_t>(seat);
        const auto type = plays[at] ? session::party_member::PlayerTypeOf(players::generation::MemberOf(seat)) : std::nullopt;
        g_held[at]      = type ? *type : rules::kNoType;
        g_seatIds[at]   = type ? input::pad_sticks::ControllerIdOf(players::generation::ControllerOf(seat)) : -2;
        anyPlays        = anyPlays || type.has_value();
        if (!type || !g_perView)
        {
            Release(seat, !type ? L"the player plays no member" : L"the switch is off");
        }
    }
    g_split = splitShown && anyPlays;
    if (!g_perView)
    {
        return;
    }
    // Each playing seat's decision on its own controller once a tick (in a fight its pass decides again, finding no
    // change), also while one view shows, so its count follows its danger through a scene. Before it: player 1's
    // allowed byte on the seat's controller (an event hides the edge on player 1's only, and the seat's decision then
    // gives its count back), and the seat's holds brought to its danger value (the game's own reset of a controller,
    // at a fight's end, zeroes the value through the latch and leaves the count).
    std::uint8_t allowed = 1;
    if (const std::uintptr_t one = ControllerOf(0); one != 0)
    {
        Read(one + f::kCmdDangerAllowed, allowed);
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const std::uintptr_t controller = ControllerOf(seat);
        if (g_held[static_cast<std::size_t>(seat)] == rules::kNoType || controller == 0)
        {
            continue;
        }
        std::uint8_t own = allowed;
        if (Read(controller + f::kCmdDangerAllowed, own) && own != allowed)
        {
            platform::WriteGuarded(reinterpret_cast<void*>(controller + f::kCmdDangerAllowed), &allowed, sizeof(allowed));
        }
        const std::uint8_t extra =
            g_holds[static_cast<std::size_t>(seat)] & static_cast<std::uint8_t>(~rules::HoldsFor(DangerOf(seat)));
        if (extra != 0)
        {
            GiveBack(seat, extra);
            platform::LogDebug(L"low health: {}'s count given back to match its controller (its danger value was reset)",
                               players::SeatName(seat));
        }
        g_deciding = seat;
        if (platform::CallGuarded(g_decide, reinterpret_cast<void*>(controller)))
        {
            ++g_decisions;
        }
        g_deciding = -1;
    }
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"low-health-per-view",
                       .shape = control::Shape::OnOff,
                       .help  = L"the low-health red edge in each player's own view, fading on that player's own danger (on); "
                                L"off: the game's whole-party edge in every view",
                       .set   = [](const control::Value& value) {
                           g_perView = std::get<bool>(value);
                           platform::Log(L"low health: {}", g_perView ? L"each view takes its own player's red edge"
                                                                     : L"every view takes the game's whole-party red edge");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_perView}; }});
    control::Register({.name  = L"hud.low-health",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the game's red edge, each player's danger, count and own fade, and the views given "
                                L"their own since the start",
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
}

#endif

}  // namespace lss::hud::low_health
