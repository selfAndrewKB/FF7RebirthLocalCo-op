#include "battle/menu_input_route.h"

#include "battle/command_menu.h"
#include "battle/command_menu_tick_shared.h"
#include "battle/focus_scope.h"
#include "battle/menu_rules.h"
#include "facts/command_controller.h"
#include "facts/controller.h"
#include "facts/hud.h"
#include "facts/in_battle.h"
#include "facts/menu.h"
#include "facts/party.h"
#include "input/button_view.h"
#include "input/keyboard_keys.h"
#include "input/pad_buttons.h"
#include "input/pad_sticks.h"
#include "input/pad_table.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/tset_find.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/seat_ladder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::battle::menu_input_route {

namespace seat_ladder = scopes::seat_ladder;

namespace {

// The CommandMenu action's key (logged). An action event's key name is at +0x18 (the handler compares
// it with the FKeys' first eight bytes). Target switching (Prev/NextTarget) goes to the seat's menu
// like every other action: its handler switches the target fields a TargetScope holds as the seat's
// (research/RE-p2-targeting-scoped-tick.md).
constexpr std::uintptr_t kCommandMenuKey = 0x14901C5D0;
constexpr std::uint32_t  kEventKey       = 0x18;
// The up route's call in ActionUp_UpdateTables (bool(owner, event)): research/RE-p2-command-menu-input.md. Each release's
// pad comes from the input processor's key-up handler, which input/keyboard_keys holds.
constexpr std::uint8_t  kUpRouteBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                           0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50, 0xFE, 0xFF, 0xFF};
constexpr platform::FunctionFact kUpRoute{L"SquareInput__RouteActionUpToUIHandlerStack", 0x1409F84C0, kUpRouteBytes};
constexpr std::uintptr_t kUpRouteCall = 0x1409F81C8;

// The action keys (FKeys) the route and its log compare an event's key name with.
enum Action : std::size_t
{
    kAttack,
    kUnique,
    kGuard,
    kDodge,
    kActions
};
constexpr std::array<std::uintptr_t, kActions> kActionKeys{facts::kKeyAttack, facts::kKeyUniqueAbility,
                                                           facts::kKeyGuard, facts::kKeyDodge};

using facts::kUICommandController;
using facts::kUIInput;
using facts::kPartyManagerLeaderType;
using facts::kCmdMembers;
using facts::kCmdBattleInput;
using facts::kCmdMenuLayer;
using command_menu_tick::kReqAttack;
using command_menu_tick::kReqUnique;
using command_menu_tick::kReqGuardDodge;
using command_menu_tick::kReqGuard;
using command_menu_tick::RequestOf;
using command_menu_tick::kReqOpenMenu;

using UpRouteFn       = std::uint8_t (*)(void* owner, void* event);

// Set once at install.
platform::CallRedirect               g_upRouteCall;
std::array<std::uintptr_t, kActions> g_keys{};
std::uintptr_t                       g_commandMenuKey = 0;

// Game thread.
unsigned     g_routed      = 0;  // the seats' presses dispatched in their focus contexts
std::array<unsigned, 5> g_routedKnown{};  // per action the log counts on its own (KnownAction)
bool         g_routeFailed = false;
unsigned     g_upRouted    = 0;   // the seats' releases routed to their menus
unsigned     g_upLost      = 0;   // releases whose press went to a seat's UI but could not follow it
const wchar_t* g_uiRefusal = L"";  // why InSeatUi last ran nothing (for the log)
// Each seat's keys whose press went to its UI (by seat; index 0 unused): two seats holding the same action keep apart.
std::array<menu_rules::TakenKeys, players::kMaxPlayers> g_taken{};

/// The seat whose device the source bit belongs to (the seat's pad is its number, and the keyboard is the seat's while
/// it plays it: input::button_view::SeatSources, the same sources input/pad_buttons routes for the seat), or 0.
int SeatOfSource(std::uint32_t sourceBit) noexcept
{
    const int keyboard = input::pad_table::KeyboardSeat();
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (((input::button_view::SeatSources(seat, keyboard) >> (sourceBit & 31)) & 1) != 0)
        {
            return seat;
        }
    }
    return 0;
}

/// The seat whose controller has controller id `pad`, or 0.
int SeatOfPad(std::int32_t pad) noexcept
{
    for (int seat = 1; pad >= 0 && seat <= players::kSeatsBuilt; ++seat)
    {
        if (pad == input::pad_sticks::ControllerIdOf(players::generation::ControllerOf(seat)))
        {
            return seat;
        }
    }
    return 0;
}

/// An event line of `level` (under the tick's name: the route is its).
void Line(const std::wstring& text, platform::Level level = platform::Level::Info) noexcept
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"command menu tick: {}", text);
    }
}

