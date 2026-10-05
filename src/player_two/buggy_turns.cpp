#include "player_two/buggy_turns.h"

#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"

#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::buggy_turns {

namespace {

// AEndPlayerController::ActiveVehicleCoordinator (SDK; RE-p2-control-mode-object.md).
constexpr std::uint32_t kActiveVehicle = 0x5B8;
// UEndPlayerBuggyCoordinator: its vtable, VehicleChara, and the pointer to its state id (3 boarding, 4 riding,
// 5 getting off; RE-p2-control-mode-object.md, read live by player_two/ride_watch).
constexpr std::uintptr_t kBuggyCoordVt  = 0x145D77220;
constexpr std::uint32_t  kCoordBody     = 0x30;
constexpr std::uint32_t  kBuggyStatePtr = 0x1E8;

// Set once at install.
std::uintptr_t g_buggyVt = 0;

// Game thread.
bool          g_riding    = false;
std::int32_t  g_state     = -1;       // the buggy's state as last logged
void*         g_given     = nullptr;  // the member player 2 played when the ride began (to log where the game puts it)
std::uint64_t g_givenWeak = 0;        // its weak pointer: nothing is read once it no longer names that member

std::int32_t StateOf(const void* coordinator) noexcept
{
    const void* state = ReadPointer(coordinator, kBuggyStatePtr);
    return state != nullptr ? ReadField<std::int32_t>(state, 0) : -1;
}

/// The member's AI state type (its retained AI controller's active brain's state), or -1.
std::int32_t AiStateOf(const void* character) noexcept
{
    const void* ai    = ReadPointer(character, facts::kCharacterRetainedAi);
    const void* brain = ReadPointer(ai, facts::kAiActiveBrain);
    const void* state = ReadPointer(brain, facts::kBrainState);
    return state != nullptr ? ReadField<std::uint8_t>(state, facts::kAiStateType) : -1;
}

/// Where the member player 2 gave back is, against the buggy: what the game's boarding did with it.
void LogMember(const void* coordinator, std::int32_t state)
{
    if (g_given == nullptr || !leader_scope::StillNames(g_givenWeak, g_given))
    {
        return;
    }
    float      member[3]{}, buggy[3]{};
    void*      body = ReadPointer(coordinator, kCoordBody);
    const bool both = charapop::ActorLocation(g_given, member) && body != nullptr && charapop::ActorLocation(body, buggy);
    platform::Log(L"buggy turns: buggy state {}: the member player 2 gave back, {}, is {}; driven by {}, AI state {}",
                  state, NameOf(g_given),
                  both ? std::format(L"{:.1f} m from the buggy", charapop::Distance(member, buggy) / 100.0f)
                       : std::wstring{L"at a position that cannot be read"},
                  NameOf(ReadPointer(g_given, facts::kCharacterEffectiveController)), AiStateOf(g_given));
}

bool Run()
{
    void*       playerOne   = generation::Active() ? generation::PlayerOneController() : nullptr;
    const void* coordinator = ReadPointer(playerOne, kActiveVehicle);
    const bool  riding      = coordinator != nullptr && VTableOf(coordinator) == g_buggyVt;
    if (riding != g_riding)
    {
        g_riding = riding;
        if (riding)
        {
            g_given     = generation::Member();  // read before the session gives it back
            g_givenWeak = g_given != nullptr ? leader_scope::WeakOf(g_given) : 0;
            platform::Log(L"buggy turns: player 1 rides the buggy: player 2 stands aside ({}), player 1's view alone, "
                          L"every pad drives",
                          g_given != nullptr ? std::format(L"{} goes back to its AI", NameOf(g_given))
                                             : std::wstring{L"no member held"});
        }
        else
        {
            platform::Log(L"buggy turns: the buggy ride is over{}: player 2 takes the 2nd slot's member again; the split "
                          L"and per-pad input come back",
                          playerOne == nullptr ? L" (or player 2 left)" : L"");
            g_given     = nullptr;
            g_givenWeak = 0;
            g_state     = -1;
        }
    }
    if (riding)
    {
        const std::int32_t state = StateOf(coordinator);
        if (state != g_state)
        {
            g_state = state;
            LogMember(coordinator, state);
        }
    }
    return riding;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto vt = image.Rebase(kBuggyCoordVt);
    g_buggyVt     = vt ? *vt : 0;
    platform::Log(L"buggy turns: {}", g_buggyVt != 0 ? L"installed (while player 1 rides the buggy, player 2 stands aside: "
                                                       L"player 1's view alone, every pad drives the buggy)"
                                                     : L"UNAVAILABLE (the buggy coordinator's vtable is not where expected)");
    return g_buggyVt != 0;
}

bool Tick() noexcept
{
    if (g_buggyVt == 0 || !generation::OnGameThread())
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

}  // namespace lss::player_two::buggy_turns
