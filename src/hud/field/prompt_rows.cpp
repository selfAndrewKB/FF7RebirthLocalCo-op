#include "hud/field/prompt_rows.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/hud.h"
#include "facts/vehicles.h"
#include "hud/field/action_rows.h"
#include "hud/layout/viewport_placement.h"
#include "hud/passes/pass_opacity.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settled_log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/rides/chocobo_ride.h"
#include "player_two/weak_object.h"
#include "player_two/world/field_pickup.h"
#include "players/seats.h"
#include "players/widget_opacity.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lss::hud::prompt_rows {

namespace {

using RC::Unreal::UObject;

// RE-p2-field-prompts.md §1.2 and §4 (this build).
constexpr std::uint32_t  kLeaderActions     = 0x598;        // PC -> the leader-action component
constexpr std::uint32_t  kComponentRowBytes = 0x9A8;        // + 12*i: request byte
constexpr std::uint32_t  kComponentRowWeak  = 0x9AC;        // + 12*i: the row's weak widget
constexpr std::uint32_t  kRowStride         = 12;
constexpr std::size_t    kComponentRows     = 14;
constexpr std::uint32_t  kWidgetPriority    = 0x400;
constexpr std::uint32_t  kWidgetRemoveState = 0x418;
using action_rows::kMaxRows;

struct Row
{
    const UObject* widget     = nullptr;
    std::uint64_t  weak       = 0;  // the list's FWeakObjectPtr, as stored
    int            seatRow    = -1; // the owning seat's component row naming it now, or -1
    int            seatRide   = -1; // the handle of the owning seat's driven chocobo naming it now, or -1
    int            seatPickup = -1; // the field-prompt slot of the owning seat's pass naming it now, or -1
    int            owner      = 0;  // the seat whose row it is (0 player 1's)
    std::int32_t   priority   = 0;
    std::int32_t   remove     = 0;
    bool operator==(const Row&) const = default;
};

/// What names a seat's rows this frame.
struct SeatNames
{
    std::array<const UObject*, kComponentRows>                                        component{};
    std::array<const UObject*, std::size(facts::kChocoboMoveStateRowHandles)>         ride{};
    std::array<const UObject*, std::tuple_size_v<player_two::field_pickup::Weaks>> pickup{};
};

// Game thread.
std::vector<Row>                          g_rows;
struct RowsSteady
{
    std::array<Row, kMaxRows>                rows{};  // the first `count`, each remove state 0
    std::size_t                              count = 0;
    std::array<std::uint8_t, kComponentRows> bytes{};
    bool operator==(const RowsSteady&) const = default;
};
using RowRemoves = std::array<std::int32_t, kMaxRows>;
constexpr std::uint32_t kRowStillFrames = 30;
platform::SettledLog<RowsSteady, RowRemoves> g_rowLog(kRowStillFrames);
std::array<std::vector<const UObject*>, players::kMaxPlayers> g_seatWidgets;  // each seat's until they leave the list
std::vector<std::pair<const UObject*, int>> g_laidOut;  // the list as last laid out per player (widget, owner)
HiddenSet                                 g_hidden;  // the other player's rows, transparent for a pass
bool                                      g_layOutAgain = false;  // LayOutAgain: rows outside the layout placed at once
float                                     g_againWidth  = 0.0f;
float                                     g_againHeight = 0.0f;
std::array<std::vector<const UObject*>, players::kMaxPlayers> g_stillSeat;  // scratch, reused
std::vector<std::pair<const UObject*, int>> g_layoutNow;  // scratch, reused
std::array<SeatNames, players::kMaxPlayers> g_names;      // scratch, reused ([0] unused)
std::vector<action_rows::RowEntry>        g_listRows;   // scratch, reused: the game's list as last read

using platform::Read;  // the guarded reads: platform/guarded.h

UObject* Resolve(std::uint64_t weakValue) noexcept
{
    return static_cast<UObject*>(player_two::weak_object::ObjectOf(weakValue));
}

bool LaidOutBefore(const UObject* widget) noexcept
{
    return std::any_of(g_laidOut.begin(), g_laidOut.end(), [widget](const auto& entry) { return entry.first == widget; });
}

/// Each player's rows laid out on their own, when the list or an owner changed. A row new since the
/// last layout goes straight to its slot, as the game places a row it adds; the others slide.
void LayOutPerPlayer()
{
    g_layoutNow.clear();
    for (const Row& row : g_rows)
    {
        g_layoutNow.emplace_back(row.widget, row.owner);
    }
    if (g_layoutNow == g_laidOut || !action_rows::LayoutAvailable())
    {
        return;
    }
    const bool again = g_layOutAgain;
    g_layOutAgain    = false;
    for (int owner = 0; owner < players::kMaxPlayers; ++owner)
    {
        std::vector<std::uint64_t> weaks;
        std::vector<void*>         widgets;
        void*                      placeNow = nullptr;
        for (const Row& row : g_rows)
        {
            if (row.owner == owner)
            {
                weaks.push_back(row.weak);
                widgets.push_back(const_cast<UObject*>(row.widget));
                const bool fresh = row.remove == 0 && !LaidOutBefore(row.widget);  // new since the last layout
                placeNow         = placeNow == nullptr && fresh ? widgets.back() : placeNow;
            }
        }
        action_rows::ListHeader list{weaks.data(), static_cast<std::int32_t>(weaks.size()),
                                     static_cast<std::int32_t>(weaks.size())};
        // This player's rows by the game's layout (hud/field/action_rows.h): after a base change the rows stranded
        // outside the layout are named in a call each; otherwise one call places the new row at once, the others slide.
        if (!weaks.empty() && !action_rows::LayOut(&list, widgets, again, g_againWidth, g_againHeight, placeNow))
        {
            platform::Log(L"prompt rows: the game's layout raised an exception; rows keep the shared layout");
            return;
        }
    }
    g_laidOut = g_layoutNow;
}

std::wstring Describe(const std::vector<Row>& rows)
{
    std::wstring text;
    for (const Row& row : rows)
    {
        const wchar_t*     name  = players::SeatName(row.owner);
        const std::wstring owner = row.seatRow >= 0      ? std::format(L"{} row {}", name, row.seatRow)
                                   : row.seatRide >= 0   ? std::format(L"{}'s chocobo row {}", name, row.seatRide)
                                   : row.seatPickup >= 0 ? std::format(L"{}'s field prompt slot {}", name, row.seatPickup)
                                   : row.owner != 0      ? std::format(L"{} (leaving)", name)
                                                         : std::wstring(L"player 1");
        text += std::format(L"{}[{} prio {} remove {}]", text.empty() ? L"" : L" ", owner, row.priority, row.remove);
    }
    return text.empty() ? L"none" : text;
}

void LogIfChanged(const std::array<std::uint8_t, kComponentRows>& bytes)  // player 2's requests
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_rowLog.Reset();  // off: the next line starts fresh
        return;
    }
    // Which rows, whose, and player 2's requests log at once; a row's remove state steps through its fade
    // over a few frames, so it logs once it holds.
    RowsSteady steady;
    RowRemoves removes{};
    steady.bytes = bytes;
    for (const Row& row : g_rows)
    {
        if (steady.count == steady.rows.size())
        {
            break;  // the read stops at kMaxRows
        }
        removes[steady.count] = row.remove;
        Row& kept             = steady.rows[steady.count++];
        kept                  = row;
        kept.remove           = 0;
    }
    if (!g_rowLog.Offer(steady, removes))
    {
        return;
    }
    std::wstring requests;
    for (std::size_t i = 0; i < kComponentRows; ++i)
    {
        requests += bytes[i] != 0 ? std::format(L" {}:{:#04x}", i, bytes[i]) : L"";
    }
    platform::LogDebug(L"prompt rows: {}; player 2's requests:{}", Describe(g_rows), requests.empty() ? L" none" : requests);
}