/// Which of the actions the log counts on its own a key is (0 CommandMenu, 1 Attack, 2 Unique, 3 Guard,
/// 4 Dodge), or -1.
int KnownAction(std::uint64_t key) noexcept
{
    const std::array<std::uintptr_t, 5> keys{g_commandMenuKey, g_keys[kAttack], g_keys[kUnique], g_keys[kGuard],
                                             g_keys[kDodge]};
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i] != 0 && ReadAt<std::uint64_t>(keys[i]) == key)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// A name for the log: the action keys it knows, else the raw key name.
std::wstring KeyText(std::uint64_t key)
{
    const std::array<std::pair<std::uintptr_t, const wchar_t*>, 5> known{
        {{g_commandMenuKey, L"CommandMenu"}, {g_keys[kAttack], L"Attack"}, {g_keys[kUnique], L"UniqueAbility"},
         {g_keys[kGuard], L"Guard"}, {g_keys[kDodge], L"Dodge"}}};
    for (const auto& [address, name] : known)
    {
        if (address != 0 && ReadAt<std::uint64_t>(address) == key)
        {
            return name;
        }
    }
    return players::unreal::NameText(key);
}

/// What InSeatUi runs: a press (gated as below) or the release of a press that went to the seat's UI
/// (ungated: the release goes to whoever took the press, as the game's capture map decides).
enum class Route
{
    kPress,
    kRelease
};

/// Runs `call` (a guarded game call, true when it did not fault) in `seat`'s UI: the seat's focus
/// context and scopes. A press runs only while the seat's controller ticks, with the seat's menu layer in place and
/// player 1's top layer the command menu's own (a pause menu or popup keeps the seat's pad out of every menu). False,
/// running nothing, when those do not hold; true once it ran (a fault stops the route for the session).
template <class Call>
bool InSeatUi(int seat, Route route, const wchar_t* what, Call&& call) noexcept
{
    const auto refuse = [](const wchar_t* why) {
        g_uiRefusal = why;
        return false;
    };
    if (!command_menu_tick::Installed() || g_routeFailed || command_menu_tick::Pass() != 0 ||
        !players::generation::OnGameThread() || !players::SeatBuilt(seat) || !players::generation::Active(seat))
    {
        return refuse(L"the route is off, inside a pass, or the player is out");
    }
    const auto own    = reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat));
    const auto one    = ReadAt<std::uintptr_t>(command_menu_tick::UiSystem() + kUICommandController);
    void*      member = players::generation::MemberOf(seat);
    if (own == 0 || one == 0 || one == own || member == nullptr)
    {
        return refuse(L"a controller or the player's member is missing");
    }
    if (route == Route::kPress &&
        !menu_rules::PressAllowed(command_menu_tick::SeatTicks(seat), ReadAt<std::uint8_t>(own + kCmdMenuLayer),
                                  ReadAt<std::uint8_t>(one + kCmdMenuLayer), focus_scope::PlayerOneTopIsCommandMenu()))
    {
        return refuse(L"the player's menu is not ticking, or another menu is on top");
    }
    seat_ladder::Args args;
    args.seat          = seat;
    args.controller    = reinterpret_cast<void*>(own);
    const auto manager = ReadAt<std::uintptr_t>(command_menu_tick::PartyManager());
    if (!seat_ladder::FillMember(args, member) || manager == 0)
    {
        return refuse(L"the player's member could not be read");
    }
    args.leaderType = ReadAt<std::uint8_t>(manager + kPartyManagerLeaderType);
    // The target fields are optional where menu_rules says a press does not need them.
    const bool needsTargets = menu_rules::PressNeedsTargets(route == Route::kPress, facts::InBattle());
    seat_ladder::SeatScopes scopes(seat_ladder::Focus | seat_ladder::Primary | seat_ladder::Targets |
                                       seat_ladder::Leader | seat_ladder::LeaderType | seat_ladder::Rules,
                                   needsTargets ? 0u : seat_ladder::Targets, args);
    if (!scopes.Held())
    {
        return refuse(scopes.Refusal());
    }
    command_menu_tick::SetPass(3, seat);
    const bool ran = call();
    command_menu_tick::SetPass(0, 0);
    if (!ran)
    {
        scopes.Get<seat_ladder::FocusPart>()->scope.Abandon();
        g_routeFailed = true;
        Line(std::format(L"FAILED: pad {}'s {} faulted in {}'s menu; players 2 and 3's pads stay out of the menus",
                         seat + 1, what, players::SeatName(seat)));
    }
    return true;
}

