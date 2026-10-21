#include "hud/search_pins.h"

#include "facts/hud.h"
#include "hud/hud_roots.h"
#include "hud/hud_rules.h"
#include "hud/widget_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::hud::search_pins {

namespace {

// RE-p2-ranged-reticle.md §2.3 (this build). EndUISystem[55] points to the pin manager: +0x00 the compass (weak),
// +0x30 the pins, a TSparseArray of 40-byte entries: +0x00 the pin (weak), +0x08 its world location (FVector), +0x18
// the hook icon (weak).
constexpr std::uint32_t  kPinManager  = 0x1B8;
constexpr std::uint32_t  kCompass     = 0x00;
constexpr std::uint32_t  kPins        = 0x30;
constexpr std::uint32_t  kPinEntry    = 40;
constexpr std::uint32_t  kPinWidget   = 0x00;
constexpr std::uint32_t  kPinLocation = 0x08;
constexpr std::uint32_t  kPinHook     = 0x18;
// The leader-action component (PC+0x598) and its interact points (§2.2, §2.3; the same offsets in 1.005): +0x1490
// data, +0x1498 count, 624-byte points, the point's pin (weak) at +0x1C; +0x14A0 "a point is active" (the byte its
// update compares before opening or closing the compass); +0x4E0 the ranged selection's published state (logged).
constexpr std::uint32_t kLeaderActions = 0x598;
constexpr std::uint32_t kPoints        = 0x1490;
constexpr std::uint32_t kPointCount    = 0x1498;
constexpr std::uint32_t kPointSize     = 624;
constexpr std::uint32_t kPointPin      = 0x1C;
constexpr std::uint32_t kAnyActive     = 0x14A0;
constexpr std::uint32_t kShooting      = 0x4E0;
constexpr std::size_t   kMaxPoints     = 32;
constexpr std::size_t   kMaxHidden     = 64;

// The per-entry placement the list's tick runs (§2.3): stateless.
constexpr std::uint8_t kPlacePinBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55,
                                           0x57, 0x41, 0x56, 0x48, 0x8B, 0xEC, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlacePin{L"EndUI__M1B8_PlacePin_OwnerPC_EdgeFade", 0x143A0CFDC, kPlacePinBytes};
constexpr std::uint8_t kPlaceHookBytes[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x32, 0x01, 0x00, 0x00, 0xB8, 0x28, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlaceHook{L"EndUI__M1B8_PlaceHookIcon_ByAngle", 0x143A0CEA0, kPlaceHookBytes};

using PlacePinFn  = std::uint8_t (*)(void* manager, void* pin, const void* location, float* angle);
using PlaceHookFn = void (*)(void* unused, void* hook, const float* angle);

struct Hidden
{
    std::uint64_t weak     = 0;
    float         previous = 1.0f;  // the widget's own render opacity
};

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
    bool         activeOne = false, activeTwo = false;
    std::uint8_t shootingOne = 0, shootingTwo = 0;
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
std::array<Hidden, kMaxHidden> g_hidden{};
std::size_t                    g_hiddenCount   = 0;
unsigned                       g_compassOwners = 3;  // bit 0 player 1, bit 1 player 2 (both until known)
bool                           g_placeFaulted  = false;
std::array<Logged, 2>          g_logged{};
bool                           g_loggedOnce[2] = {false, false};

using platform::ReadAt;  // the guarded reads: platform/guarded.h

void* Resolve(std::uintptr_t weakField) noexcept
{
    return player_two::weak_object::ObjectAt(reinterpret_cast<const void*>(weakField));
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
    void* widget = Resolve(weakField);
    if (widget == nullptr || g_hiddenCount >= kMaxHidden)
    {
        return;
    }
    const float previous = widget_opacity::Get(widget);
    if (previous != 0.0f && widget_opacity::Set(widget, 0.0f))
    {
        g_hidden[g_hiddenCount++] = {ReadAt<std::uint64_t>(weakField), previous};
    }
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
        // The first few pins by name, so the reticle the user sees can be named.
        std::wstring names;
        int          named = 0;
        platform::ForEachSparseElement(manager + kPins, kPinEntry, [&names, &named](std::uintptr_t entry) {
            names += std::format(L"{}{} + {}", named != 0 ? L", " : L"", player_two::NameOf(Resolve(entry + kPinWidget)),
                                 player_two::NameOf(Resolve(entry + kPinHook)));
            return ++named < 3;
        });
        platform::LogDebug(L"search pins: player {}'s half: {} pin(s) ({} its player's, {} hidden as the other's, {} "
                           L"nobody's) [{}]; compass {} (owners {}); point active: player 1 {}, player 2 {}; ranged "
                           L"state +0x4E0: player 1 {:#x}, player 2 {:#x}",
                           mine + 1, now.entries, now.mine, now.theirs, now.nobody, names,
                           now.compass ? L"open" : L"none", now.compassOwners, player_two::YesNo(now.activeOne),
                           player_two::YesNo(now.activeTwo), now.shootingOne, now.shootingTwo);
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(facts::kEndUISystem, kPinManager + sizeof(void*));
    const auto pin    = platform::FindFunction(image, kPlacePin);
    const auto hook   = platform::FindFunction(image, kPlaceHook);
    g_system          = system ? *system : 0;
    g_placePin        = pin ? reinterpret_cast<PlacePinFn>(*pin) : nullptr;
    g_placeHook       = hook ? reinterpret_cast<PlaceHookFn>(*hook) : nullptr;
    platform::Log(L"search pins: each half's pins placed from its own camera: {}; each half shows only its own player's "
                  L"pins: {}",
                  g_system != 0 && g_placePin != nullptr && g_placeHook != nullptr ? L"ready" : L"UNAVAILABLE (a check failed)",
                  g_system == 0                  ? L"UNAVAILABLE (no pin list)"
                  : !widget_opacity::Installed() ? L"UNAVAILABLE (the opacity setter is not what the research read)"
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

void ShowOnly(int player) noexcept
{
    Restore();
    const auto manager = Manager();
    if (manager == 0 || !widget_opacity::Installed())
    {
        return;
    }
    const int  mine = player != 0 ? 1 : 0;
    const Held one  = HeldBy(player_two::generation::PlayerOneController());
    const Held two  = HeldBy(player_two::generation::PlayerTwoController());
    // The compass: whoever had an active point while it was open; while it closes nobody does (kept); a new one
    // starts as both players'.
    const bool compass = Resolve(manager + kCompass) != nullptr;
    g_compassOwners    = hud_rules::CompassOwners(compass, one.active, two.active, g_compassOwners);
    Logged now{.compass = compass, .compassOwners = g_compassOwners, .activeOne = one.active, .activeTwo = two.active,
               .shootingOne = one.shooting, .shootingTwo = two.shooting};
    std::array<Known, kMaxHidden> seen{};
    std::size_t                   seenCount = 0;
    try
    {
        platform::ForEachSparseElement(manager + kPins, kPinEntry, [&](std::uintptr_t entry) {
            ++now.entries;
            const auto pin   = ReadAt<std::uint64_t>(entry + kPinWidget);
            int        owner = hud_rules::PinOwnerOf(Holds(one, pin), Holds(two, pin));
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
    for (std::size_t i = 0; i < g_hiddenCount; ++i)
    {
        if (void* widget = player_two::weak_object::ObjectAt(&g_hidden[i].weak))
        {
            widget_opacity::Set(widget, g_hidden[i].previous);
        }
    }
    g_hiddenCount = 0;
}

}  // namespace lss::hud::search_pins
