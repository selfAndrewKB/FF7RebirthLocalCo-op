#include "input/pad_table.h"

#include "facts/engine.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/seats.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Xinput.h>

namespace lss::input::pad_table {

namespace {

namespace rules = pad_rules;

using GetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);
using SetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_VIBRATION* vibration);

constexpr int kSlots = rules::kSlots;
static_assert(kSlots == players::kXInputUsers, "the table answers exactly XInput's users (the mirror views' ids follow them)");

/// One physical pad, as the game's poll last read it. Game thread (the poll and the console both run there).
struct Pad
{
    rules::OpenHold hold;      // View: held back from the game; a second down opens the player's window
    rules::Held     leave;     // B: a window's close (a tap) or its leave (a hold)
    rules::LetGo    letGo;     // idle to the game until it lets go of what it held at a change or its idle's end
    std::uint16_t   last = 0;  // the buttons on its last poll (the listener's presses)
};

platform::VTableEntry g_getEntry;
platform::VTableEntry g_setEntry;
GetStateFn            g_get        = nullptr;  // what the import slots held: XInput's own, or whatever was there first
SetStateFn            g_set        = nullptr;
std::uintptr_t        g_appGlobal  = 0;
std::uintptr_t        g_sonyVTable = 0;
bool                  g_installed  = false;

std::array<std::atomic<int>, kSlots> g_table{0, 1, 2, 3};
std::atomic<Listener>                g_listener{nullptr};
std::atomic<std::uint32_t>           g_idleUsers{0};  // bit N: XInput user N reads idle to the game
std::atomic<std::uint32_t>           g_playing{1};    // the slots a player holds (SetPlaying)

// Game thread.
std::array<Pad, kSlots> g_pads{};
std::uint32_t           g_opens = 0, g_waited = 0, g_rechecks = 0, g_changes = 0, g_startsHeld = 0;
std::uint32_t           g_unseenSlots    = 0;  // the slots whose pad answered while the engine counted them empty (bit N)
std::uint32_t           g_unseenRechecks = 0;  // the re-checks set for them
bool                    g_recheckFailing = false;  // the last re-check write failed (its warning said)
std::wstring            g_last           = L"none yet";

std::int64_t NowMs() noexcept
{
    return static_cast<std::int64_t>(GetTickCount64());
}

/// "pad 2" for XInput user 1: the numbering the `pads` reading uses.
std::wstring PadName(int user)
{
    return std::format(L"pad {}", user + 1);
}

std::wstring TableText(const rules::PadTable& table)
{
    return std::format(L"player 1 {}, player 2 {}, player 3 {}, spare {}", PadName(table[0]), PadName(table[1]),
                       PadName(table[2]), PadName(table[3]));
}

/// The engine's XInputDevice (FWindowsApplication+0xD8), or null.
std::uint8_t* XInputDevice() noexcept
{
    void* const app = g_appGlobal != 0 ? platform::ReadPointerAt(g_appGlobal) : nullptr;
    return static_cast<std::uint8_t*>(app != nullptr ? platform::ReadPointer(app, facts::kAppXInputInterface) : nullptr);
}

/// The engine's own "poll every slot again" byte, as its WM_DEVICECHANGE handling sets it.
bool FlagRecheck() noexcept
{
    std::uint8_t* const xi = XInputDevice();
    const std::uint8_t  on = 1;
    if (xi == nullptr || !platform::WriteGuarded(xi + facts::kXInputNeedsStateUpdate, &on, 1))
    {
        if (!g_recheckFailing)  // said once per run of failures (RecheckAnswering asks every second)
        {
            platform::LogWarning(L"pads: the engine's pad re-check could not be set; a pad moved onto a slot it counted "
                                 L"empty is read after the next device change");
        }
        g_recheckFailing = true;
        return false;
    }
    g_recheckFailing = false;
    ++g_rechecks;
    return true;
}

/// Every slot's last rumble sent forgotten, so the game's next poll sends each slot's values again (it sends only
/// when they change): to the pad that now answers the slot.
void ForgetSentRumble() noexcept
{
    std::uint8_t* const xi = XInputDevice();
    if (xi == nullptr)
    {
        return;
    }
    const float never = -1.0f;  // no motor value the game computes: the next compare sees a change
    for (int slot = 0; slot < kSlots; ++slot)
    {
        std::uint8_t* const state = xi + facts::kXInputControllerStates + static_cast<std::uint32_t>(slot) * facts::kXInputControllerStateBytes;
        platform::WriteGuarded(state + facts::kXInputStateLastLarge, &never, sizeof(never));
        platform::WriteGuarded(state + facts::kXInputStateLastSmall, &never, sizeof(never));
    }
}

