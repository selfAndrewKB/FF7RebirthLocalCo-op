#include "hud/field/search_pins.h"

#include "facts/controller.h"
#include "facts/hud.h"
#include "hud/hud_roots.h"
#include "hud/hud_rules.h"
#include "hud/passes/pass_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"
#include "scopes/leader_scope.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::hud::search_pins {

namespace {

// research/RE-p2-ranged-reticle.md (this build). EndUISystem[55] points to the pin manager: +0x00 the compass (weak),
// +0x30 the pins, a TSparseArray of 40-byte entries: +0x00 the pin (weak), +0x08 its world location (FVector), +0x18
// the hook icon (weak).
constexpr std::uint32_t  kPinManager  = 0x1B8;
constexpr std::uint32_t  kCompass     = 0x00;
constexpr std::uint32_t  kPins        = 0x30;
constexpr std::uint32_t  kPinEntry    = 40;
constexpr std::uint32_t  kPinWidget   = 0x00;
constexpr std::uint32_t  kPinLocation = 0x08;
constexpr std::uint32_t  kPinHook     = 0x18;
// The leader-action component (facts/controller.h) and its interact points (the same offsets in 1.005): +0x1490
// data, +0x1498 count, 624-byte points, the point's pin (weak) at +0x1C; +0x14A0 "a point is active" (the byte its
// update compares before opening or closing the compass); the ranged selection's published state (logged).
constexpr std::uint32_t kLeaderActions = facts::kControllerLeaderActions;
constexpr std::uint32_t kPoints        = 0x1490;
constexpr std::uint32_t kPointCount    = 0x1498;
constexpr std::uint32_t kPointSize     = 624;
constexpr std::uint32_t kPointPin      = 0x1C;
constexpr std::uint32_t kAnyActive     = 0x14A0;
constexpr std::uint32_t kShooting      = facts::kLeaderActionShootingState;
constexpr std::size_t   kMaxPoints     = 32;
constexpr std::size_t   kMaxHidden     = 64;

// The per-entry placement the list's tick runs: stateless.
constexpr std::uint8_t kPlacePinBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55,
                                           0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlacePin{L"EndUI__M1B8_PlacePin_OwnerPC_EdgeFade", 0x14374ED08, kPlacePinBytes};
constexpr std::uint8_t kPlaceHookBytes[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x32, 0x01, 0x00, 0x00, 0xB8, 0x28, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlaceHook{L"EndUI__M1B8_PlaceHookIcon_ByAngle", 0x14374EBCC, kPlaceHookBytes};

using PlacePinFn  = std::uint8_t (*)(void* manager, void* pin, const void* location, float* angle);
using PlaceHookFn = void (*)(void* unused, void* hook, const float* angle);

/// One player's component: the pins its interact points hold, and whether a point is active.
struct Held
{
    std::array<std::uint64_t, kMaxPoints> pins{};
    std::size_t                           count  = 0;
    bool                                  active = false;
    std::uint8_t                          shooting = 0;
};

struct Logged
{
    std::size_t  entries = 0, mine = 0, theirs = 0, nobody = 0;
    bool         compass = false;
    unsigned     compassOwners = 0;
    unsigned     active = 0;  // bit `seat`: the seat's component has an active point
    std::array<std::uint8_t, players::kMaxPlayers> shooting{};
    bool operator==(const Logged&) const = default;
};

std::uintptr_t g_system    = 0;  // set once at install
PlacePinFn     g_placePin  = nullptr;
PlaceHookFn    g_placeHook = nullptr;
// Game thread.
/// A pin's owner as last seen: a point that goes inactive releases its pin (EndUI__RemoveFieldTargetSearchPin), which
/// stays in the list through its out-animation with no point holding it; it keeps its owner until it leaves the list.
struct Known
{
    std::uint64_t pin   = 0;
    int           owner = -1;
};
std::array<Known, kMaxHidden> g_known{};
std::size_t                   g_knownCount = 0;
HiddenSet                      g_hidden;  // the other players' pins and the compass, transparent for a pass
unsigned                       g_compassOwners = hud_rules::kEveryPlayer;  // bit `seat` (every player until known)
bool                           g_placeFaulted  = false;
std::array<Logged, players::kMaxPlayers> g_logged{};
std::array<bool, players::kMaxPlayers>   g_loggedOnce{};

using platform::ReadAt;  // the guarded reads: platform/guarded.h

void* Resolve(std::uintptr_t weakField) noexcept
{
    return players::weak_object::ObjectAt(reinterpret_cast<const void*>(weakField));
}

std::uintptr_t Manager() noexcept
{
    return g_system != 0 ? ReadAt<std::uintptr_t>(g_system + kPinManager) : 0;
}

Held HeldBy(void* controller) noexcept
{
    Held       held;
    const auto component = controller != nullptr
                               ? ReadAt<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(controller) + kLeaderActions)
                               : 0;
    if (component == 0)
    {
        return held;
    }
    held.active   = ReadAt<std::uint8_t>(component + kAnyActive) != 0;
    held.shooting = ReadAt<std::uint8_t>(component + kShooting);
    const auto points = ReadAt<std::uintptr_t>(component + kPoints);
    const auto count  = ReadAt<std::int32_t>(component + kPointCount);
    for (std::int32_t i = 0; points != 0 && i < count && held.count < kMaxPoints; ++i)
    {
        if (const auto pin = ReadAt<std::uint64_t>(points + kPointSize * static_cast<std::uintptr_t>(i) + kPointPin);
            pin != 0)
        {
            held.pins[held.count++] = pin;
        }
    }
    return held;
}

bool Holds(const Held& held, std::uint64_t pin) noexcept
{
    for (std::size_t i = 0; i < held.count; ++i)
    {
        if (held.pins[i] == pin)
        {
            return true;
        }
    }
    return false;
}

void Hide(std::uintptr_t weakField) noexcept
{
    g_hidden.Hide(Resolve(weakField), ReadAt<std::uint64_t>(weakField));
}

void LogIfChanged(int mine, const Logged& now, std::uintptr_t manager) noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || (g_loggedOnce[mine] && now == g_logged[mine]))
    {
        return;
    }
    g_logged[mine]     = now;
    g_loggedOnce[mine] = true;
    try
    {
        // The first few pins by name, so the reticle a player sees can be named.
        std::wstring names;
        int          named = 0;
        platform::ForEachSparseElement(manager + kPins, kPinEntry, [&names, &named](std::uintptr_t entry) {
            names += std::format(L"{}{} + {}", named != 0 ? L", " : L"", players::NameOf(Resolve(entry + kPinWidget)),
                                 players::NameOf(Resolve(entry + kPinHook)));
            return ++named < 3;
        });
        std::wstring active;
        for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
        {
            active += std::format(L"{}{} {} (ranged state +0x4E0 {:#x})", seat == 0 ? L"" : L", ",
                                  players::SeatName(seat), players::YesNo((now.active & (1u << seat)) != 0),
                                  now.shooting[static_cast<std::size_t>(seat)]);
        }
        platform::LogDebug(L"search pins: {}'s view: {} pin(s) ({} its player's, {} hidden as another's, {} nobody's) "
                           L"[{}]; compass {} (owners {}); point active: {}",
                           players::SeatName(mine), now.entries, now.mine, now.theirs, now.nobody, names,
                           now.compass ? L"open" : L"none", now.compassOwners, active);
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto system = players::game_globals::Address(players::game_globals::Global::UiSystem);
    const auto pin    = platform::FindFunction(image, kPlacePin);
    const auto hook   = platform::FindFunction(image, kPlaceHook);
    g_system          = system;
    g_placePin        = pin ? reinterpret_cast<PlacePinFn>(*pin) : nullptr;
    g_placeHook       = hook ? reinterpret_cast<PlaceHookFn>(*hook) : nullptr;
    platform::Log(L"search pins: each view's pins placed from its own camera: {}; each view shows only its own player's "
                  L"pins: {}",
                  g_system != 0 && g_placePin != nullptr && g_placeHook != nullptr ? L"ready" : L"UNAVAILABLE (a check failed)",
                  g_system == 0 ? L"UNAVAILABLE (no pin list)"
                  : !players::widget_opacity::Installed()
                      ? L"UNAVAILABLE (the opacity setter is not what the research read)"
                      : L"ready");
}

