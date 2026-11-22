#include "hud/field/jet_gauges.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "hud/passes/pass_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/first_player_scope.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"
#include "rides/chocobo_ride.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::hud::jet_gauges {

namespace {

// UEndChocoboJetGaugeComponent (research/RE-chocobo-exits-pads.md; the reflection dump): its BeginPlay makes the
// widget; its placement projects the owner's socket through the world's first local player and moves the widget,
// then runs the show and hide animations and the fill, every tick. 1.005 through replaced-chocobo-spots.json.
constexpr std::uint8_t kBeginPlayBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xCC, 0x46, 0x64, 0xFE,
                                            0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::SlotFact kBeginPlaySlot{L"the chocobo jet gauge component's BeginPlay slot", 0x146679650,
                                            {L"ChocoboJetGaugeComp_BeginPlay_CreatesWidget", 0x143B274A8, kBeginPlayBytes}};
constexpr std::uint8_t kPlaceBytes[] = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xBC, 0xE2, 0x62, 0xFE,
                                        0x48, 0x2B, 0xE0, 0x48, 0x83, 0xB9, 0xC0, 0x00, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kPlace{L"ChocoboJetGaugeComp_PlaceWidget_FirstLocalPlayerView", 0x143B3D8B8, kPlaceBytes};
using BeginPlayFn = std::uintptr_t (*)(void* component);
using PlaceFn     = void (*)(void* component);
constexpr std::uint32_t kGaugeWidget = 0xC0;  // JetGaugeWidget: the widget on the root overlay (null before BeginPlay)

/// A gauge component noted at its BeginPlay, by its chocobo (both as the game's weak pointers).
struct Gauge
{
    std::uint64_t component = 0;
    std::uint64_t owner     = 0;
};
constexpr std::size_t kMaxGauges = 8;

struct Logged
{
    bool        placed = false;
    bool        first  = false;  // the seat's controller first in the world's list
    std::size_t hidden = 0;
    bool operator==(const Logged&) const = default;
};

PlaceFn               g_place = nullptr;  // set once at install
platform::VTableEntry g_beginPlay;
// Game thread.
std::array<Gauge, kMaxGauges>            g_gauges{};
std::size_t                              g_nextGauge = 0;  // the entry that gives way when every one is live
HiddenSet                                g_hidden;
bool                                     g_placeFaulted = false;
std::array<Logged, players::kMaxPlayers> g_logged{};
std::array<bool, players::kMaxPlayers>   g_loggedOnce{};

void* Live(std::uint64_t weak) noexcept
{
    return players::weak_object::ObjectOf(weak);
}

/// The gauge component of `chocobo`, or null.
void* ComponentOf(const void* chocobo) noexcept
{
    for (const Gauge& gauge : g_gauges)
    {
        if (gauge.owner != 0 && chocobo != nullptr && Live(gauge.owner) == chocobo)
        {
            return Live(gauge.component);
        }
    }
    return nullptr;
}

/// Whose gauge a chocobo's is: a seat's number, 0 player 1's, -1 nobody's (an AI chocobo's, kept in every view; a
/// chocobo gone).
int OwnerOf(const void* chocobo) noexcept
{
    if (chocobo == nullptr)
    {
        return -1;
    }
    const int seat = rides::chocobo_ride::SeatDriving(chocobo);
    if (seat >= 1)
    {
        return seat;
    }
    return chocobo == players::actor_fields::Pawn(players::generation::PlayerOneController()) ? 0 : -1;
}

void LogIfChanged(int seat, const Logged& now) noexcept
{
    const auto index = static_cast<std::size_t>(seat);
    if (!platform::Enabled(platform::Level::Debug) || (g_loggedOnce[index] && now == g_logged[index]))
    {
        return;
    }
    g_logged[index]     = now;
    g_loggedOnce[index] = true;
    try
    {
        if (seat == 0)
        {
            platform::LogDebug(L"jet gauges: player 1's view: {} other player's gauge(s) hidden", now.hidden);
            return;
        }
        platform::LogDebug(L"jet gauges: {}'s view: their chocobo's gauge {}{}; {} other player's gauge(s) hidden",
                           players::SeatName(seat),
                           now.placed ? L"placed from their own view" : L"not placed (no ride, no gauge, or the placement off)",
                           now.placed && !now.first ? L" with player 1 still first in the world's list" : L"", now.hidden);
    }
    catch (...)
    {
    }
}

/// The entry for a gauge of `owner`: the one that names the owner already, else a dead or empty one, else the next
/// in turn (more live gauges than kept: wild chocobos').
Gauge& EntryFor(const void* owner) noexcept
{
    for (Gauge& gauge : g_gauges)
    {
        if (gauge.owner != 0 && Live(gauge.owner) == owner)
        {
            return gauge;
        }
    }
    for (Gauge& gauge : g_gauges)
    {
        if (gauge.component == 0 || Live(gauge.component) == nullptr)
        {
            return gauge;
        }
    }
    Gauge& gauge = g_gauges[g_nextGauge];
    g_nextGauge  = (g_nextGauge + 1) % kMaxGauges;
    return gauge;
}

/// The component's BeginPlay: the gauge noted by its chocobo.
std::uintptr_t BeginPlay(void* component) noexcept
{
    const auto result = reinterpret_cast<BeginPlayFn>(g_beginPlay.Original())(component);
    if (component == nullptr || !players::generation::OnGameThread())
    {
        return result;
    }
    void* owner = players::actor_fields::ComponentOwner(component);
    Gauge noted{players::weak_object::WeakOf(component), players::weak_object::WeakOf(owner)};
    if (noted.component == 0 || noted.owner == 0)
    {
        return result;
    }
    EntryFor(owner) = noted;
    try
    {
        platform::LogDebug(L"jet gauges: a gauge noted for {}", players::NameOf(owner));
    }
    catch (...)
    {
    }
    return result;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto place = platform::FindFunction(image, kPlace);
    g_place          = place ? reinterpret_cast<PlaceFn>(*place) : nullptr;
    const bool noted = g_place != nullptr &&
                       platform::ReplaceSlot(image, kBeginPlaySlot, reinterpret_cast<std::uintptr_t>(&BeginPlay), g_beginPlay);
    g_hidden.Reserve(kMaxGauges);
    platform::Log(L"jet gauges: player 2's or 3's chocobo's gauge placed from their own view: {}; each view shows only its "
                  L"own player's gauge: {}",
                  noted ? L"ready" : L"UNAVAILABLE (a check failed; the gauge keeps player 1's placement)",
                  noted && players::widget_opacity::Installed() ? L"ready" : L"UNAVAILABLE");
}

void Remove() noexcept
{
    if (g_beginPlay.Original() != 0 && !g_beginPlay.Restore())
    {
        platform::Log(L"jet gauges: the BeginPlay slot was changed after us and is left in place (it passes through)");
    }
    g_hidden.Forget();
    g_gauges.fill({});
    g_place = nullptr;
}

void Place(int seat) noexcept
{
    if (!players::SeatBuilt(seat) || g_place == nullptr || g_placeFaulted || !players::generation::OnGameThread())
    {
        return;
    }
    Logged& now     = g_logged[static_cast<std::size_t>(seat)];
    Logged  next    = now;
    next.placed     = false;
    next.first      = false;
    void* component = ComponentOf(rides::chocobo_ride::DrivenChocobo(seat));
    if (component != nullptr && ReadPointer(component, kGaugeWidget) != nullptr)
    {
        const players::FirstPlayerScope first(seat);
        next.first = first.Held();
        if (!platform::CallGuarded(g_place, component))
        {
            g_placeFaulted = true;
            platform::Log(L"jet gauges: FAILED: the game's placement faulted; every gauge keeps player 1's placement");
            return;
        }
        next.placed = true;
    }
    LogIfChanged(seat, next);
}

void ShowOnly(int seat) noexcept
{
    Restore();
    if (seat < 0 || seat >= players::kMaxPlayers || !players::widget_opacity::Installed() ||
        !players::generation::OnGameThread())
    {
        return;
    }
    std::size_t hidden = 0;
    for (const Gauge& gauge : g_gauges)
    {
        void* component = gauge.component != 0 ? Live(gauge.component) : nullptr;
        const int owner = component != nullptr ? OwnerOf(Live(gauge.owner)) : -1;
        if (owner < 0 || owner == seat)
        {
            continue;  // this view's own, or no player's
        }
        if (g_hidden.Hide(ReadPointer(component, kGaugeWidget)))
        {
            ++hidden;
        }
    }
    Logged& now = g_logged[static_cast<std::size_t>(seat)];
    Logged  next = now;
    next.hidden  = hidden;
    LogIfChanged(seat, next);
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::hud::jet_gauges
