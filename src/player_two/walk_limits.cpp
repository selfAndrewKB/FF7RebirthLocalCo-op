#include "player_two/walk_limits.h"

#include "player_two/world_rules.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::walk_limits {

namespace {

// RE-p2-walk-mode.md §4.2: PartyManager_SetPrimaryPC_CreateNavAreaComp(partyManager, controller) sets the primary
// controller, ensures its trigger, NewObjects and registers its UEndPlayerExistNavAreaComponent (pc+0x590) and sets
// pc+0x7DC. It has no guard of its own (a second call makes a second component), so it runs only while +0x590 is null.
constexpr std::uintptr_t kCreateNavArea = 0x143D00E00;
constexpr std::uint8_t   kCreateNavAreaBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8B,
                                                  0xEC, 0xB8, 0x70, 0x00, 0x00, 0x00, 0xE8, 0x1D};
constexpr std::uint32_t  kControllerNavArea  = 0x590;
constexpr std::uint32_t  kNavAreaController  = 0xB0;

// §4.3.3: LocMoveTypeListener_Vt40_SetPrimaryPCLayer7(listener, LocationId, move, dash, enter) writes the primary
// controller's layer 7 (+0x897 move, +0x8AB dash): {move, dash} on enter, {6, 0} on leave; nothing for LocationId 0.
// `this` is unused; the game's instance is the singleton at 0x14939BCE8.
constexpr std::uintptr_t kSetLayer7 = 0x14182BE00;
constexpr std::uint8_t   kSetLayer7Bytes[] = {0x48, 0x85, 0xD2, 0x74, 0x54, 0x48, 0x89, 0x5C,
                                              0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kMoveTypeListener = 0x14939BCE8;
constexpr std::uint32_t  kControllerLayer7Move = 0x890 + 7;
constexpr std::uint32_t  kControllerLayer7Dash = 0x8A4 + 7;

// §4.3.1: the location manager (a pointer), its volume array, lock, pause byte and move-type stack; each
// AEndLocationVolume's fields; and the game's containment test LocationVolume_ContainsPoint(volume, &position, margin).
constexpr std::uintptr_t kLocationManager    = 0x14939BCA0;
constexpr std::uint32_t  kManagerVolumes     = 0x08;
constexpr std::uint32_t  kManagerVolumeCount = 0x10;
constexpr std::uint32_t  kManagerStack       = 0xF8;   // {int32 refcount; u8 move; u8 dash; pad} entries
constexpr std::uint32_t  kManagerStackCount  = 0x100;
constexpr std::uint32_t  kManagerPaused      = 0x128;  // byte: the game's update does nothing while set
constexpr std::uint32_t  kManagerLock        = 0x188;
constexpr std::uint32_t  kVolumeLocationId   = 0x270;
constexpr std::uint32_t  kVolumeUntouchMargin = 0x278;
constexpr std::uint32_t  kVolumeFlags        = 0x280;
constexpr std::uint32_t  kVolumeMoveType     = 0x2E9;  // EEndSpecificMoveType
constexpr std::uint32_t  kVolumeDashType     = 0x2EA;  // EEndDashType
constexpr std::uint32_t  kVolumeForcedOut    = 0x308;  // bool: never entered by position; a forced volume leaves
constexpr std::uint32_t  kFlagDisabled = 0x1, kFlagForce = 0x2, kFlagChangeMovementType = 0x8000;
constexpr std::int32_t   kMaxVolumes   = 4096;
constexpr std::uintptr_t kContainsPoint = 0x140A9F840;
constexpr std::uint8_t   kContainsPointBytes[] = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xE4,
                                                  0xF6, 0x60, 0x01, 0x48, 0x2B, 0xE0, 0x33, 0xDB};

constexpr int       kMaxInside  = 32;   // move-type volumes player 2 stands in at once (a handful in practice)
constexpr ULONGLONG kRetryMs    = 250;  // a write the game's gate declined is tried again this often
constexpr ULONGLONG kStateMs    = 1000;
constexpr int       kMaxLines   = 400;

using CreateNavAreaFn = void (*)(void* partyManager, void* controller);
using SetLayer7Fn     = void (*)(void* listener, std::uint64_t location, std::uint8_t move, std::uint8_t dash,
                             std::uint8_t enter);
using ContainsFn      = bool (*)(const void* volume, const float* position, float margin);