void Idle(XINPUT_STATE& state) noexcept
{
    state.Gamepad = XINPUT_GAMEPAD{};
}

/// The game's poll of `slot`: the assigned pad's state, gated (see the header).
DWORD WINAPI GetStateFor(DWORD slot, XINPUT_STATE* state) noexcept
{
    if (slot >= static_cast<DWORD>(kSlots) || state == nullptr)
    {
        return g_get(slot, state);
    }
    const int   user   = g_table[slot].load(std::memory_order_relaxed);
    const DWORD result = g_get(static_cast<DWORD>(user), state);
    // The listener, the opener, the let-go and the Start gate follow the stock poll, on the game thread; Square's
    // pad-type rescan and its last-active-pad check read through the same slot for the result alone and get the
    // assigned pad as it is (idle or not).
    const bool gameThread = players::generation::OnGameThread();
    Pad&       pad        = g_pads[static_cast<std::size_t>(user)];
    if (result != ERROR_SUCCESS)
    {
        *state = XINPUT_STATE{};  // the engine reads the buffer as left after a failure (RE-pad-slots.md)
        if (gameThread)
        {
            pad.hold  = {};  // a View held back as it dropped out is not a tap on its return
            pad.leave = {};
            pad.last  = 0;
        }
        return result;
    }
    const auto         raw = state->Gamepad.wButtons;
    const std::int64_t now = NowMs();
    rules::Gated       gated{raw, false};
    std::uint16_t      pressed = 0;  // the buttons down on this poll and up on the last
    if (gameThread)
    {
        // Every pad's new presses, the opener and B's tap or hold go to the listener, an idle pad's too (its window's
        // keys); the listener only notes them (a table change inside the poll would lose its re-check).
        gated                     = rules::Gate(pad.hold, raw, now);
        pressed                   = static_cast<std::uint16_t>(raw & ~pad.last);
        const rules::Long b       = rules::Hold(pad.leave, (raw & rules::kB) != 0, now, rules::kLeaveHoldMs);
        pad.last                  = raw;
        if (const Listener listener = g_listener.load(std::memory_order_relaxed);
            listener != nullptr && (pressed != 0 || gated.open || b != rules::Long::None))
        {
            listener(user, pressed, gated.open, b == rules::Long::Tap, b == rules::Long::Hold);
        }
        if (gated.open)
        {
            ++g_opens;
            platform::Log(L"pads: View held 1 s on pad {} (player {}'s slot)", user + 1, slot + 1);
        }
    }
    if ((g_idleUsers.load(std::memory_order_relaxed) >> user & 1u) != 0)
    {
        Idle(*state);
        return result;
    }
    if (!gameThread)
    {
        return result;
    }
    const std::uint8_t left  = state->Gamepad.bLeftTrigger;
    const std::uint8_t right = state->Gamepad.bRightTrigger;
    const rules::Wait  wait  = rules::Poll(pad.letGo, raw, left, right, now);
    if (wait != rules::Wait::None)
    {
        Idle(*state);  // idle on the poll that ends the wait too
        if (wait == rules::Wait::LetGoNow)
        {
            platform::LogDebug(L"pads: pad {} let go: it reaches the game again (player {}'s slot)", user + 1, slot + 1);
        }
        else if (wait == rules::Wait::TriggerRests)
        {
            platform::Log(L"pads: pad {}'s triggers rest at {} and {} (of 255) 2 s after its buttons let go: it reaches "
                          L"the game again (player {}'s slot)",
                          user + 1, left, right, slot + 1);
        }
        else if (wait == rules::Wait::ButtonStuck)
        {
            platform::Log(L"pads: pad {} still holds buttons {:#06x} 10 s after the change: it reaches the game again "
                          L"(player {}'s slot)",
                          user + 1, raw, slot + 1);
        }
        return result;
    }
    // A pad that plays no one never pauses the game: its Start, heard by the listener, joins (session/player_window).
    // View reaches the game only as a tap at its let-go (the opener holds it back).
    const std::uint16_t shown = rules::StartGate(gated.buttons, static_cast<int>(slot), g_playing.load(std::memory_order_relaxed));
    if ((pressed & rules::kStart) != 0 && (shown & rules::kStart) == 0)
    {
        ++g_startsHeld;
        platform::LogDebug(L"pads: pad {}'s Start held back from the game (player {}'s slot, no player there): a join",
                           user + 1, slot + 1);
    }
    state->Gamepad.wButtons = shown;
    return result;
}

