#include "views/mirror_player.h"

#include "camera/view_target.h"
#include "facts/engine.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/local_players.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "session/own_controller.h"
#include "views/columns.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>

namespace lss::views::mirror_player {

namespace {

using players::unreal::UObject;

constexpr std::uint32_t kStatusTicks = 300;
constexpr std::uint32_t kRetryTicks  = 30;  // a refused add or removal tried again twice a second at 60

/// One mirror: its list record (the controller id is kFirstMirrorId + its slot) and what it does while in.
struct Mirror
{
    std::optional<players::LocalPlayers> list;
    void*                                controller = nullptr;
    std::uint32_t                        reaims     = 0;
};

// Game thread.
std::array<Mirror, columns::kMirrorsBuilt> g_mirrors;
int            g_wanted  = 0;
std::uint32_t  g_ticks   = 0;
std::uint32_t  g_retryAt = 0;  // no add or removal before this tick (after a refusal)
std::uint32_t  g_adds    = 0;
std::uint32_t  g_removes = 0;
std::uint32_t  g_dropped = 0;
bool           g_refused = false;    // a create raised an exception: no add again this session (as a seat's join)
const wchar_t* g_waiting = nullptr;  // why the add waits, logged once per reason
const wchar_t* g_fault   = nullptr;  // a refused add or removal, logged once until one succeeds

const wchar_t* YesNo(bool value) noexcept
{
    return value ? L"yes" : L"no";
}

std::uintptr_t Address(const void* object) noexcept
{
    return reinterpret_cast<std::uintptr_t>(object);
}

void Fault(const wchar_t* why) noexcept
{
    g_retryAt = g_ticks + kRetryTicks;
    if (g_fault != why)
    {
        g_fault = why;
        platform::Log(L"mirror view: {}; tried again twice a second", why);
    }
}

bool In(const Mirror& m) noexcept
{
    return m.list && m.list->HasPlayer();
}

/// The mirror's column: its place in the engine's list (-1 if not listed).
int ColumnOf(const Mirror& m) noexcept
{
    auto* listed = players::unreal::LocalPlayers(players::unreal::GameInstance());
    for (std::int32_t i = 0; listed != nullptr && In(m) && i < listed->Num(); ++i)
    {
        if ((*listed)[i] == m.list->Player())
        {
            return i;
        }
    }
    return -1;
}

/// Player 1's controller as the engine lists it (the list's first), or null.
void* PlayerOneController() noexcept
{
    auto* listed = players::unreal::LocalPlayers(players::unreal::GameInstance());
    if (listed == nullptr || listed->Num() < 1)
    {
        return nullptr;
    }
    UObject** field = players::unreal::ControllerField((*listed)[0]);
    return field == nullptr ? nullptr : *field;
}

/// Aims the mirror's camera manager at player 1's view target when it views another; logged at Debug.
void FollowPlayerOne(Mirror& m, void* playerOne) noexcept
{
    void* target = playerOne == nullptr ? nullptr : camera::view_target::Current(playerOne);
    void* viewed = m.controller == nullptr ? nullptr : camera::view_target::Current(m.controller);
    if (target != nullptr && viewed != target && camera::view_target::AimAt(m.controller, target))
    {
        ++m.reaims;
        platform::LogDebug(L"mirror view: id {} re-aimed at player 1's view target {:#x} (viewed {:#x})", m.list->Seat(),
                           Address(target), Address(viewed));
    }
}

void Remove(Mirror& m, const wchar_t* why) noexcept
{
    if (!In(m))
    {
        return;
    }
    const int  column = ColumnOf(m);
    const bool listed = m.list->StillListed();
    if (!m.list->Remove())
    {
        Fault(L"the engine did NOT remove a mirror view; kept");
        return;
    }
    ++g_removes;
    g_fault = nullptr;
    platform::Log(L"mirror view: {}: the mirror in column {} (controller id {}) {} (re-aimed {} time(s) while in)", why,
                  column + 1, m.list->Seat(), listed ? L"left" : L"was already gone", m.reaims);
    m.controller = nullptr;
    m.reaims     = 0;
}

/// The mirrors the engine dropped by itself, forgotten; each one's controller read again (a rebuilt one is followed).
void FollowList() noexcept
{
    for (Mirror& m : g_mirrors)
    {
        if (!In(m))
        {
            continue;
        }
        if (!m.list->StillListed())
        {
            ++g_dropped;
            platform::Log(L"mirror view: the engine dropped the mirror with controller id {} by itself (added again on "
                          L"a later tick while its column is held)",
                          m.list->Seat());
            m.controller = nullptr;
            m.reaims     = 0;
            continue;
        }
        UObject** field      = players::unreal::ControllerField(m.list->Player());
        void*     controller = field == nullptr ? nullptr : *field;
        if (controller != m.controller)
        {
            platform::Log(L"mirror view: the mirror with controller id {} has a new controller {:#x}; followed",
                          m.list->Seat(), Address(controller));
            m.controller = controller;
        }
    }
}

/// Why no mirror can be created for `one` (player 1 as the list gives it) now, or null. One literal per reason: the
/// waiting log compares them by address.
const wchar_t* WhyNotAddable(const players::LocalPlayers::PlayerOne& one) noexcept
{
    if (one.controller == nullptr)
    {
        return L"the engine's list has no room, player 1 has no controller, or the id is a seat's";
    }
    if (!session::own_controller::Ready())
    {
        return L"player 1's camera names are not set yet (its camera manager has not run)";
    }
    if (!session::own_controller::UsesNativeClasses(one.controller))
    {
        return L"player 1's controller is not the End class";
    }
    return nullptr;
}

/// One mirror into the next column: the lowest free id, through the join scope, aimed at player 1's view target.
void Add() noexcept
{
    const auto free = std::find_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& m) { return m.list && !In(m); });
    if (free == g_mirrors.end())
    {
        return;
    }
    Mirror&        m   = *free;
    const auto     one = m.list->FirstPlayer(false);
    const wchar_t* why = WhyNotAddable(one);
    if (why != nullptr)
    {
        if (g_waiting != why)
        {
            g_waiting = why;
            platform::LogDebug(L"mirror view: waiting to fill a column: {}", why);
        }
        return;
    }
    g_waiting = nullptr;
    players::LocalPlayers::Added      added;
    session::own_controller::Report report;
    {
        const session::own_controller::JoinScope scope(one.controller, session::own_controller::kNoSeat);
        added  = m.list->Add(one.controller);
        report = scope.Result();
    }
    if (!In(m))
    {
        Fault(L"the engine did not add a local player for the mirror view");
        return;
    }
    ++g_adds;
    m.controller = added.controller;
    m.reaims     = 0;
    if (added.raised)
    {
        // The guarded fault leaves the engine half-changed: never carry on with it (the session's rule for a seat).
        g_refused = true;
        platform::Log(L"mirror view: the engine raised an exception while creating the mirror (camera manager handled: "
                      L"{}, owner filled: {}, names swapped: {}); removing it, and no mirror view is added again this "
                      L"session",
                      YesNo(report.cameraManager != nullptr), YesNo(report.ownerFilled), YesNo(report.namesSwapped));
        Remove(m, L"the create raised");
        return;
    }
    g_fault = nullptr;
    void*      target = camera::view_target::Current(one.controller);
    const bool aimed  = m.controller != nullptr && target != nullptr &&
                       camera::view_target::AimAt(m.controller, target);
    platform::Log(L"mirror view: in column {} (controller id {}, controller {:#x}; camera manager handled {}, owner "
                  L"filled {}, names swapped {}); its camera manager views {:#x} ({}player 1's view target {:#x})",
                  ColumnOf(m) + 1, m.list->Seat(), Address(m.controller), YesNo(report.cameraManager != nullptr),
                  YesNo(report.ownerFilled), YesNo(report.namesSwapped),
                  Address(m.controller == nullptr ? nullptr : camera::view_target::Current(m.controller)),
                  aimed ? L"" : L"NOT ", Address(target));
}