/// For the route log: a seat's battle input, its action requests, and player 1's capture map's count.
std::wstring RouteState(std::uintptr_t own)
{
    const auto input    = ReadAt<std::uintptr_t>(command_menu_tick::UiSystem() + kUIInput);
    const auto captures = input + facts::kInputCaptureMap;
    return std::format(L"25C {} req 6:{} 7:{} 12:{} 13:{} 26:{} 32:{} 37:{} | members {} {} {} | captures {}",
                       ReadAt<std::uint8_t>(own + kCmdBattleInput), RequestOf(own, 6), RequestOf(own, 7),
                       RequestOf(own, kReqAttack), RequestOf(own, kReqUnique), RequestOf(own, kReqOpenMenu),
                       RequestOf(own, kReqGuardDodge), RequestOf(own, kReqGuard),
                       ReadAt<std::int32_t>(own + kCmdMembers), ReadAt<std::int32_t>(own + kCmdMembers + 4),
                       ReadAt<std::int32_t>(own + kCmdMembers + 8),
                       ReadAt<std::int32_t>(captures + platform::kSetCount) -
                           ReadAt<std::int32_t>(captures + platform::kSetFreeCount));
}

/// For a CommandMenu press's log line: the route's gates and the game's field press and open gates on the seat's
/// controller (research/RE-p2-field-command-menu.md). Outside the seat's UI (player 1's top layer is read as is).
std::wstring CommandMenuGates(int seat, std::uintptr_t own)
{
    const auto one = ReadAt<std::uintptr_t>(command_menu_tick::UiSystem() + kUICommandController);
    return std::format(L"menu ticks {}, menu layer {} (player 1's {}), player 1's top layer the command menu's {}, field "
                       L"menu {}, menu allowed {}, set up {}, battle HUD {}, open request {}, open pending {}",
                       command_menu_tick::SeatTicks(seat) ? 1 : 0, ReadAt<std::uint8_t>(own + kCmdMenuLayer),
                       one != 0 ? ReadAt<std::uint8_t>(one + kCmdMenuLayer) : 0xFF,
                       focus_scope::PlayerOneTopIsCommandMenu() ? 1 : 0,
                       ReadAt<std::uint8_t>(own + facts::kCmdFieldMenuAllowed),
                       ReadAt<std::uint8_t>(own + facts::kCmdMenuAllowed), ReadAt<std::uint8_t>(own + facts::kCmdSetupDone),
                       ReadAt<std::uint8_t>(own + facts::kCmdHudMode), RequestOf(own, kReqOpenMenu),
                       ReadAt<std::int32_t>(own + facts::kCmdOpenPending));
}

/// Notes that `seat`'s pad's press of `key` went to the seat's UI: its release goes there too.
void NoteTaken(int seat, std::uint64_t key) noexcept
{
    if (g_taken[static_cast<std::size_t>(seat)].NoteTaken(key) == menu_rules::TakenKeys::Note::Full)
    {
        Line(std::format(L"WARNING: more pad-{} presses held in {}'s UI than noted; a release may reach player 1's "
                         L"route",
                         seat + 1, players::SeatName(seat)));
    }
}

/// True (and forgotten) if `seat`'s pad's press of `key` went to the seat's UI.
bool TakeTaken(int seat, std::uint64_t key) noexcept
{
    return players::SeatIndex(seat) && g_taken[static_cast<std::size_t>(seat)].Take(key);
}

/// A seat's pad's press through the game's own dispatcher in the seat's UI, so its route, list and
/// command-menu handler act on the seat's menu (research/RE-p2-command-menu-input.md). The seat comes
/// from the press's source bit.
bool RouteSeatPad(input::pad_buttons::ActionDownFn dispatch, void* owner, void* unused, std::uint32_t sourceBit, void* event,
                  std::uint8_t alreadyHandled, char& result) noexcept
{
    const auto key = ReadAt<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event) + kEventKey);
    const int  seat = SeatOfSource(sourceBit);
    if (key == 0 || seat == 0)
    {
        return false;
    }
    char taken = 1;  // after a fault: not dispatched again in player 1's context
    const int  action = KnownAction(key);
    const auto own    = reinterpret_cast<std::uintptr_t>(command_menu::Controller(seat));
    std::wstring before;
    const bool routed = InSeatUi(seat, Route::kPress, L"press", [&] {
        const bool logIt = platform::Enabled(platform::Level::Trace) && action >= 0 &&
                           g_routedKnown[static_cast<std::size_t>(action)] < 10;
        if (logIt)
        {
            before = RouteState(own);
        }
        const bool ran = platform::CallGuarded(dispatch, taken, owner, unused, sourceBit, event, alreadyHandled);
        if (logIt)
        {
            Line(std::format(L"pad {}'s {} in {}'s UI: before [{}] after [{}]", seat + 1, KeyText(key),
                             players::SeatName(seat), before, RouteState(own)),
                 platform::Level::Trace);
        }
        return ran;
    });
    if (action == 0 && own != 0 && platform::Enabled(platform::Level::Debug))
    {
        try
        {
            Line(std::format(L"pad {}'s CommandMenu press {} ({}): {}", seat + 1,
                             routed ? L"went to their menu" : L"was refused",
                             routed ? (taken != 0 ? L"taken" : L"not taken") : g_uiRefusal, CommandMenuGates(seat, own)),
                 platform::Level::Debug);
        }
        catch (...)
        {
        }
    }
    if (!routed)
    {
        return false;
    }
    NoteTaken(seat, key);
    result = taken;
    if (platform::Enabled(platform::Level::Trace) &&
        (action >= 0 ? ++g_routedKnown[static_cast<std::size_t>(action)] <= 10 : (++g_routed <= 20 || g_routed % 500 == 0)))
    {
        Line(std::format(L"pad {}'s {} went to {}'s menu (taken: {}; {} press(es))", seat + 1, KeyText(key),
                         players::SeatName(seat), taken != 0 ? 1 : 0, g_routed),
             platform::Level::Trace);
    }
    return true;
}