CreateNavAreaFn g_createNavArea = nullptr;
SetLayer7Fn     g_setLayer7     = nullptr;
ContainsFn      g_contains      = nullptr;
std::uintptr_t  g_partyManager  = 0;  // addresses of the globals holding the pointers
std::uintptr_t  g_listener      = 0;
std::uintptr_t  g_manager       = 0;

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
/// Player 2's copy of the manager's per-volume "inside" bits and its move-type stack (index 0 = top).
struct State
{
    void*         controller = nullptr;
    void*         member     = nullptr;
    Inside        inside[kMaxInside];
    int           insideCount = 0;
    world_rules::PairStack<Pair, kMaxInside> stack;  // every entry is held by an inside volume, so never more
    std::uint64_t gateLocation = 0;  // a LocationId for the listener's "not 0" gate
};
State     g_state;
void*     g_navTried   = nullptr;  // the controller the nav-area creation ran (or was found) for
ULONGLONG g_nextRetry  = 0;
ULONGLONG g_nextState  = 0;
int       g_lines      = 0;
bool      g_overflowed = false;
std::wstring g_lastState, g_lastCall;

/// A line of `level`; a line above the current level uses none of the budget.
template <class... A>
void LineAt(platform::Level level, std::wformat_string<A...> format, A&&... args) noexcept
{
    if (g_lines >= kMaxLines || !platform::Enabled(level))
    {
        return;
    }
    try
    {
        ++g_lines;
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

template <class T>
bool Read(const void* object, std::uint32_t offset, T& out) noexcept
{
    return object != nullptr && platform::CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

void* Global(std::uintptr_t address) noexcept
{
    void* value = nullptr;
    return address != 0 && platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(value)) ? value
                                                                                                                 : nullptr;
}

std::wstring PairText(Pair pair)
{
    return pair == kNoLimit ? std::wstring(L"none") : std::format(L"{}/{}", pair.move, pair.dash);
}

/// The game's push: a pair already on the stack gains a reference; a new one goes to the front.
void Push(Pair pair) noexcept
{
    g_state.stack.Push(pair);
}

/// The game's pop: the pair loses a reference and goes at 0.
void Pop(Pair pair) noexcept
{
    g_state.stack.Pop(pair);
}

/// Once per controller: the game creates player 2's nav-area component, run as the primary player's.
void EnsureNavArea(void* controller) noexcept
{
    if (g_createNavArea == nullptr || g_navTried == controller)
    {
        return;
    }
    if (ReadPointer(controller, kControllerNavArea) != nullptr)
    {
        g_navTried = controller;
        Line(L"player 2's controller already has its nav-area component (kept)");
        return;
    }
    void* partyManager = Global(g_partyManager);
    if (partyManager == nullptr)
    {
        return;  // tried again next tick
    }
    bool ran = false;
    {
        const primary_controller::Scope scope;
        if (!scope.Held())
        {
            return;  // tried again next tick; never unscoped (it would make player 2 the primary player for good)
        }
        g_navTried = controller;
        ran        = platform::CallGuarded(g_createNavArea, partyManager, controller);
    }
    void* component = ReadPointer(controller, kControllerNavArea);
    Line(L"player 2's nav-area component by the game's own creation: {} (component {}, its controller is player 2's: {})",
         ran ? L"ran" : L"FAULTED", component, component != nullptr && ReadPointer(component, kNavAreaController) == controller);
}

/// The leaves and enters player 2's member makes this tick, by the game's rules. False if the manager is busy,
/// paused or unreadable (nothing changes then).
struct Changes
{
    bool   leave[kMaxInside] = {};
    Inside enter[kMaxInside];
    int    enterCount = 0;
    Pair   managerTop = kNoLimit;  // the game's own (player 1's) top, for the log
};

bool Walk(const float (&position)[3], Changes& changes) noexcept
{
    void* manager = Global(g_manager);
    if (manager == nullptr)
    {
        return false;
    }
    auto* lock    = reinterpret_cast<CRITICAL_SECTION*>(static_cast<std::byte*>(manager) + kManagerLock);
    BOOL  entered = FALSE;
    if (!platform::CallGuarded(&TryEnterCriticalSection, entered, lock) || !entered)
    {
        return false;
    }
    std::uint8_t paused  = 0;
    void* const* volumes = nullptr;
    std::int32_t count   = 0;
    bool readable = Read(manager, kManagerPaused, paused) && paused == 0 && Read(manager, kManagerVolumes, volumes) &&
                    Read(manager, kManagerVolumeCount, count) && count >= 0 && count <= kMaxVolumes &&
                    (count == 0 || volumes != nullptr);
    bool seen[kMaxInside] = {};
    for (std::int32_t i = 0; readable && i < count; ++i)
    {
        const void*   volume   = nullptr;
        std::uint32_t flags    = 0;
        std::uint64_t location = 0;
        if (!platform::CopyGuarded(&volume, volumes + i, sizeof(volume)) || volume == nullptr ||
            !Read(volume, kVolumeFlags, flags) || (flags & kFlagChangeMovementType) == 0 ||
            !Read(volume, kVolumeLocationId, location) || location == 0)
        {
            continue;
        }
        g_state.gateLocation = g_state.gateLocation != 0 ? g_state.gateLocation : location;
        int current = -1;  // matched by LocationId as well as address: a streamed-in volume may reuse an address
        for (int k = 0; k < g_state.insideCount; ++k)
        {
            if (g_state.inside[k].volume == volume && g_state.inside[k].location == location)
            {
                current = k;
                seen[k] = true;
                break;
            }
        }
        if ((flags & kFlagDisabled) != 0)
        {
            continue;  // the game's rule: no enter, no leave, the state kept
        }
        bool in = false;
        if ((flags & kFlagForce) != 0)
        {
            std::uint8_t forcedOut = 0;
            if (!Read(volume, kVolumeForcedOut, forcedOut))
            {
                continue;
            }
            in = world_rules::ForcedInside(current >= 0, forcedOut);
        }
        else
        {
            float margin = 0.0f;
            if ((current >= 0 && !Read(volume, kVolumeUntouchMargin, margin)) ||
                !platform::CallGuarded(g_contains, in, volume, &position[0], current >= 0 ? margin : 0.0f))
            {
                continue;  // unreadable: this volume's state is kept this tick
            }
        }
        if (current >= 0 && !in)
        {
            changes.leave[current] = true;
            g_state.gateLocation   = location;
        }
        else if (current < 0 && in)
        {
            Inside here{volume, location};
            if (!Read(volume, kVolumeMoveType, here.pair.move) || !Read(volume, kVolumeDashType, here.pair.dash))
            {
                continue;
            }
            if (changes.enterCount < kMaxInside)
            {
                changes.enter[changes.enterCount++] = here;
                g_state.gateLocation                = location;
            }
        }
    }
    std::int32_t stackCount = 0;
    const std::byte* stack  = nullptr;
    if (readable && Read(manager, kManagerStackCount, stackCount) && stackCount > 0 && Read(manager, kManagerStack, stack))
    {
        Read(stack, 4, changes.managerTop.move);
        Read(stack, 5, changes.managerTop.dash);
    }
    if (!platform::CallGuarded(&LeaveCriticalSection, lock))
    {
        readable = false;
    }
    if (!readable)
    {
        return false;
    }
    for (int k = 0; k < g_state.insideCount; ++k)
    {
        changes.leave[k] = changes.leave[k] || !seen[k];  // gone from the list (streamed out): a leave
    }
    return true;
}

/// The game's order: every leave of the tick, then every enter.
void Apply(const Changes& changes) noexcept
{
    int kept = 0;
    for (int k = 0; k < g_state.insideCount; ++k)
    {
        if (changes.leave[k])
        {
            Pop(g_state.inside[k].pair);  // the pair stored at the enter, as for a volume gone from the list
        }
        else
        {
            g_state.inside[kept++] = g_state.inside[k];
        }
    }
    g_state.insideCount = kept;
    for (int e = 0; e < changes.enterCount; ++e)
    {
        if (g_state.insideCount >= kMaxInside)
        {
            if (!g_overflowed)
            {
                g_overflowed = true;
                Line(L"player 2 stands in more than {} move-type volumes; the rest are not counted", kMaxInside);
            }
            break;
        }
        g_state.inside[g_state.insideCount++] = changes.enter[e];
        Push(changes.enter[e].pair);
    }
}

Pair ControllerLayer7(const void* controller) noexcept
{
    Pair pair{0xFF, 0xFF};
    Read(controller, kControllerLayer7Move, pair.move);
    Read(controller, kControllerLayer7Dash, pair.dash);
    return pair;
}

/// Writes player 2's layer 7 with the game's own listener method, as the primary player's.
void Write(void* controller, Pair want, Pair before) noexcept
{
    const ULONGLONG now = GetTickCount64();
    void*           listener = Global(g_listener);
    if (now < g_nextRetry || listener == nullptr || g_state.gateLocation == 0)
    {
        return;
    }
    bool ran = false, held = false;
    {
        const primary_controller::Scope scope;
        held = scope.Held();
        if (held)
        {
            const bool limit = !(want == kNoLimit);
            ran = platform::CallGuarded(g_setLayer7, listener, g_state.gateLocation, want.move, want.dash,
                                        static_cast<std::uint8_t>(limit ? 1 : 0));
        }
    }
    const Pair after = ControllerLayer7(controller);
    g_nextRetry      = after == want ? 0 : now + kRetryMs;
    try
    {
        std::wstring call = std::format(L"player 2's layer 7 {} -> {}: {}", PairText(before), PairText(want),
                                        !held  ? L"not written (the primary scope was refused; tried again)"
                                        : !ran ? L"FAULTED"
                                        : after == want ? L"written by the game's listener method, as the primary player"
                                                        : L"the game's gate declined (the field leader is not ready; tried again)");
        if (call != g_lastCall)
        {
            g_lastCall = call;
            Line(L"{}", call);
        }
    }
    catch (...)
    {
    }
}

void Reset(void* controller, void* member) noexcept
{
    g_state            = {};
    g_state.controller = controller;
    g_state.member     = member;
    g_nextRetry        = 0;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto create   = image.Rebase(kCreateNavArea);
    const auto party    = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto set      = image.Rebase(kSetLayer7);
    const auto listener = image.Rebase(kMoveTypeListener, sizeof(void*));
    const auto manager  = image.Rebase(kLocationManager, sizeof(void*));
    const auto contains = image.Rebase(kContainsPoint);
    if (create && party && primary_controller::Installed() && image.BytesMatch(kCreateNavArea, kCreateNavAreaBytes))
    {
        g_createNavArea = reinterpret_cast<CreateNavAreaFn>(*create);
        g_partyManager  = *party;
    }
    if (set && listener && manager && contains && primary_controller::Installed() &&
        image.BytesMatch(kSetLayer7, kSetLayer7Bytes) && image.BytesMatch(kContainsPoint, kContainsPointBytes))
    {
        g_setLayer7 = reinterpret_cast<SetLayer7Fn>(*set);
        g_listener  = *listener;
        g_manager   = *manager;
        g_contains  = reinterpret_cast<ContainsFn>(*contains);
    }
    platform::Log(L"walk limits: player 2's nav-area component by the game's creation {}; player 2's location walk "
                  L"limit from player 2's own position {}",
                  g_createNavArea != nullptr ? L"yes" : L"NO (a check failed)",
                  g_setLayer7 != nullptr ? L"yes" : L"NO (a check failed)");
    return g_createNavArea != nullptr || g_setLayer7 != nullptr;
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    void* const controller = generation::Active() ? generation::PlayerTwoController() : nullptr;
    void* const member     = controller != nullptr ? generation::Member() : nullptr;
    if (controller != g_state.controller || member != g_state.member)
    {
        Reset(controller, member);  // take, leave, rejoin or another member: rebuilt from the position next
    }
    if (member == nullptr)
    {
        return;
    }
    EnsureNavArea(controller);
    float position[3]{};
    Changes changes;
    if (g_setLayer7 == nullptr || !players::unreal::ActorLocation(member, position) || !Walk(position, changes))
    {
        return;
    }
    Apply(changes);
    const Pair want   = g_state.stack.Count() > 0 ? g_state.stack.At(0).pair : kNoLimit;
    const Pair before = ControllerLayer7(controller);
    if (!(before == want))
    {
        Write(controller, want, before);
    }

    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextState && platform::Enabled(platform::Level::Debug))
    {
        g_nextState = now + kStateMs;
        try
        {
            std::wstring state = std::format(L"player 1's layer 7 {} (the game's top {}); player 2's {} (own top {}, {} "
                                             L"volumes, {} stack entries)",
                                             PairText(ControllerLayer7(generation::PlayerOneController())),
                                             PairText(changes.managerTop), PairText(ControllerLayer7(controller)),
                                             PairText(want), g_state.insideCount, g_state.stack.Count());
            if (state != g_lastState)
            {
                g_lastState = state;
                LineAt(platform::Level::Debug, L"{}", state);
            }
        }
        catch (...)
        {
        }
    }
}

}  // namespace lss::player_two::walk_limits
