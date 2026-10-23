#include "player_two/rides/shared_ride.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/rides/ride_rules.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/world/charapop_facts.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::shared_ride {

namespace {

// AEndPlayerController::ActiveVehicleCoordinator (SDK; RE-p2-control-mode-object.md).
using facts::kControllerActiveVehicle;
// A coordinator's VehicleChara (the buggy, the boat).
using facts::kCoordinatorBody;
// APawn::Controller, and AActor's bHidden bit (RE-p2-tiny-bronco.md logging plan).
using facts::kPawnController;
constexpr std::uint32_t kActorHidden    = 0x58;
constexpr std::uint8_t  kHiddenBit      = 0x20;

/// A vehicle the whole party shares: its coordinator's vtable, the pointer to its state id, and its getting-off state.
struct Kind
{
    const wchar_t* name;
    std::uintptr_t vtable;
    std::uint32_t  statePtr;
    std::int32_t   gettingOff;
};
constexpr std::array<Kind, 2> kKinds = {{
    {L"buggy", facts::kBuggyCoordinatorVTable, 0x1E8, 5},  // RE-p2-control-mode-object.md (states read live)
    {L"bronco", facts::kBroncoCoordinatorVTable, 0x70, 4},  // RE-p2-tiny-bronco.md
}};
constexpr std::int32_t kIdle = 0;

using GlobalTestFn = std::uint8_t (*)();

// Set once at install.
std::array<std::uintptr_t, kKinds.size()> g_vt{};
std::uintptr_t g_partyManager = 0;  // the global holding the party manager; the manager's byte +0 is "party busy" (0: idle)
GlobalTestFn   g_inBattle     = nullptr;

// Game thread.
bool          g_riding     = false;
std::size_t   g_kind       = 0;
std::int32_t  g_state      = -1;       // the vehicle's state as last logged
bool          g_gotOff     = false;    // the ride passed through its getting-off state
std::int32_t  g_battle     = -1;       // as last logged during the ride
void*         g_given      = nullptr;  // the member player 2 played when the ride began (to log where the game puts it)
std::uint64_t g_givenWeak  = 0;        // its weak pointer: nothing is read once it no longer names that member

std::int32_t StateOf(const void* coordinator, const Kind& kind) noexcept
{
    const void* state = ReadPointer(coordinator, kind.statePtr);
    return state != nullptr ? ReadField<std::int32_t>(state, 0) : -1;
}

/// The shared vehicle the coordinator drives (an index into kKinds), or kKinds.size() for none.
std::size_t KindOf(const void* coordinator) noexcept
{
    const std::uintptr_t vt   = VTableOf(coordinator);
    std::size_t          kind = kKinds.size();
    for (std::size_t i = 0; coordinator != nullptr && i < kKinds.size(); ++i)
    {
        kind = g_vt[i] != 0 && vt == g_vt[i] ? i : kind;
    }
    return kind;
}

/// The member's AI state type (its retained AI controller's active brain's state), or -1.
std::int32_t AiStateOf(const void* character) noexcept
{
    const void* ai    = ReadPointer(character, facts::kCharacterRetainedAi);
    const void* brain = ReadPointer(ai, facts::kAiActiveBrain);
    const void* state = ReadPointer(brain, facts::kBrainState);
    return state != nullptr ? ReadField<std::uint8_t>(state, facts::kAiStateType) : -1;
}

std::int32_t InBattle() noexcept
{
    std::uint8_t answer = 0;
    return g_inBattle != nullptr && platform::CallGuarded(g_inBattle, answer) ? answer : -1;
}

std::int32_t PartyBusy() noexcept
{
    const void* manager = g_partyManager != 0 ? ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0) : nullptr;
    return manager != nullptr ? ReadField<std::uint8_t>(manager, 0) : -1;
}