void Adopt() noexcept
{
    const auto manager = Manager();
    if (manager == 0)
    {
        return;
    }
    try
    {
        hud_roots::Adopt(static_cast<RC::Unreal::UObject*>(Resolve(manager + kCompass)));
        platform::ForEachSparseElement(manager + kPins, kPinEntry, [](std::uintptr_t entry) {
            hud_roots::Adopt(static_cast<RC::Unreal::UObject*>(Resolve(entry + kPinWidget)));
            hud_roots::Adopt(static_cast<RC::Unreal::UObject*>(Resolve(entry + kPinHook)));
            return true;
        });
    }
    catch (...)
    {
    }
}

void Place() noexcept
{
    const auto manager = Manager();
    if (manager == 0 || g_placePin == nullptr || g_placeHook == nullptr || g_placeFaulted)
    {
        return;
    }
    try
    {
        platform::ForEachSparseElement(manager + kPins, kPinEntry, [manager](std::uintptr_t entry) {
            void* pin = Resolve(entry + kPinWidget);
            if (pin == nullptr)
            {
                return true;
            }
            float        angle  = 0.0f;
            std::uint8_t inside = 0;
            if (!platform::CallGuarded(g_placePin, inside, reinterpret_cast<void*>(manager), pin,
                                       reinterpret_cast<const void*>(entry + kPinLocation), &angle) ||
                !platform::CallGuarded(g_placeHook, static_cast<void*>(nullptr), Resolve(entry + kPinHook),
                                       static_cast<const float*>(&angle)))
            {
                g_placeFaulted = true;
                platform::Log(L"search pins: FAILED: the game's pin placement faulted; the pins keep player 1's placement");
                return false;
            }
            return true;
        });
    }
    catch (...)
    {
    }
}

