#include "battle/keyboard_menu_keys.h"

#include "battle/command_menu.h"
#include "input/pad_table.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"

#include <cstdint>
#include <utility>

namespace lss::battle::keyboard_menu_keys {

namespace {

// FieldCommandCtrl_CurrentStateId(ctrl) -> i32: the state of the controller's current menu (-1 none, 0 the idle state);
// a read of the controller's own fields. The UI system's tick calls it on its command controller (the slot, player 1's)
// when it sets or clears the movement keys' d-pad bit; that call fits one 16-byte block.
constexpr std::uint8_t kStateBytes[] = {0x83, 0x79, 0x38, 0x00, 0x8B, 0x41, 0x34, 0x0F,
                                        0x4D, 0x41, 0x38, 0x39, 0x41, 0x28, 0x76, 0x36};
constexpr platform::FunctionFact kState{L"FieldCommandCtrl_CurrentStateId", 0x14088DCDC, kStateBytes};
constexpr std::uintptr_t         kStateCall = 0x14088BAB7;
constexpr std::int32_t           kNoMenu    = -1;

using StateFn = std::int32_t (*)(const void* controller);

// Set once at install.
platform::CallRedirect g_stateCall;

// Game thread: the last change logged (the keyboard's seat, -1 for none, and the state its menu answered).
int          g_saidSeat  = -1;
std::int32_t g_saidState = kNoMenu;

/// The game's test of a state: a menu open in a state other than the idle one.
bool Open(std::int32_t state) noexcept
{
    return state != kNoMenu && state != 0;
}

/// At Debug, each change of the state the keyboard player's menu answers (or of who the keyboard plays), with player
/// 1's beside it (read for the line only). Game thread.
void Note(int seat, std::int32_t state, const void* controller, StateFn original) noexcept
{
    if (seat == g_saidSeat && state == g_saidState)
    {
        return;
    }
    const int was = std::exchange(g_saidSeat, seat);
    g_saidState   = state;
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    if (seat < 1)
    {
        if (was >= 1)
        {
            platform::LogAt(platform::Level::Debug,
                            L"keyboard menu keys: the keyboard no longer plays {}: its movement keys follow player 1's "
                            L"command menu",
                            players::SeatName(was));
        }
        return;
    }
    const bool one = controller != nullptr && Open(original(controller));
    platform::LogAt(platform::Level::Debug,
                    L"keyboard menu keys: {}'s command menu {} (state {}): the movement keys {} (player 1's menu {})",
                    players::SeatName(seat), Open(state) ? L"open" : L"closed", state,
                    Open(state) ? L"work its cursor" : L"move their character", one ? L"open" : L"closed");
}

/// Replaces the UI system's tick's call of the state read: while the keyboard plays player 2 or 3, their own
/// controller's state (no menu before it is built); otherwise the game's controller's.
std::int32_t KeyboardMenuState(const void* controller) noexcept
{
    const auto original = reinterpret_cast<StateFn>(g_stateCall.Original());
    if (!players::generation::OnGameThread())
    {
        return original(controller);
    }
    const int seat = input::pad_table::KeyboardSeat();
    if (seat < 1 || seat > players::kSeatsBuilt || !players::generation::Active(seat))
    {
        Note(-1, kNoMenu, controller, original);
        return original(controller);
    }
    const void*        own   = command_menu::Ready(seat) ? command_menu::Controller(seat) : nullptr;
    const std::int32_t state = own != nullptr ? original(own) : kNoMenu;
    Note(seat, state, controller, original);
    return state;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto state = platform::FindFunction(image, kState);
    const auto site  = image.Rebase(kStateCall, 5);
    const bool ok =
        state && site && g_stateCall.Install(*site, *state, reinterpret_cast<std::uintptr_t>(&KeyboardMenuState));
    platform::Log(L"keyboard menu keys: {}",
                  ok ? L"while the keyboard plays player 2 or 3, its movement keys work the cursor of that player's own "
                       L"command menu only"
                     : L"UNAVAILABLE (a check failed): player 1's open command menu turns the keyboard's movement keys "
                       L"into its cursor for every player");
    return ok;
}

void Remove() noexcept
{
    if (g_stateCall.IsInstalled() && !g_stateCall.Restore())
    {
        platform::Log(L"keyboard menu keys: the state read's call was changed after us; left in place (it passes through)");
    }
    g_saidSeat  = -1;
    g_saidState = kNoMenu;
}

}  // namespace lss::battle::keyboard_menu_keys