/// The game's rumble for `slot`: to the assigned pad; off while that pad is idle.
DWORD WINAPI SetStateFor(DWORD slot, XINPUT_VIBRATION* vibration) noexcept
{
    if (slot >= static_cast<DWORD>(kSlots))
    {
        return g_set(slot, vibration);
    }
    const auto user = static_cast<DWORD>(g_table[slot].load(std::memory_order_relaxed));
    if ((g_idleUsers.load(std::memory_order_relaxed) >> user & 1u) != 0)
    {
        XINPUT_VIBRATION off{};
        return g_set(user, &off);
    }
    return g_set(user, vibration);
}

/// Every pad still holding what it holds now stays idle to the game until it lets go of that.
void AwaitReleases() noexcept
{
    for (Pad& pad : g_pads)
    {
        rules::AwaitLetGo(pad.letGo);
    }
}

/// The module an address lies in, for the install line.
std::wstring ModuleOf(std::uintptr_t address)
{
    HMODULE module         = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
    {
        return L"no module";
    }
    const std::wstring full(path);
    const auto         slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

/// Logs the engine's other input devices (a PlayStation pad read through scePad is not moved by the table). At install.
void LogDevices() noexcept
{
    void* const  app   = g_appGlobal != 0 ? platform::ReadPointerAt(g_appGlobal) : nullptr;
    void* const  data  = app != nullptr ? platform::ReadPointer(app, facts::kAppInputDevices) : nullptr;
    std::int32_t count = 0;
    if (app == nullptr || !platform::Read(app, facts::kAppInputDevices + sizeof(void*), count) || count < 0 || count > 64)
    {
        platform::LogDebug(L"pads: the engine's other input devices are not readable");
        return;
    }
    int sony = 0;
    for (std::int32_t i = 0; i < count && data != nullptr; ++i)
    {
        // 16-byte shared references: the object pointer first.
        void* const device = platform::ReadPointer(data, static_cast<std::size_t>(i) * 2 * sizeof(void*));
        void* const vtable = device != nullptr ? platform::ReadPointer(device, 0) : nullptr;
        sony += reinterpret_cast<std::uintptr_t>(vtable) == g_sonyVTable ? 1 : 0;
    }
    if (count == 0)
    {
        platform::Log(L"pads: the engine's other input devices are not made yet (its first pad poll makes them); a "
                      L"PlayStation pad it reads through its own reader is not moved by the pad table");
        return;
    }
    platform::Log(L"pads: the engine lists {} other input device(s){}", count,
                  sony > 0 ? L", one the PlayStation pad reader: a PlayStation pad it reads is not moved by the pad table"
                           : L", none the PlayStation pad reader");
}

/// "pad 1, pad 3" for the idle bits.
std::wstring IdleText(std::uint32_t users)
{
    std::wstring text;
    for (int user = 0; user < kSlots; ++user)
    {
        if ((users >> user & 1u) != 0)
        {
            text += (text.empty() ? L"" : L", ") + PadName(user);
        }
    }
    return text;
}

#if LSS_DEV
std::wstring Reading()
{
    const std::uint32_t idle = g_idleUsers.load();
    return std::format(L"{}{}; {}; slots with a player {:#x}; View held {}, Starts held back {}, waits for a let-go {}, re-checks {} "
                       L"({} for a pad the engine had not seen; such slots now {:#x}), changes {}; last: {}",
                       g_installed ? L"on" : L"not installed",
                       idle != 0 ? std::format(L" (idle to the game: {})", IdleText(idle)) : std::wstring(),
                       TableText(Table()), g_playing.load(), g_opens, g_startsHeld, g_waited, g_rechecks, g_unseenRechecks,
                       g_unseenSlots, g_changes, g_last);
}
#endif

}  // namespace