/// The seat's component, ride and pickup rows this frame; `bytes` gets the component's request bytes.
void ReadSeatNames(int seat, SeatNames& names, std::array<std::uint8_t, kComponentRows>& bytes)
{
    names = {};
    bytes = {};
    const auto controller = reinterpret_cast<std::uintptr_t>(player_two::generation::ControllerOf(seat));
    if (controller == 0)
    {
        return;
    }
    // The seat's component: its row widgets and request bytes.
    std::uintptr_t component = 0;
    Read(controller + kLeaderActions, component);
    for (std::size_t i = 0; component != 0 && i < kComponentRows; ++i)
    {
        std::uint64_t weak = 0;
        Read(component + kComponentRowBytes + kRowStride * i, bytes[i]);
        names.component[i] = Read(component + kComponentRowWeak + kRowStride * i, weak) ? Resolve(weak) : nullptr;
    }
    // The seat's driven chocobo's own handles (Dash, Dismount, Jet, Scent, Scour): its tick makes their rows in the
    // one shared list, so they are the seat's the same way (RE-p2-chocobo-rows.md §4).
    const auto ride = reinterpret_cast<std::uintptr_t>(player_two::chocobo_ride::DrivenMoveState(seat));
    for (std::size_t i = 0; ride != 0 && i < names.ride.size(); ++i)
    {
        std::uint64_t weak = 0;
        names.ride[i] = Read(ride + facts::kChocoboMoveStateRowHandles[i], weak) ? Resolve(weak) : nullptr;
    }
    // The seat's field prompts (pickups): FieldActionMgr's slot rows in the seat's copy of its slot block, made by the
    // seat's pass of the game's own update (player_two/world/field_pickup.h).
    player_two::field_pickup::Weaks pickupWeaks{};
    player_two::field_pickup::RowWeaks(seat, pickupWeaks);
    for (std::size_t i = 0; i < pickupWeaks.size(); ++i)
    {
        names.pickup[i] = pickupWeaks[i] != 0 ? Resolve(pickupWeaks[i]) : nullptr;
    }
}