/// Replaces the up route's call: the release of a seat's press that went to its UI walks the seat's focus context,
/// whatever is open now (a pause menu, a popup, the battle's end), so the seat's capture map hands it to the widget
/// that took the press and its menu clears its own held requests, as the game's up handler does. Other releases keep
/// the game's route, where their press went. The seat comes from the release's pad; a key's release, or one with no
/// sender (a mouse button's), is the keyboard's seat's while the keyboard plays one.
std::uint8_t UpRoute(void* owner, void* event) noexcept
{
    const auto         original = reinterpret_cast<UpRouteFn>(g_upRouteCall.Original());
    const std::int32_t user     = input::keyboard_keys::ReleaseUser();
    const int          seat     = user >= 0 ? SeatOfPad(user) : input::pad_table::KeyboardSeat();
    const auto key      = ReadAt<std::uint64_t>(reinterpret_cast<std::uintptr_t>(event) + kEventKey);
    if (seat <= 0 || !TakeTaken(seat, key))
    {
        return original(owner, event);
    }
    std::uint8_t answer = 0;
    if (InSeatUi(seat, Route::kRelease, L"release",
                 [&] { return platform::CallGuarded(original, answer, owner, event); }))
    {
        ++g_upRouted;
        return answer;
    }
    // The seat's UI could not be entered (the seat left, or a scope was refused): the release is not given to player
    // 1's menu, which never took the press. The game's own paths clear the seat's held level: ClearOneShotRequests
    // while its battle input is off, and the battle-entry HUD mode.
    if (++g_upLost <= 10 || g_upLost % 100 == 0)
    {
        Line(std::format(L"pad {}'s release of {} could not reach {}'s UI ({}); dropped ({} time(s))", seat + 1,
                         KeyText(key), players::SeatName(seat), g_uiRefusal, g_upLost));
    }
    return 0;
}

}  // namespace

bool Find(const platform::GameImage& image) noexcept
{
    const auto commandMenu = image.Rebase(kCommandMenuKey, 24);
    if (!commandMenu)
    {
        return false;
    }
    for (std::size_t i = 0; i < kActions; ++i)
    {
        const auto key = image.Rebase(kActionKeys[i], 24);
        if (!key)
        {
            return false;
        }
        g_keys[i] = *key;
    }
    g_commandMenuKey = *commandMenu;
    return true;
}

void Install(const platform::GameImage& image) noexcept
{
    input::pad_buttons::SetSeatRoute(&RouteSeatPad);  // a seat's presses to its menu
    // A seat's releases: the up route's call redirected, each release's pad from the key-up handler keyboard_keys holds.
    const auto upRoute = platform::FindFunction(image, kUpRoute);
    const auto site    = image.Rebase(kUpRouteCall, 5);
    const bool ok      = upRoute && site && input::keyboard_keys::Installed() &&
                    g_upRouteCall.Install(*site, *upRoute, reinterpret_cast<std::uintptr_t>(&UpRoute));
    platform::Log(L"command menu tick: players 2 and 3's pads' releases {}",
                  ok ? L"reach their menus" : L"UNAVAILABLE (a check failed): they keep player 1's up route");
}

void Remove() noexcept
{
    input::pad_buttons::SetSeatRoute(nullptr);
    if (g_upRouteCall.IsInstalled() && !g_upRouteCall.Restore())
    {
        platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::battle::menu_input_route
