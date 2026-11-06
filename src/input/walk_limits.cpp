#include "input/walk_limits.h"

#include "camera/location_volumes.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/unreal_access.h"
#include "scopes/primary_controller.h"
#include "world/world_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::input::walk_limits {

namespace {

// RE-p2-walk-mode.md §4.2: PartyManager_SetPrimaryPC_CreateNavAreaComp(partyManager, controller) sets the primary
// controller, ensures its trigger, NewObjects and registers its UEndPlayerExistNavAreaComponent (pc+0x590) and sets
// pc+0x7DC. It has no guard of its own (a second call makes a second component), so it runs only while +0x590 is null.
constexpr std::uint8_t   kCreateNavAreaBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8B,
                                                  0xEC, 0xB8, 0x70, 0x00, 0x00, 0x00, 0xE8, 0x1D};
constexpr platform::FunctionFact kCreateNavArea{L"PartyManager_SetPrimaryPC_CreateNavAreaComp", 0x143D00E00,
                                                kCreateNavAreaBytes};
constexpr std::uint32_t  kControllerNavArea  = 0x590;
using camera::location_volumes::kNavAreaController;

// §4.3.3: LocMoveTypeListener_Vt40_SetPrimaryPCLayer7(listener, LocationId, move, dash, enter) writes the primary
// controller's layer 7 (+0x897 move, +0x8AB dash): {move, dash} on enter, {6, 0} on leave; nothing for LocationId 0.
// `this` is unused; the game's instance is the singleton at 0x14939BCE8.
constexpr std::uint8_t   kSetLayer7Bytes[] = {0x48, 0x85, 0xD2, 0x74, 0x54, 0x48, 0x89, 0x5C,
                                              0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSetLayer7{L"LocMoveTypeListener_Vt40_SetPrimaryPCLayer7", 0x14182BE00,
                                            kSetLayer7Bytes};
constexpr std::uintptr_t kMoveTypeListener = 0x14939BCE8;
constexpr std::uint32_t  kControllerLayer7Move = 0x890 + 7;
constexpr std::uint32_t  kControllerLayer7Dash = 0x8A4 + 7;

// §4.3.1: the location manager's volumes, lock, pause byte and move-type stack, each volume's fields and the game's
// containment test: camera/location_volumes.h.
using camera::location_volumes::kFlagChangeMovementType;
using camera::location_volumes::kFlagDisabled;
using camera::location_volumes::kFlagForce;
using camera::location_volumes::kVolumeDashType;
using camera::location_volumes::kVolumeForcedOut;
using camera::location_volumes::kVolumeMoveType;
using camera::location_volumes::kVolumeUntouchMargin;

constexpr int       kMaxInside  = 32;   // move-type volumes a seat's member stands in at once (a handful in practice)
constexpr ULONGLONG kRetryMs    = 250;  // a write the game's gate declined is tried again this often
constexpr ULONGLONG kStateMs    = 1000;

using CreateNavAreaFn = void (*)(void* partyManager, void* controller);
using SetLayer7Fn     = void (*)(void* listener, std::uint64_t location, std::uint8_t move, std::uint8_t dash,
                             std::uint8_t enter);

CreateNavAreaFn g_createNavArea = nullptr;
SetLayer7Fn     g_setLayer7     = nullptr;
std::uintptr_t  g_partyManager  = 0;  // addresses of the globals holding the pointers
std::uintptr_t  g_listener      = 0;

/// {SpecificMoveType, DashType}; {6 Empty, 0 Enable} is no limit.
struct Pair
{
    std::uint8_t move = 6, dash = 0;
    bool         operator==(const Pair&) const = default;
};
constexpr Pair kNoLimit{};

struct Inside
{
    const void*   volume   = nullptr;
    std::uint64_t location = 0;
    Pair          pair;
};
/// A seat's copy of the manager's per-volume "inside" bits and its move-type stack (index 0 = top), and the seat's
/// retry and log state.
struct State
{
    void*         controller = nullptr;
    void*         member     = nullptr;
    Inside        inside[kMaxInside];
    int           insideCount = 0;
    world::world_rules::PairStack<Pair, kMaxInside> stack;  // every entry is held by an inside volume, so never more
    std::uint64_t gateLocation = 0;  // a LocationId for the listener's "not 0" gate
    ULONGLONG     nextRetry    = 0;
};
/// One per seat (index 0 unused): the state, rebuilt on a take or leave, and what outlives it.
struct SeatWalk
{
    State        state;
    void*        navTried   = nullptr;  // the controller the nav-area creation ran (or was found) for
    ULONGLONG    nextState  = 0;
    bool         overflowed = false;
    std::wstring lastState, lastCall;
};
std::array<SeatWalk, players::kMaxPlayers> g_seats{};

/// A line of `level`.
template <class... A>
void LineAt(platform::Level level, std::wformat_string<A...> format, A&&... args) noexcept
{
    if (!platform::Enabled(level))
    {
        return;
    }
    try
    {
        platform::LogAt(level, L"walk limits: {}", std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
}

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    LineAt(platform::Level::Info, format, std::forward<A>(args)...);
}

std::wstring PairText(Pair pair)
{
    return pair == kNoLimit ? std::wstring(L"none") : std::format(L"{}/{}", pair.move, pair.dash);
}

/// Once per controller: the game creates the seat's nav-area component, run as the primary player's.
void EnsureNavArea(int seat, SeatWalk& walk, void* controller) noexcept
{
    if (g_createNavArea == nullptr || walk.navTried == controller)
    {
        return;
    }
    const wchar_t* name = players::SeatName(seat);
    if (ReadPointer(controller, kControllerNavArea) != nullptr)
    {
        walk.navTried = controller;
        Line(L"{}'s controller already has its nav-area component (kept)", name);
        return;
    }
    void* partyManager = ReadPointerAt(g_partyManager);
    if (partyManager == nullptr)
    {
        return;  // tried again next tick
    }
    bool ran = false;
    {
        const scopes::primary_controller::Scope scope(seat);
        if (!scope.Held())
        {
            return;  // tried again next tick; never unscoped (it would make the seat's player the primary player for good)
        }
        walk.navTried = controller;
        ran           = platform::CallGuarded(g_createNavArea, partyManager, controller);
    }
    void* component = ReadPointer(controller, kControllerNavArea);
    Line(L"{}'s nav-area component by the game's own creation: {} (component {}, its controller is {}'s: {})", name,
         ran ? L"ran" : L"FAULTED", component, name,
         component != nullptr && ReadPointer(component, kNavAreaController) == controller);
}

/// The leaves and enters a seat's member makes this tick, by the game's rules. False if the manager is busy,
/// paused or unreadable (nothing changes then).
struct Changes
{
    bool   leave[kMaxInside] = {};
    Inside enter[kMaxInside];
    int    enterCount = 0;
    Pair   managerTop = kNoLimit;  // the game's own (player 1's) top, for the log
};

/// The move-type enter/leave over the walk's volumes (the game's rules: bForce and bDisabled honoured).
struct Pass
{
    const float* position;
    Changes*     changes;
    State*       state;  // the seat's
    bool         seen[kMaxInside] = {};
};

void Visit(void* context, const camera::location_volumes::Volume& volume) noexcept
{
    auto*  pass  = static_cast<Pass*>(context);
    State& state = *pass->state;
    if ((volume.flags & kFlagChangeMovementType) == 0 || volume.location == 0)
    {
        return;
    }
    const std::uint64_t location = volume.location;
    state.gateLocation           = state.gateLocation != 0 ? state.gateLocation : location;
    int current = -1;  // matched by LocationId as well as address: a streamed-in volume may reuse an address
    for (int k = 0; k < state.insideCount; ++k)
    {
        if (state.inside[k].volume == volume.volume && state.inside[k].location == location)
        {
            current       = k;
            pass->seen[k] = true;
            break;
        }
    }
    if ((volume.flags & kFlagDisabled) != 0)
    {
        return;  // the game's rule: no enter, no leave, the state kept
    }
    bool in = false;
    if ((volume.flags & kFlagForce) != 0)
    {
        std::uint8_t forcedOut = 0;
        if (!Read(volume.volume, kVolumeForcedOut, forcedOut))
        {
            return;
        }
        in = world::world_rules::ForcedInside(current >= 0, forcedOut);
    }
    else
    {
        float margin = 0.0f;
        const float (&position)[3] = *reinterpret_cast<const float (*)[3]>(pass->position);
        if ((current >= 0 && !Read(volume.volume, kVolumeUntouchMargin, margin)) ||
            !camera::location_volumes::Contains(volume.volume, position, current >= 0 ? margin : 0.0f, in))
        {
            return;  // unreadable: this volume's state is kept this tick
        }
    }
    Changes& changes = *pass->changes;
    if (current >= 0 && !in)
    {
        changes.leave[current] = true;
        state.gateLocation     = location;
    }
    else if (current < 0 && in)
    {
        Inside here{volume.volume, location};
        if (!Read(volume.volume, kVolumeMoveType, here.pair.move) ||
            !Read(volume.volume, kVolumeDashType, here.pair.dash))
        {
            return;
        }
        if (changes.enterCount < kMaxInside)
        {
            changes.enter[changes.enterCount++] = here;
            state.gateLocation                  = location;
        }
    }
}

bool Walk(State& state, const float (&position)[3], Changes& changes) noexcept
{
    Pass                    pass{&position[0], &changes, &state};
    camera::location_volumes::Reads reads;
    if (camera::location_volumes::WalkVolumes(true, &Visit, &pass, reads) != camera::location_volumes::WalkResult::Done)
    {
        return false;  // busy, paused or unreadable: nothing changes this tick
    }
    if (reads.hasTop)
    {
        changes.managerTop = {reads.topMove, reads.topDash};
    }
    for (int k = 0; k < state.insideCount; ++k)
    {
        changes.leave[k] = changes.leave[k] || !pass.seen[k];  // gone from the list (streamed out): a leave
    }
    return true;
}

/// The game's order: every leave of the tick, then every enter (the game's pop: a pair loses a reference and goes at
/// 0; its push: a pair already on the stack gains a reference, a new one goes to the front).
void Apply(int seat, SeatWalk& walk, const Changes& changes) noexcept
{
    State& state = walk.state;
    int    kept  = 0;
    for (int k = 0; k < state.insideCount; ++k)
    {
        if (changes.leave[k])
        {
            state.stack.Pop(state.inside[k].pair);  // the pair stored at the enter, as for a volume gone from the list
        }
        else
        {
            state.inside[kept++] = state.inside[k];
        }
    }
    state.insideCount = kept;
    for (int e = 0; e < changes.enterCount; ++e)
    {
        if (state.insideCount >= kMaxInside)
        {
            if (!walk.overflowed)
            {
                walk.overflowed = true;
                Line(L"{} stands in more than {} move-type volumes; the rest are not counted", players::SeatName(seat),
                     kMaxInside);
            }
            break;
        }
        state.inside[state.insideCount++] = changes.enter[e];
        state.stack.Push(changes.enter[e].pair);
    }
}

Pair ControllerLayer7(const void* controller) noexcept
{
    Pair pair{0xFF, 0xFF};
    Read(controller, kControllerLayer7Move, pair.move);
    Read(controller, kControllerLayer7Dash, pair.dash);
    return pair;
}

/// Writes the seat's layer 7 with the game's own listener method, as the primary player's.
void Write(int seat, SeatWalk& walk, void* controller, Pair want, Pair before) noexcept
{
    State&          state    = walk.state;
    const ULONGLONG now      = GetTickCount64();
    void*           listener = ReadPointerAt(g_listener);
    if (now < state.nextRetry || listener == nullptr || state.gateLocation == 0)
    {
        return;
    }
    bool ran = false, held = false;
    {
        const scopes::primary_controller::Scope scope(seat);
        held = scope.Held();
        if (held)
        {
            const bool limit = !(want == kNoLimit);
            ran = platform::CallGuarded(g_setLayer7, listener, state.gateLocation, want.move, want.dash,
                                        static_cast<std::uint8_t>(limit ? 1 : 0));
        }
    }
    const Pair after = ControllerLayer7(controller);
    state.nextRetry  = after == want ? 0 : now + kRetryMs;
    try
    {
        std::wstring call = std::format(L"{}'s layer 7 {} -> {}: {}", players::SeatName(seat), PairText(before),
                                        PairText(want),
                                        !held  ? L"not written (the primary scope was refused; tried again)"
                                        : !ran ? L"FAULTED"
                                        : after == want ? L"written by the game's listener method, as the primary player"
                                                        : L"the game's gate declined (the field leader is not ready; tried again)");
        if (call != walk.lastCall)
        {
            walk.lastCall = call;
            Line(L"{}", call);
        }
    }
    catch (...)
    {
    }
}

/// One seat's tick.
void TickSeat(int seat) noexcept
{
    SeatWalk&   walk       = g_seats[static_cast<std::size_t>(seat)];
    void* const controller = players::generation::Active(seat) ? players::generation::ControllerOf(seat) : nullptr;
    void* const member     = controller != nullptr ? players::generation::MemberOf(seat) : nullptr;
    if (controller != walk.state.controller || member != walk.state.member)
    {
        // Take, leave, rejoin or another member: rebuilt from the position next.
        walk.state            = {};
        walk.state.controller = controller;
        walk.state.member     = member;
    }
    if (member == nullptr)
    {
        return;
    }
    EnsureNavArea(seat, walk, controller);
    float position[3]{};
    Changes changes;
    if (g_setLayer7 == nullptr || !players::unreal::ActorLocation(member, position) || !Walk(walk.state, position, changes))
    {
        return;
    }
    Apply(seat, walk, changes);
    const Pair want   = walk.state.stack.Count() > 0 ? walk.state.stack.At(0).pair : kNoLimit;
    const Pair before = ControllerLayer7(controller);
    if (!(before == want))
    {
        Write(seat, walk, controller, want, before);
    }

    const ULONGLONG now = GetTickCount64();
    if (now >= walk.nextState && platform::Enabled(platform::Level::Debug))
    {
        walk.nextState = now + kStateMs;
        try
        {
            std::wstring state = std::format(L"player 1's layer 7 {} (the game's top {}); {}'s {} (own top {}, {} "
                                             L"volumes, {} stack entries)",
                                             PairText(ControllerLayer7(players::generation::ControllerOf(0))),
                                             PairText(changes.managerTop), players::SeatName(seat),
                                             PairText(ControllerLayer7(controller)), PairText(want),
                                             walk.state.insideCount, walk.state.stack.Count());
            if (state != walk.lastState)
            {
                walk.lastState = state;
                LineAt(platform::Level::Debug, L"{}", state);
            }
        }
        catch (...)
        {
        }
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto create   = platform::FindFunction(image, kCreateNavArea);
    const auto party    = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto set      = platform::FindFunction(image, kSetLayer7);
    const auto listener = image.Rebase(kMoveTypeListener, sizeof(void*));
    const bool volumes  = camera::location_volumes::Install(image);
    if (create && party && scopes::primary_controller::Installed())
    {
        g_createNavArea = reinterpret_cast<CreateNavAreaFn>(*create);
        g_partyManager  = *party;
    }
    if (set && listener && volumes && scopes::primary_controller::Installed())
    {
        g_setLayer7 = reinterpret_cast<SetLayer7Fn>(*set);
        g_listener  = *listener;
    }
    platform::Log(L"walk limits: a further player's nav-area component by the game's creation {}; that player's location walk "
                  L"limit from their own position {}",
                  g_createNavArea != nullptr ? L"yes" : L"NO (a check failed)",
                  g_setLayer7 != nullptr ? L"yes" : L"NO (a check failed)");
    return g_createNavArea != nullptr || g_setLayer7 != nullptr;
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        TickSeat(seat);
    }
}

}  // namespace lss::input::walk_limits
