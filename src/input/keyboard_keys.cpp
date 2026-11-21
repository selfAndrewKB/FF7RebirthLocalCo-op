#include "input/keyboard_keys.h"

#include "facts/controller.h"
#include "input/pad_rules.h"
#include "platform/byte_scope.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "views/player_view_point.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>


namespace lss::input::keyboard_keys {

namespace {

namespace rules = pad_rules;

using KeyFn       = std::uint8_t (*)(void* self, void* application, const void* keyEvent);
using PointerFn   = std::uint8_t (*)(void* self, void* application, const void* pointerEvent);
using GateFn      = std::uint8_t (*)(void* self, void* application);
using CopyEventFn = void* (*)(void* to, const void* from);
using FreeEventFn = void* (*)(void* event);

// Set once at install.
platform::VTableEntry g_downSlot;
platform::VTableEntry g_upSlot;
platform::VTableEntry g_mouseDownSlot;
platform::VTableEntry g_wheelSlot;
KeyFn                 g_keyToActions = nullptr;
GateFn                g_inputGate    = nullptr;
CopyEventFn           g_copyEvent    = nullptr;
FreeEventFn           g_freeEvent    = nullptr;
bool                  g_installed    = false;

// A short Tab's press, given to the game at the let-go: its release follows a frame later, so the game's per-frame
// "pressed" reads see the press (a press and release in one frame read as neither).
struct TapRelease
{
    alignas(8) std::array<std::byte, facts::kKeyEventBytes> event{};  // a copy of Tab's key-up event, alive while due
    void* self        = nullptr;
    void* application = nullptr;
    int   due         = 0;  // polls left before the release: 2 at the let-go (that frame's poll), 0 for none
};
TapRelease g_tapRelease;

std::atomic<pad_table::Listener> g_listener{nullptr};

// Game thread (Slate's key events and the session's tick).
std::bitset<256> g_heldBack;          // by key code: a down the mod took from the game, whose up is the mod's too
bool             g_idle        = false;
bool             g_tabDown     = false;
bool             g_tabFired    = false;  // the opener fired on this press: no tap at its let-go
bool             g_tabInWindow = false;  // the press came while the window was open: its tap is the window's (nothing)
std::int64_t     g_tabSince    = 0;
bool             g_leaveDown   = false;
bool             g_leaveSeen   = false;  // Backspace went down since the last poll (a tap between two polls counts)
std::uint16_t    g_pressed     = 0;      // the window's keys pressed since the last poll, as the rules' bits
rules::Held      g_leave{};
std::int32_t     g_releaseUser = -1;
std::uint32_t    g_opens = 0, g_taps = 0, g_windowKeys = 0, g_flagsKept = 0;

KeyFn OriginalDown() noexcept
{
    return reinterpret_cast<KeyFn>(g_downSlot.Original());
}

KeyFn OriginalUp() noexcept
{
    return reinterpret_cast<KeyFn>(g_upSlot.Original());
}

/// A keyboard key event's virtual-key code and its system repeat flag; false for a pad button's event (the same
/// handlers carry those), off the game thread, or unreadable.
bool ReadKey(const void* keyEvent, std::uint32_t& keyCode, bool& repeat) noexcept
{
    std::uint8_t repeatByte = 0;
    if (!players::generation::OnGameThread() ||
        !platform::Read(static_cast<const std::byte*>(keyEvent) + facts::kKeyEventKeyCode, keyCode) ||
        !platform::Read(static_cast<const std::byte*>(keyEvent) + facts::kKeyEventRepeat, repeatByte))
    {
        return false;
    }
    repeat = repeatByte != 0;
    return rules::IsKeyboardKey(keyCode);
}

/// Runs `call` (a key, mouse button or wheel event in the game's handler) with the game's "keyboard used" flag put back
/// as it was, while the keyboard plays player 2 or 3 and views are split: the flag makes the keyboard the last device
/// for everyone (every view's button art, player 1's rumble), which belongs to player 1's device there (hud/button_art
/// gives the keyboard player's view its keycaps). With one view on screen, the keyboard moves the game's art as any
/// device does.
template <class Call>
std::uint8_t KeepingKeyboardFlag(Call&& call) noexcept
{
    platform::ByteStore<1, 1> store;
    const auto owner = reinterpret_cast<std::uintptr_t>(
        players::game_globals::Object(players::game_globals::Global::SquareInputOwner));
    if (owner != 0 && pad_table::KeyboardSeat() > 0 && views::SplitShown())
    {
        const platform::ByteContext::Range flag{owner + facts::kInputKeyboardUsed, 1, platform::ByteContext::Kind::Keep};
        store.Define({&flag, 1});
    }
    platform::ByteScope scope(store);
    const std::uint8_t  answer = call();
    g_flagsKept += scope.Release() != 0 ? 1u : 0u;
    return answer;
}

/// A key-up handed to the game's own handler, with its sender noted for the battle menu's release route: a pad's
/// controller id, or kKeyboardRelease for a key (player 1's pad shares the keyboard's Slate user).
std::uint8_t ReleaseToGame(void* self, void* application, const void* keyEvent) noexcept
{
    const std::int32_t before  = g_releaseUser;
    std::uint32_t      keyCode = 0;
    std::int32_t       user    = -1;
    if (platform::Read(static_cast<const std::byte*>(keyEvent) + facts::kKeyEventKeyCode, keyCode) &&
        rules::IsKeyboardKey(keyCode))
    {
        g_releaseUser = kKeyboardRelease;
    }
    else
    {
        g_releaseUser = platform::Read(static_cast<const std::byte*>(keyEvent) + facts::kKeyEventUser, user) ? user : -1;
    }
    const std::uint8_t answer = OriginalUp()(self, application, keyEvent);
    g_releaseUser             = before;
    return answer;
}

/// A short Tab's press to the game at its let-go (`keyEvent`, the key-up: the same key), the way the game's own key
/// repeat presses a key, and its release kept for a later poll. Nothing while the game's input gate is shut.
void TapToGame(void* self, void* application, const void* keyEvent) noexcept
{
    if (g_tapRelease.due != 0 || g_inputGate(self, application) == 0)
    {
        return;
    }
    g_copyEvent(g_tapRelease.event.data(), keyEvent);
    g_tapRelease.self        = self;
    g_tapRelease.application = application;
    g_tapRelease.due         = 2;
    ++g_taps;
    KeepingKeyboardFlag([&] { return g_keyToActions(self, application, g_tapRelease.event.data()); });
}

/// The kept release, once its frame has passed; or at once (`now`), at the mod's end.
void ReleaseTap(bool now) noexcept
{
    if (g_tapRelease.due == 0 || (!now && --g_tapRelease.due > 0))
    {
        return;
    }
    g_tapRelease.due = 0;
    ReleaseToGame(g_tapRelease.self, g_tapRelease.application, g_tapRelease.event.data());
    g_freeEvent(g_tapRelease.event.data());
}

void Tell(std::uint16_t pressed, bool open, bool bTap, bool bHeld) noexcept
{
    if (const pad_table::Listener listener = g_listener.load(std::memory_order_relaxed); listener != nullptr)
    {
        listener(rules::kKeyboardPad, pressed, open, bTap, bHeld);
    }
}

/// A key down the mod takes from the game: Tab (the opener, its system repeats too), and every key while the window is
/// open but a repeat of one held from before it opened (its down reached the game, so its up must too).
bool TakesDown(std::uint32_t keyCode, bool repeat) noexcept
{
    return keyCode == rules::kOpenKey || (g_idle && !(repeat && !g_heldBack.test(keyCode)));
}

/// Square's key-down handler, the mod's first: Tab held back (the opener), and every key while the window is open. A
/// pad button's event, and every event once the mod has ended (a slot it could not put back), pass as they came.
std::uint8_t KeyDown(void* self, void* application, const void* keyEvent) noexcept
{
    std::uint32_t keyCode = 0;
    bool          repeat  = false;
    if (!g_installed || !ReadKey(keyEvent, keyCode, repeat) || !TakesDown(keyCode, repeat))
    {
        return KeepingKeyboardFlag([&] { return OriginalDown()(self, application, keyEvent); });
    }
    g_heldBack.set(keyCode);
    if (keyCode == rules::kOpenKey)
    {
        if (!g_tabDown)
        {
            g_tabDown     = true;
            g_tabFired    = false;
            g_tabInWindow = g_idle;
            g_tabSince    = pad_table::NowMs();
        }
    }
    else if (!repeat)
    {
        g_pressed = static_cast<std::uint16_t>(g_pressed | rules::FromKey(keyCode));
        if (keyCode == rules::kLeaveKey)
        {
            g_leaveDown = true;
            g_leaveSeen = true;
        }
        ++g_windowKeys;
    }
    return 1;
}

/// Square's key-up handler: a held-back key's up is the mod's (Tab's after a short press gives the game its press);
/// every other up reaches the game, with its sender noted for the battle menu's release route.
std::uint8_t KeyUp(void* self, void* application, const void* keyEvent) noexcept
{
    std::uint32_t keyCode = 0;
    bool          repeat  = false;
    if (g_installed && ReadKey(keyEvent, keyCode, repeat) && g_heldBack.test(keyCode))
    {
        g_heldBack.reset(keyCode);
        if (keyCode == rules::kOpenKey)
        {
            const bool tap = g_tabDown && !g_tabFired && !g_tabInWindow && !g_idle;
            g_tabDown      = false;
            g_tabFired     = false;
            if (tap)
            {
                TapToGame(self, application, keyEvent);
            }
        }
        else if (keyCode == rules::kLeaveKey)
        {
            g_leaveDown = false;
        }
        return 1;
    }
    return ReleaseToGame(self, application, keyEvent);
}
/// Square's mouse-button-down or wheel handler (the one `Slot` held): passed to the game's, with the keyboard flag kept
/// as above.
template <platform::VTableEntry& Slot>
std::uint8_t PointerEvent(void* self, void* application, const void* pointerEvent) noexcept
{
    const auto original = reinterpret_cast<PointerFn>(Slot.Original());
    return KeepingKeyboardFlag([&] { return original(self, application, pointerEvent); });
}

#if LSS_DEV
std::wstring Reading()
{
    return std::format(L"{}; window {}, Tab {}; opener held {} time(s), map taps given {}, window keys {}; the keyboard "
                       L"flag put back after {} event(s) of player 2's or 3's keyboard",
                       g_installed ? L"on" : L"not installed", g_idle ? L"open" : L"closed",
                       g_tabDown ? L"down" : L"up", g_opens, g_taps, g_windowKeys, g_flagsKept);
}
#endif

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto downEntry = image.Rebase(facts::kSquareKeyDownEntry, sizeof(void*));
    const auto down      = image.Rebase(facts::kSquareKeyDown);
    const auto upEntry   = image.Rebase(facts::kSquareKeyUpEntry, sizeof(void*));
    const auto up        = image.Rebase(facts::kSquareKeyUp);
    const auto toActions = platform::FindFunction(image, facts::kSquareKeyToActions);
    const auto gate      = platform::FindFunction(image, facts::kSquareInputGate);
    const auto copy      = platform::FindFunction(image, facts::kKeyEventCopy);
    const auto destroy   = platform::FindFunction(image, facts::kKeyEventDestroy);
    const bool checked   = downEntry && down && upEntry && up && toActions && gate && copy && destroy &&
                         platform::FindFunction(image, facts::kSquareKeyDownAfterFirst) &&
                         platform::FindFunction(image, facts::kSquareKeyUpAfterFirst);
    if (checked)
    {
        // Known before either slot is ours.
        g_keyToActions = reinterpret_cast<KeyFn>(*toActions);
        g_inputGate    = reinterpret_cast<GateFn>(*gate);
        g_copyEvent    = reinterpret_cast<CopyEventFn>(*copy);
        g_freeEvent    = reinterpret_cast<FreeEventFn>(*destroy);
    }
    g_installed = checked && g_downSlot.Replace(*downEntry, *down, reinterpret_cast<std::uintptr_t>(&KeyDown)) &&
                  g_upSlot.Replace(*upEntry, *up, reinterpret_cast<std::uintptr_t>(&KeyUp));
    if (!g_installed && g_downSlot.Original() != 0)
    {
        g_downSlot.Restore();
    }
    // The mouse's two handlers only keep the keyboard flag: the keyboard plays on without them.
    const auto mouseEntry = image.Rebase(facts::kSquareMouseDownEntry, sizeof(void*));
    const auto mouse      = image.Rebase(facts::kSquareMouseDown);
    const auto wheelEntry = image.Rebase(facts::kSquareWheelEntry, sizeof(void*));
    const auto wheel      = image.Rebase(facts::kSquareWheel);
    const bool mouseTaken = g_installed && mouseEntry && mouse && wheelEntry && wheel &&
                            platform::FindFunction(image, facts::kSquareMouseDownAfterFirst) &&
                            platform::FindFunction(image, facts::kSquareWheelAfterFirst) &&
                            g_mouseDownSlot.Replace(*mouseEntry, *mouse, reinterpret_cast<std::uintptr_t>(&PointerEvent<g_mouseDownSlot>)) &&
                            g_wheelSlot.Replace(*wheelEntry, *wheel, reinterpret_cast<std::uintptr_t>(&PointerEvent<g_wheelSlot>));
    if (!mouseTaken && g_mouseDownSlot.Original() != 0)
    {
        g_mouseDownSlot.Restore();
    }
    if (g_installed && !mouseTaken)
    {
        platform::LogWarning(L"keyboard: the game's mouse handlers could not be taken (a check failed): with the keyboard "
                             L"on player 2 or 3, a mouse click still turns every view's button art to keycaps");
    }
    if (g_installed)
    {
        platform::Log(L"keyboard: Tab held a second opens the keyboard's player's window, or adds a player on the keyboard "
                      L"(a shorter Tab still opens the map at its let-go); while that window is open every key is its "
                      L"own (the arrows trade player numbers, Q/E move the view, Enter or Escape done, Backspace held: "
                      L"leave)");
    }
    else
    {
        platform::LogWarning(L"keyboard: the game's key handlers could not be taken (a check failed): the keyboard opens "
                             L"no player window and plays beside player 1");
    }
    return g_installed;
}

void Remove() noexcept
{
    g_listener.store(nullptr);
    g_idle = false;
    if (g_installed)
    {
        ReleaseTap(true);  // a short Tab's press never outlives the mod
    }
    // From here every key passes as it came, should a slot stay ours (changed after us).
    g_installed = false;
    g_heldBack.reset();
    g_tabDown   = false;
    g_tabFired  = false;
    g_leaveDown = false;
    g_leaveSeen = false;
    if (g_wheelSlot.Original() != 0 && !g_wheelSlot.Restore())
    {
        platform::Log(L"keyboard: the wheel slot was changed after us; left in place (it passes through)");
    }
    if (g_mouseDownSlot.Original() != 0 && !g_mouseDownSlot.Restore())
    {
        platform::Log(L"keyboard: the mouse-button slot was changed after us; left in place (it passes through)");
    }
    if (g_upSlot.Original() != 0 && !g_upSlot.Restore())
    {
        platform::Log(L"keyboard: the key-up slot was changed after us; left in place (it passes through)");
    }
    if (g_downSlot.Original() != 0 && !g_downSlot.Restore())
    {
        platform::Log(L"keyboard: the key-down slot was changed after us; left in place (it passes through)");
    }
}

bool Installed() noexcept
{
    return g_installed;
}

void Listen(pad_table::Listener listener) noexcept
{
    g_listener.store(listener);
}

void Poll() noexcept
{
    if (!g_installed)
    {
        return;
    }
    ReleaseTap(false);
    const std::int64_t now  = pad_table::NowMs();
    const bool         open = g_tabDown && !g_tabFired && now - g_tabSince >= rules::kOpenHoldMs;
    if (open)
    {
        g_tabFired = true;
        ++g_opens;
        platform::Log(L"keyboard: Tab held 1 s");
    }
    const rules::Long   leave   = rules::Hold(g_leave, g_leaveDown || std::exchange(g_leaveSeen, false), now,
                                              rules::kLeaveHoldMs);
    const std::uint16_t pressed = std::exchange(g_pressed, 0);
    if (pressed != 0 || open || leave != rules::Long::None)
    {
        Tell(pressed, open, leave == rules::Long::Tap, leave == rules::Long::Hold);
    }
}

void SetIdle(bool idle) noexcept
{
    if (idle == g_idle)
    {
        return;
    }
    g_idle = idle;
    if (!idle)
    {
        g_pressed   = 0;
        g_leaveDown = false;
        g_leaveSeen = false;
        g_leave     = {};
    }
    platform::LogDebug(L"keyboard: {}", idle ? L"its player's window is open: every key is the window's"
                                             : L"its player's window closed: the keys reach the game again");
}

std::int32_t ReleaseUser() noexcept
{
    return g_releaseUser;
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"players.keyboard",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the keyboard as a player's device: its window, Tab, and the opens, map taps "
                                L"and window keys so far",
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

}  // namespace lss::input::keyboard_keys