/// The mirror in the highest column, or null.
Mirror* Last() noexcept
{
    Mirror* last   = nullptr;
    int     column = -1;
    for (Mirror& m : g_mirrors)
    {
        const int at = ColumnOf(m);
        if (In(m) && at >= column)
        {
            last   = &m;
            column = at;
        }
    }
    return last;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto removeLocalPlayer = image.Rebase(facts::kRemoveLocalPlayer.address);
    if (!removeLocalPlayer)
    {
        platform::Log(L"mirror view: RemoveLocalPlayer is not where the research read it; no mirror views (an empty "
                      L"column stays black)");
        return;
    }
    for (std::size_t slot = 0; slot < g_mirrors.size(); ++slot)
    {
        g_mirrors[slot].list.emplace(*removeLocalPlayer, players::kFirstMirrorId + static_cast<std::int32_t>(slot));
    }
    platform::Log(L"mirror view: ready (player 1's picture in every column no player has while more columns are held "
                  L"than players are in; controller ids {} to {})",
                  players::kFirstMirrorId, players::kFirstMirrorId + columns::kMirrorsBuilt - 1);
}

void Tick(int wanted, const wchar_t* (*hold)() noexcept) noexcept
{
    if (!g_mirrors[0].list)
    {
        return;
    }
    ++g_ticks;
    FollowList();
    g_wanted = std::clamp(wanted, 0, columns::kMirrorsBuilt);
    while (Count() > g_wanted && g_ticks >= g_retryAt)
    {
        const int before = Count();
        Remove(*Last(), L"its column is a player's, or no longer held");
        if (Count() == before)
        {
            break;  // the engine kept it; tried again after the retry wait
        }
    }
    if (Count() < g_wanted && g_ticks >= g_retryAt && !g_refused)
    {
        const wchar_t* why = hold == nullptr ? nullptr : hold();
        if (why == nullptr)
        {
            Add();
        }
        else if (g_waiting != why)
        {
            g_waiting = why;
            platform::LogDebug(L"mirror view: waiting to fill a column: {}", why);
        }
    }
    else if (Count() >= g_wanted || g_refused)
    {
        g_waiting = nullptr;  // nothing waits: the columns are filled, or no add is made again this session
    }
    void* playerOne = Count() == 0 ? nullptr : PlayerOneController();
    for (Mirror& m : g_mirrors)
    {
        if (In(m))
        {
            FollowPlayerOne(m, playerOne);
        }
    }
    if (Count() != 0 && g_ticks % kStatusTicks == 0 && platform::Enabled(platform::Level::Debug))
    {
        platform::LogDebug(L"mirror view: {}", Reading());
    }
}

bool Clear(const wchar_t* why) noexcept
{
    FollowList();
    for (Mirror& m : g_mirrors)
    {
        Remove(m, why);
    }
    return Count() == 0;
}

int Count() noexcept
{
    return static_cast<int>(std::count_if(g_mirrors.begin(), g_mirrors.end(), [](const Mirror& m) { return In(m); }));
}

std::wstring Reading()
{
    std::wstring in;
    for (const Mirror& m : g_mirrors)
    {
        if (In(m))
        {
            in += std::format(L"{}column {} (controller id {})", in.empty() ? L"" : L", ", ColumnOf(m) + 1, m.list->Seat());
        }
    }
    return std::format(L"wanted {}; {}; adds {}, removes {}, dropped by the engine {}{}{}{}", g_wanted,
                       in.empty() ? std::wstring(L"none in") : L"in " + in, g_adds, g_removes, g_dropped,
                       g_refused ? L"; no more adds (a create raised an exception)" : L"",
                       g_waiting != nullptr ? std::format(L"; waiting: {}", g_waiting) : std::wstring(),
                       g_fault != nullptr ? std::format(L"; refused: {}", g_fault) : std::wstring());
}

}  // namespace lss::views::mirror_player
