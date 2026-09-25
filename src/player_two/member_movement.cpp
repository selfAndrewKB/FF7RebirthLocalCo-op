#include "player_two/member_movement.h"

#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/engine_guard.h"
#include "player_two/generation.h"
#include "player_two/move_state.h"
#include "player_two/object_fields.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"

#include <atomic>

namespace lss::player_two::member_movement {

namespace {

using TickForCharacterFn = void (*)(void* moveState, float deltaTime);
using LeaderTestFn       = std::uint8_t (*)(const void* component);
using PlayerTypeFn       = std::uint8_t (*)(const void* component);

constexpr int kNoType = -1;

// Set once at install.
platform::VTableEntry g_leaderSlot;
TickForCharacterFn    g_tickForCharacter = nullptr;
LeaderTestFn          g_engineLeaderTest = nullptr;
std::uintptr_t        g_moveStateVTable  = 0;

// The player type the leader test says yes to; kNoType outside the member's tick. Written on the
// game thread; the leader test is also called on other threads, which read kNoType.
std::atomic<int>      g_leaderType{kNoType};
std::atomic<unsigned> g_leaderAnswers{0};

// Game thread only: set once the engine raised an exception inside the member's tick.
bool g_stopped = false;

StageCounters g_counters;

int PlayerTypeOfComponent(const void* component) noexcept
{
    const auto vtable = reinterpret_cast<const void*>(VTableOf(component));
    return vtable == nullptr ? kNoType : ReadField<PlayerTypeFn>(vtable, facts::kPlayerTypeSlotOffset)(component);
}

std::uint8_t LeaderTest(const void* component)
{
    const int type = g_leaderType.load(std::memory_order_relaxed);
    if (type != kNoType && generation::OnGameThread() && PlayerTypeOfComponent(component) == type)
    {
        g_leaderAnswers.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return g_engineLeaderTest(component);
}

/// Answers yes to the leader test for one player type while it lives.
class LeaderAnswer
{
public:
    explicit LeaderAnswer(int type) noexcept { g_leaderType.store(type, std::memory_order_relaxed); }
    ~LeaderAnswer() { g_leaderType.store(kNoType, std::memory_order_relaxed); }
    LeaderAnswer(const LeaderAnswer&)            = delete;
    LeaderAnswer& operator=(const LeaderAnswer&) = delete;
};

int MemberType(const void* member) noexcept
{
    const auto type = party_member::PlayerTypeOf(member);
    return type ? *type : kNoType;
}

/// The engine's tick inside a structured-exception guard (player_two/engine_guard.h).
bool GuardedTick(void* state, float deltaTime) noexcept
{
    __try
    {
        g_tickForCharacter(state, deltaTime);
        return true;
    }
    __except (EngineGuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Ticks the member's state inside every scope, or not at all.
void TickInScopes(void* playerTwoController, void* state, int type, float deltaTime) noexcept
{
    const pad_sticks::StickScope    sticks(pad_sticks::ControllerIdOf(playerTwoController));
    const primary_controller::Scope primary;
    if (!sticks.Held() || !primary.Held())
    {
        g_counters.Refuse();
        return;
    }
    const LeaderAnswer        leader(type);
    const move_state::OwnTick own;
    g_counters.Act();
    if (!GuardedTick(state, deltaTime))
    {
        g_stopped = true;
    }
}

/// The member's move state, if the member is player 2's possessed character and the tick's
/// unchecked reads hold (the controller's leader-action component).
void* MoveStateToTick(void* playerTwoController, void* member) noexcept
{
    void* state = ReadPointer(member, facts::kCharacterMoveState);
    if (VTableOf(state) != g_moveStateVTable || ReadPointer(state, facts::kMoveStateCharacter) != member ||
        ReadPointer(member, facts::kPawnController) != playerTwoController ||
        ReadPointer(playerTwoController, facts::kControllerLeaderActions) == nullptr)
    {
        return nullptr;
    }
    return state;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto tick       = platform::FindFunction(image, facts::kMoveStateTickForCharacter);
    const auto leaderTest = platform::FindFunction(image, facts::kIsPartyLeader.target);
    const auto vtable     = image.Rebase(facts::kMoveStateVTable);
    if (!tick || !leaderTest || !vtable || !party_member::Installed() || !primary_controller::Installed() ||
        !pad_sticks::Installed())
    {
        return false;
    }
    g_tickForCharacter = reinterpret_cast<TickForCharacterFn>(*tick);
    g_engineLeaderTest = reinterpret_cast<LeaderTestFn>(*leaderTest);
    g_moveStateVTable  = *vtable;
    return platform::ReplaceSlot(image, facts::kIsPartyLeader, reinterpret_cast<std::uintptr_t>(&LeaderTest),
                                 g_leaderSlot);
}

void Remove() noexcept
{
    if (!g_leaderSlot.Restore())
    {
        platform::Log(L"could not restore the leader-test slot; the replacement stays in place");
    }
}

void Tick(void* playerTwoController, void* member) noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    const auto engine = move_state::TakeEngineTicks();
    if (!pad_sticks::Enabled() || !generation::Active() || g_tickForCharacter == nullptr || g_stopped)
    {
        return;
    }
    if (engine.count == 0 || engine.memberTicked)
    {
        g_counters.Pass();  // the game's field ticker is idle (a menu, an event), or ran the member itself
        return;
    }
    void*     state = MoveStateToTick(playerTwoController, member);
    const int type  = MemberType(member);
    if (state == nullptr || type == kNoType)
    {
        g_counters.Refuse();
        return;
    }
    TickInScopes(playerTwoController, state, type, engine.deltaTime);
    if (g_stopped)
    {
        platform::Log(L"member movement STOPPED: the engine raised an exception inside the member's move-state tick; "
                      L"player 2's member is no longer ticked this session");
    }
}

StageCounters& Counters() noexcept
{
    return g_counters;
}

unsigned TakeLeaderAnswers() noexcept
{
    return g_leaderAnswers.exchange(0, std::memory_order_relaxed);
}

}  // namespace lss::player_two::member_movement