void ShowOnly(int seat) noexcept
{
    Restore();
    const auto manager = Manager();
    if (manager == 0 || !players::widget_opacity::Installed() || seat < 0 || seat >= players::kMaxPlayers)
    {
        return;
    }
    const int mine = seat;
    // Player 1's component and each built seat's ([0] player 1's).
    std::array<Held, players::kMaxPlayers> held{};
    held[0] = HeldBy(players::generation::PlayerOneController());
    for (int s = 1; s <= players::kSeatsBuilt; ++s)
    {
        held[static_cast<std::size_t>(s)] = HeldBy(players::generation::ControllerOf(s));
    }
    Logged now{.compass = Resolve(manager + kCompass) != nullptr};
    for (std::size_t s = 0; s < held.size(); ++s)
    {
        now.active |= held[s].active ? 1u << s : 0u;
        now.shooting[s] = held[s].shooting;
    }
    // The compass: whoever had an active point while it was open; while it closes nobody does (kept); a new one
    // starts as every player's.
    const bool compass = now.compass;
    g_compassOwners    = hud_rules::CompassOwners(compass, now.active, g_compassOwners);
    now.compassOwners  = g_compassOwners;
    std::array<Known, kMaxHidden> seen{};
    std::size_t                   seenCount = 0;
    try
    {
        platform::ForEachSparseElement(manager + kPins, kPinEntry, [&](std::uintptr_t entry) {
            ++now.entries;
            const auto pin   = ReadAt<std::uint64_t>(entry + kPinWidget);
            unsigned holders = 0;
            for (std::size_t s = 0; s < held.size(); ++s)
            {
                holders |= Holds(held[s], pin) ? 1u << s : 0u;
            }
            int owner = hud_rules::PinOwnerOf(holders);
            for (std::size_t i = 0; owner < 0 && i < g_knownCount; ++i)
            {
                owner = g_known[i].pin == pin ? g_known[i].owner : -1;  // released, still animating out
            }
            if (owner >= 0 && seenCount < seen.size())
            {
                seen[seenCount++] = {pin, owner};
            }
            if (owner < 0)
            {
                ++now.nobody;
            }
            else if (owner == mine)
            {
                ++now.mine;
            }
            else
            {
                ++now.theirs;
                Hide(entry + kPinWidget);
                Hide(entry + kPinHook);
            }
            return true;
        });
    }
    catch (...)
    {
        // What was hidden before the throw is listed and comes back in Restore.
    }
    g_known      = seen;  // pins gone from the list are forgotten
    g_knownCount = seenCount;
    if (compass &&(g_compassOwners & (1u << mine)) == 0)
    {
        Hide(manager + kCompass);
    }
    LogIfChanged(mine, now, manager);
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::hud::search_pins
