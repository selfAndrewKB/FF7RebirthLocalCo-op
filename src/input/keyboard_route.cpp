#include "input/keyboard_route.h"

#include "facts/engine.h"
#include "input/keyboard_route_rules.h"
#include "input/pad_sticks.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::input::keyboard_route {

namespace {

namespace rules = keyboard_route_rules;

using InputKeyFn     = bool (*)(void* self, const void* args);
using InputAxisFn    = bool (*)(void* self, void* viewport, std::int32_t controllerId, void* key, float delta, float deltaTime,
                                std::int32_t samples, bool gamepad);
using IsGamepadKeyFn = bool (*)(const void* key);

// Set once at install.
platform::VTableEntry g_keySlot;
platform::VTableEntry g_axisSlot;
IsGamepadKeyFn        g_isGamepadKey = nullptr;
bool                  g_installed    = false;

// Game thread.
std::array<rules::DownKey, 32> g_down{};  // the keys down, each with the id its press went on with
std::uint32_t           g_keysRouted = 0, g_axesRouted = 0;

/// The controller id the keyboard and mouse go on with: the keyboard's player's while it plays player 2 or 3 alone,
/// else -1 (the engine's own id, player 1's). Only the keyboard's own id, player 1's, is changed.
std::int32_t RoutedId(std::int32_t controllerId) noexcept
{
    if (!players::generation::OnGameThread() || controllerId != 0)
    {
        return -1;
    }
    const std::int32_t keyboard = pad_sticks::KeyboardControllerId();
    return keyboard > 0 ? keyboard : -1;
}

bool InputKey(void* self, const void* args) noexcept
{
    const auto    original     = reinterpret_cast<InputKeyFn>(g_keySlot.Original());
    const auto*   bytes        = static_cast<const std::byte*>(args);
    std::int32_t  controllerId = 0;
    std::uint64_t name         = 0;
    std::uint8_t  event        = 0;
    if (!players::generation::OnGameThread() || !platform::Read(bytes + facts::kInputKeyArgsControllerId, controllerId) ||
        !platform::Read(bytes + facts::kInputKeyArgsKey, name) || !platform::Read(bytes + facts::kInputKeyArgsEvent, event) ||
        g_isGamepadKey(bytes + facts::kInputKeyArgsKey))
    {
        return original(self, args);
    }
    const rules::KeyEvent kind = event == facts::kInputEventPressed    ? rules::KeyEvent::Press
                                 : event == facts::kInputEventReleased ? rules::KeyEvent::Release
                                                                       : rules::KeyEvent::Other;
    const std::int32_t    routed = rules::KeyId(g_down, name, kind, controllerId, RoutedId(controllerId));
    if (routed == controllerId)
    {
        return original(self, args);
    }
    // The arguments are const: a copy with the keyboard's player's id, alive for the call (the key's reference stays
    // the caller's; the callee copies the key with its own reference if it keeps it).
    std::array<std::byte, facts::kInputKeyArgsBytes> copy{};
    if (!platform::CopyGuarded(copy.data(), args, copy.size()))
    {
        return original(self, args);
    }
    std::memcpy(copy.data() + facts::kInputKeyArgsControllerId, &routed, sizeof(routed));
    ++g_keysRouted;
    return original(self, copy.data());
}

bool InputAxis(void* self, void* viewport, std::int32_t controllerId, void* key, float delta, float deltaTime,
               std::int32_t samples, bool gamepad) noexcept
{
    const auto         original = reinterpret_cast<InputAxisFn>(g_axisSlot.Original());
    const std::int32_t routed   = gamepad ? -1 : RoutedId(controllerId);
    if (routed >= 0)
    {
        ++g_axesRouted;
        controllerId = routed;
    }
    // The key by value is the caller's copy, handed on as it came: the callee destroys it.
    return original(self, viewport, controllerId, key, delta, deltaTime, samples, gamepad);
}

#if LSS_DEV
std::wstring Reading()
{
    return std::format(L"{}; the keyboard and mouse go on with controller id {}; keys and buttons routed {}, axes routed {}",
                       g_installed ? L"on" : L"not installed", pad_sticks::KeyboardControllerId(), g_keysRouted,
                       g_axesRouted);
}
#endif

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto keyEntry  = image.Rebase(facts::kViewportInputKeyEntry, sizeof(void*));
    const auto key       = image.Rebase(facts::kViewportInputKey);
    const auto axisEntry = image.Rebase(facts::kViewportInputAxisEntry, sizeof(void*));
    const auto axis      = image.Rebase(facts::kViewportInputAxis);
    const auto gamepad   = platform::FindFunction(image, facts::kIsGamepadKey);
    const bool checked   = keyEntry && key && axisEntry && axis && gamepad &&
                         platform::FindFunction(image, facts::kViewportInputKeyAfterFirst) &&
                         platform::FindFunction(image, facts::kViewportInputAxisAfterFirst);
    if (checked)
    {
        g_isGamepadKey = reinterpret_cast<IsGamepadKeyFn>(*gamepad);  // known before either entry is ours
    }
    g_installed = checked && g_keySlot.Replace(*keyEntry, *key, reinterpret_cast<std::uintptr_t>(&InputKey)) &&
                  g_axisSlot.Replace(*axisEntry, *axis, reinterpret_cast<std::uintptr_t>(&InputAxis));
    if (!g_installed && g_keySlot.Original() != 0)
    {
        g_keySlot.Restore();
    }
    if (g_installed)
    {
        platform::Log(L"keyboard route: while the keyboard plays player 2 or 3, its keys, the mouse's buttons, wheel and "
                      L"look reach that player's controller (the viewport's own input entries, by controller id)");
    }
    else
    {
        platform::LogWarning(L"keyboard route: the viewport's input entries could not be taken (a check failed): the "
                             L"mouse look stays player 1's");
    }
    return g_installed;
}

void Remove() noexcept
{
    if (g_axisSlot.Original() != 0 && !g_axisSlot.Restore())
    {
        platform::Log(L"keyboard route: the viewport's InputAxis entry was changed after us; left in place (it passes "
                      L"through)");
    }
    if (g_keySlot.Original() != 0 && !g_keySlot.Restore())
    {
        platform::Log(L"keyboard route: the viewport's InputKey entry was changed after us; left in place (it passes "
                      L"through)");
    }
    g_installed = false;
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"players.keyboard-route",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) which controller id the keyboard and mouse reach the viewport with, and how "
                                L"many keys and axes went to the keyboard's player",
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

}  // namespace lss::input::keyboard_route