/// The index of `widget` among `named`, or -1.
template <class Names>
int IndexIn(const Names& named, const UObject* widget) noexcept
{
    for (std::size_t r = 0; widget != nullptr && r < named.size(); ++r)
    {
        if (named[r] == widget)
        {
            return static_cast<int>(r);
        }
    }
    return -1;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    action_rows::Install(image);  // the list and the game's layout, shared with the icons' base
    const bool native = players::widget_opacity::Installed();  // a row of the module table before this one
    platform::Log(L"prompt rows: per-player layout {}, per-view visibility {}",
                  action_rows::LayoutAvailable() ? L"ready" : L"UNAVAILABLE", native ? L"ready" : L"UNAVAILABLE");
    return action_rows::ListKnown();
}

void Refresh() noexcept
{
    if (!action_rows::ListKnown() || !player_two::generation::AnyActive())
    {
        return;
    }
    try
    {
        std::array<std::uint8_t, kComponentRows> bytes{}, playerTwoBytes{};
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            ReadSeatNames(seat, g_names[static_cast<std::size_t>(seat)], bytes);
            playerTwoBytes = seat == players::kPlayerTwoSeat ? bytes : playerTwoBytes;
        }

        g_rows.clear();
        if (!action_rows::Rows(g_listRows))
        {
            return;
        }
        for (auto& still : g_stillSeat)
        {
            still.clear();
        }
        for (const action_rows::RowEntry& entry : g_listRows)
        {
            Row row;
            row.weak   = entry.weak;
            row.widget = static_cast<const UObject*>(entry.widget);
            // The first seat naming the row owns it; a row a seat named before stays its own until it leaves the list.
            for (int seat = 1; seat <= players::kSeatsBuilt && row.owner == 0; ++seat)
            {
                const SeatNames& names = g_names[static_cast<std::size_t>(seat)];
                row.seatRow            = IndexIn(names.component, row.widget);
                row.seatRide           = IndexIn(names.ride, row.widget);
                row.seatPickup         = IndexIn(names.pickup, row.widget);
                const auto& kept       = g_seatWidgets[static_cast<std::size_t>(seat)];
                if (row.seatRow >= 0 || row.seatRide >= 0 || row.seatPickup >= 0 ||
                    std::find(kept.begin(), kept.end(), row.widget) != kept.end())
                {
                    row.owner = seat;
                }
            }
            if (row.owner != 0)
            {
                g_stillSeat[static_cast<std::size_t>(row.owner)].push_back(row.widget);
            }
            const auto address = reinterpret_cast<std::uintptr_t>(row.widget);
            Read(address + kWidgetPriority, row.priority);
            Read(address + kWidgetRemoveState, row.remove);
            g_rows.push_back(row);
        }
        g_seatWidgets.swap(g_stillSeat);
        LayOutPerPlayer();
        LogIfChanged(playerTwoBytes);
    }
    catch (...)
    {
        g_rows.clear();
    }
}
void Forget() noexcept
{
    Restore();
    g_rows.clear();
    for (auto& widgets : g_seatWidgets)
    {
        widgets.clear();  // a later row at a reused address must not count as a seat's
    }
    g_laidOut.clear();
    g_rowLog.Reset();
}

void Forget(int seat) noexcept
{
    if (players::SeatIndex(seat))
    {
        g_seatWidgets[static_cast<std::size_t>(seat)].clear();  // a reused address must not count as the seat's
    }
}

void LayOutAgain(float width, float height) noexcept
{
    g_laidOut.clear();
    g_layOutAgain = true;
    g_againWidth  = width;
    g_againHeight = height;
}

void ShowOnly(int seat) noexcept
{
    Restore();
    for (const Row& row : g_rows)
    {
        if (row.owner != seat)
        {
            g_hidden.Hide(Resolve(row.weak), row.weak);  // the other player's: transparent for this pass
        }
    }
}

void Restore() noexcept
{
    g_hidden.RestoreAll();
}

}  // namespace lss::hud::prompt_rows