rules::PadTable Table() noexcept
{
    rules::PadTable table{};
    for (int slot = 0; slot < kSlots; ++slot)
    {
        table[static_cast<std::size_t>(slot)] = g_table[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
    }
    return table;
}

bool Set(const rules::PadTable& table, const wchar_t* why) noexcept
try
{
    if (!g_installed || !rules::IsPermutation(table))
    {
        return false;
    }
    const rules::PadTable before = Table();
    if (before == table)
    {
        return true;
    }
    for (int slot = 0; slot < kSlots; ++slot)
    {
        g_table[static_cast<std::size_t>(slot)].store(table[static_cast<std::size_t>(slot)], std::memory_order_relaxed);
    }
    AwaitReleases();
    ++g_waited;
    ++g_changes;
    FlagRecheck();
    // Every pad's motors off (a pad keeps the last rumble it was sent, and its slot may no longer be the one the game
    // sends it for), and every slot's last send forgotten, so the game's next send per slot sets the new owner's.
    for (int user = 0; user < kSlots; ++user)
    {
        XINPUT_VIBRATION off{};
        g_set(static_cast<DWORD>(user), &off);
    }
    ForgetSentRumble();
    g_last = std::format(L"{} (was {}): {}", TableText(table), TableText(before), why);
    platform::Log(L"pads: {}", g_last);
    return true;
}
catch (...)
{
    return false;
}

void SetIdleUsers(std::uint32_t users, const wchar_t* why) noexcept
try
{
    users &= (1u << kSlots) - 1u;
    const std::uint32_t before = g_idleUsers.exchange(users);
    if (!g_installed || before == users)
    {
        return;
    }
    const std::uint32_t back = before & ~users;
    for (int user = 0; user < kSlots; ++user)
    {
        if ((back >> user & 1u) != 0)
        {
            rules::AwaitLetGo(g_pads[static_cast<std::size_t>(user)].letGo);  // the press that ended it stays out
            ++g_waited;
        }
    }
    ForgetSentRumble();  // an idle pad's rumble was sent as off: its slot's next values reach it again
    g_last = std::format(L"idle to the game: {}: {}", users != 0 ? IdleText(users) : std::wstring(L"none"), why);
    platform::Log(L"pads: {}", g_last);
}
catch (...)
{
}

void Listen(Listener listener) noexcept
{
    g_listener.store(listener, std::memory_order_relaxed);
}

void SetPlaying(std::uint32_t slots) noexcept
{
    g_playing.store(slots | 1u, std::memory_order_relaxed);
}

void RecheckAnswering(std::uint32_t answeringUsers) noexcept
try
{
    const std::uint8_t* const xi = XInputDevice();
    if (xi == nullptr)
    {
        return;
    }
    std::uint32_t unseen = 0;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const int    user      = g_table[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
        std::uint8_t connected = 0;
        if ((answeringUsers >> user & 1u) == 0)
        {
            continue;
        }
        if (!platform::Read(xi, facts::kXInputControllerStates + static_cast<std::size_t>(slot) * facts::kXInputControllerStateBytes +
                                    facts::kXInputStateConnected,
                            connected))
        {
            return;  // unreadable: nothing decided on it
        }
        unseen |= connected == 0 ? 1u << slot : 0u;
    }
    if (unseen != 0 && FlagRecheck())
    {
        ++g_unseenRechecks;
    }
    if (unseen == g_unseenSlots)
    {
        return;
    }
    const std::uint32_t before = g_unseenSlots;
    g_unseenSlots              = unseen;
    for (int slot = 0; slot < kSlots; ++slot)
    {
        const bool now = (unseen >> slot & 1u) != 0;
        if (now == ((before >> slot & 1u) != 0))
        {
            continue;
        }
        const int user = g_table[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
        if (now)
        {
            platform::Log(L"pads: pad {} answers on player {}'s slot but the engine counts that slot empty (no device "
                          L"change it heard since the pad came or moved): the engine's re-check set, so its next poll "
                          L"reads it",
                          user + 1, slot + 1);
        }
        else
        {
            platform::LogDebug(L"pads: the engine reads player {}'s slot again (pad {})", slot + 1, user + 1);
        }
    }
}
catch (...)
{
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto     getSlot = image.Rebase(facts::kXInputGetStateImport, sizeof(void*));
    const auto     setSlot = image.Rebase(facts::kXInputSetStateImport, sizeof(void*));
    const auto     app     = image.Rebase(facts::kWindowsApplicationGlobal, sizeof(void*));
    const auto     sony    = image.Rebase(facts::kSonyControllerDeviceVTable, sizeof(void*));
    std::uintptr_t get = 0, set = 0;
    if (!getSlot || !setSlot || !app || !sony || !platform::Read(*getSlot, get) || !platform::Read(*setSlot, set) ||
        get == 0 || set == 0)
    {
        platform::Log(L"pads: an engine check failed; each pad stays the player with its XInput number");
        return false;
    }
    g_appGlobal  = *app;
    g_sonyVTable = *sony;
    g_get        = reinterpret_cast<GetStateFn>(get);
    g_set        = reinterpret_cast<SetStateFn>(set);
    if (!g_getEntry.Replace(*getSlot, get, reinterpret_cast<std::uintptr_t>(&GetStateFor)))
    {
        platform::Log(L"pads: the game's pad read could not be taken; each pad stays the player with its XInput number");
        return false;
    }
    if (!g_setEntry.Replace(*setSlot, set, reinterpret_cast<std::uintptr_t>(&SetStateFor)))
    {
        g_getEntry.Restore();
        platform::Log(L"pads: the game's rumble call could not be taken; each pad stays the player with its XInput number");
        return false;
    }
    g_installed = true;
    try
    {
        platform::Log(L"pads: the game's pad read and rumble go through the pad table ({}; read through {}, rumble through "
                      L"{})",
                      TableText(Table()), ModuleOf(get), ModuleOf(set));
    }
    catch (...)
    {
    }
    LogDevices();
    return true;
}

void Remove() noexcept
{
    g_listener.store(nullptr);
    g_idleUsers.store(0);
    g_playing.store(~0u);  // every Start through, should a slot stay ours (a replace not put back)
    g_setEntry.Restore();
    g_getEntry.Restore();
    if (g_installed)
    {
        for (int slot = 0; slot < kSlots; ++slot)
        {
            g_table[static_cast<std::size_t>(slot)].store(slot, std::memory_order_relaxed);
        }
        FlagRecheck();
        ForgetSentRumble();
    }
    g_installed = false;
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Shape;
    using control::Value;
    control::Register({.name  = L"pad-table",
                       .shape = Shape::Text,
                       .help  = L"which pad each player has: four pad numbers for players 1, 2, 3 and the spare slot, "
                                L"each 1-4 once (`pad-table 2 1 3 4`: pad 2 plays player 1, pad 1 player 2)",
                       .set   = [](const Value& value) {
                           const auto&     text = std::get<std::wstring>(value);
                           rules::PadTable table{};
                           int             count = 0;
                           for (const wchar_t c : text)
                           {
                               if (c >= L'1' && c <= L'4' && count < kSlots)
                               {
                                   table[static_cast<std::size_t>(count++)] = c - L'1';
                               }
                               else if (c != L' ' && c != L',')
                               {
                                   count = kSlots + 1;
                               }
                           }
                           if (count != kSlots || !rules::IsPermutation(table))
                           {
                               return Outcome{false, L"four pad numbers 1-4, each once"};
                           }
                           if (!Set(table, L"the console"))
                           {
                               return Outcome{false, L"the pad table is not installed (see the startup log)"};
                           }
                           return Outcome{true, TableText(Table())};
                       },
                       .get = [] { return Value{TableText(Table())}; }});
    control::Register({.name  = L"pads-idle",
                       .shape = Shape::OnOff,
                       .help  = L"every connected pad reads idle to the game, rumble off (off: each pad holding a "
                                L"button stays idle until it lets go)",
                       .set   = [](const Value& value) {
                           SetIdleUsers(std::get<bool>(value) ? (1u << kSlots) - 1u : 0u, L"the console");
                           return Outcome{};
                       },
                       .get = [] { return Value{g_idleUsers.load() != 0}; }});
    control::Register({.name  = L"players.pad-table",
                       .shape = Shape::Text,
                       .help  = L"(read only) the pad table, the slots with a player, the Starts held back, the waits "
                                L"for a let-go and the re-checks",
                       .get   = [] {
                           try
                           {
                               return Value{Reading()};
                           }
                           catch (...)
                           {
                               return Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
}

#endif

}  // namespace lss::input::pad_table