/// The vehicle's state, and where the member player 2 gave back is against it: what the game does with it.
void LogState(const void* coordinator, std::int32_t state)
{
    void*       body  = ReadPointer(coordinator, kCoordinatorBody);
    std::wstring member = L"no member was given back";
    if (g_given != nullptr && leader_scope::StillNames(g_givenWeak, g_given))
    {
        float      at[3]{}, vehicle[3]{};
        const bool both = charapop::ActorLocation(g_given, at) && body != nullptr && charapop::ActorLocation(body, vehicle);
        member = std::format(L"the member player 2 gave back, {}, is {}, hidden {}; driven by {}, AI state {}", NameOf(g_given),
                             both ? std::format(L"{:.1f} m from the {}", charapop::Distance(at, vehicle) / 100.0f,
                                                kKinds[g_kind].name)
                                  : std::wstring{L"at a position that cannot be read"},
                             (ReadField<std::uint8_t>(g_given, kActorHidden) & kHiddenBit) != 0 ? 1 : 0,
                             NameOf(ReadPointer(g_given, facts::kCharacterEffectiveController)), AiStateOf(g_given));
    }
    platform::Log(L"shared ride: {} state {}: {}; the {} {} is driven by {}; party busy {}, in battle {}", kKinds[g_kind].name,
                  state, member, kKinds[g_kind].name, NameOf(body), NameOf(ReadPointer(body, kPawnController)), PartyBusy(),
                  InBattle());
}

bool Run()
{
    void*       playerOne   = generation::Active() ? generation::PlayerOneController() : nullptr;
    const void* coordinator = ReadPointer(playerOne, kControllerActiveVehicle);
    const std::size_t  kind   = KindOf(coordinator);
    const std::int32_t state  = kind < kKinds.size() ? StateOf(coordinator, kKinds[kind]) : -1;
    const bool         riding = ride_rules::SharedRiding(kind < kKinds.size(), state, kIdle);
    if (ride_rules::RideChanged(g_riding, g_kind, riding, kind))
    {
        if (g_riding)
        {
            platform::Log(L"shared ride: the {} ride is over ({}){}: player 2 takes the 2nd slot's member again; the split "
                          L"and per-pad input come back",
                          kKinds[g_kind].name, g_gotOff ? L"through getting off" : L"immediately, without getting off",
                          playerOne == nullptr ? L" (or player 2 left)" : L"");
        }
        g_riding = riding;
        g_given  = nullptr;
        g_givenWeak = 0;
        g_state  = -1;
        g_gotOff = false;
        g_battle = -1;
        if (riding)
        {
            g_kind      = kind;
            g_given     = generation::Member();  // read before the session gives it back
            g_givenWeak = g_given != nullptr ? leader_scope::WeakOf(g_given) : 0;
            platform::Log(L"shared ride: player 1 rides the {}: player 2 stands aside ({}), player 1's view alone, every pad "
                          L"drives",
                          kKinds[kind].name,
                          g_given != nullptr ? std::format(L"{} goes back to its AI", NameOf(g_given))
                                             : std::wstring{L"no member held"});
        }
    }
    if (!riding)
    {
        return false;
    }
    if (state != g_state)
    {
        g_state  = state;
        g_gotOff = g_gotOff || state == kKinds[kind].gettingOff;
        LogState(coordinator, state);
    }
    const std::int32_t battle = InBattle();
    if (battle != g_battle)
    {
        if (g_battle != -1)
        {
            platform::Log(L"shared ride: a battle {} during the {} ride (its state {})", battle == 1 ? L"started" : L"ended",
                          kKinds[kind].name, state);
        }
        g_battle = battle;
    }
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    bool any = false;
    for (std::size_t i = 0; i < kKinds.size(); ++i)
    {
        const auto vt = image.Rebase(kKinds[i].vtable);
        g_vt[i]       = vt ? *vt : 0;
        any           = any || g_vt[i] != 0;
    }
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    const auto battle  = platform::FindFunction(image, facts::kIsInBattle);
    g_partyManager     = manager ? *manager : 0;
    g_inBattle         = battle ? reinterpret_cast<GlobalTestFn>(*battle) : nullptr;
    platform::Log(L"shared ride: buggy {}, bronco {} (while player 1 rides one, player 2 stands aside: player 1's view alone, "
                  L"every pad drives)",
                  g_vt[0] != 0 ? L"installed" : L"UNAVAILABLE", g_vt[1] != 0 ? L"installed" : L"UNAVAILABLE");
    return any;
}

bool Rides(const void* playerOneController) noexcept
{
    if (!generation::OnGameThread())
    {
        return false;
    }
    const void*       coordinator = ReadPointer(playerOneController, kControllerActiveVehicle);
    const std::size_t kind        = KindOf(coordinator);
    return kind < kKinds.size() && StateOf(coordinator, kKinds[kind]) != kIdle;
}

bool Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return false;
    }
    try
    {
        return Run();
    }
    catch (...)
    {
        return g_riding;  // a log line's allocation failed; the ride's state is kept
    }
}

}  // namespace lss::player_two::shared_ride
