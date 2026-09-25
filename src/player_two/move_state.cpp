#include "player_two/move_state.h"

#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/pad_sticks.h"

#include <array>
#include <atomic>

namespace lss::player_two::move_state {

namespace {

/// Slot 11 of both move-state vtables. Callers ignore the result; rax is passed through whole.
using TickFn = std::uintptr_t (*)(void* state, float deltaTime);

using CharacterTestFn  = std::uint8_t (*)(const void* character);
using ActorFn          = void* (*)();
using ControlOwnerFn   = void* (*)(const void* state);
using HasStateTagFn    = std::uint8_t (*)(const void* controlOwner, std::uint8_t tag);

struct Gate
{
    CharacterTestFn isPlayerCore  = nullptr;  // IsUCPC_Core
    CharacterTestFn isPrimaryOnes = nullptr;  // the primary controller's character
    ActorFn         specialActor  = nullptr;  // the third alternative's character
    ControlOwnerFn  controlOwner  = nullptr;  // the object the state tags are read from
    HasStateTagFn   hasStateTag   = nullptr;
};

// Set once at install.
platform::VTableEntry g_tickSlot;
platform::VTableEntry g_tickInputSlot;
TickFn                g_engineTick      = nullptr;
TickFn                g_engineTickInput = nullptr;
Gate                  g_gate;
bool                  g_gateFound = false;

// Game thread only.
bool        g_ownTick = false;
EngineTicks g_engineTicks;

/// Ticks between samples of one character's gate tests.
constexpr std::uint32_t kSampleEvery = 30;

/// What one character's move state did since the last census line. Only game-thread ticks are
/// counted, and the status line is written on the game thread too; the atomics keep whole values.
struct Entry
{
    std::atomic<void*>         character{nullptr};
    std::atomic<std::uint32_t> ticks{0};       // PlayerMoveState::Tick
    std::atomic<std::uint32_t> inputTicks{0};  // the base class, straight to TickInput
    std::atomic<std::uint32_t> ownTicks{0};    // of those, made by the mod
    std::uint32_t              samples     = 0;
    std::uint32_t              playerCore  = 0;
    std::uint32_t              primaryOnes = 0;
    std::uint32_t              special     = 0;
    std::uint32_t              tag17       = 0;
    std::uint32_t              tag22       = 0;
    std::uint32_t              blocked     = 0;  // state +0x91
    std::uint32_t              cooling     = 0;  // state +0x94 > 0
};
std::array<Entry, 4> g_entries;

Entry* EntryFor(void* character) noexcept
{
    for (Entry& entry : g_entries)
    {
        void* held = entry.character.load(std::memory_order_relaxed);
        if (held == character)
        {
            return &entry;
        }
        if (held == nullptr)
        {
            entry.character.store(character, std::memory_order_relaxed);
            return &entry;
        }
    }
    return nullptr;
}

/// Player 2's member and player 1's pawn and leader: the only characters counted.
bool OfInterest(const void* character) noexcept
{
    void* one = generation::PlayerOneController();
    return character != nullptr &&
           (character == generation::Member() || character == ReadPointer(one, facts::kControllerPawn) ||
            character == ReadPointer(one, facts::kControllerLeader));
}

/// Evaluates the per-character and per-state gate tests one by one (the gate itself starts with a
/// global test, so its answer would not say why a character is refused).
void Sample(Entry& entry, const void* state, const void* character) noexcept
{
    ++entry.samples;
    entry.playerCore += g_gate.isPlayerCore(character) != 0 ? 1 : 0;
    entry.primaryOnes += g_gate.isPrimaryOnes(character) != 0 ? 1 : 0;
    entry.special += g_gate.specialActor() == character ? 1 : 0;
    const void* owner = g_gate.controlOwner(state);
    if (owner != nullptr)
    {
        entry.tag17 += g_gate.hasStateTag(owner, facts::kMoveBlockingTagA) != 0 ? 1 : 0;
        entry.tag22 += g_gate.hasStateTag(owner, facts::kMoveBlockingTagB) != 0 ? 1 : 0;
    }
    entry.blocked += ReadField<std::uint8_t>(state, facts::kMoveStateBlocked) != 0 ? 1 : 0;
    entry.cooling += ReadField<float>(state, facts::kMoveStateCooldown) > 0.0f ? 1 : 0;
}

void Count(void* state, void* character, bool viaTickInput) noexcept
{
    Entry* entry = OfInterest(character) ? EntryFor(character) : nullptr;
    if (entry == nullptr)
    {
        return;
    }
    if (g_ownTick)
    {
        entry->ownTicks.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint32_t ticks =
        (viaTickInput ? entry->inputTicks : entry->ticks).fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_gateFound && ticks % kSampleEvery == 1)
    {
        Sample(*entry, state, character);
    }
}

/// Runs one move-state tick. On the game thread it is counted, and the game's own ticks are
/// recorded; with per-pad sticks on, player 1's character reads only player 1's pad. (Player 2's
/// member is ticked by the mod with its scopes already open: player_two/member_movement.h.)
std::uintptr_t RunForOwner(TickFn engine, void* state, float deltaTime, bool viaTickInput) noexcept
{
    if (!generation::OnGameThread())
    {
        return engine(state, deltaTime);
    }
    void* character = ReadPointer(state, facts::kMoveStateCharacter);
    if (!g_ownTick)
    {
        ++g_engineTicks.count;
        g_engineTicks.deltaTime = deltaTime;
        g_engineTicks.memberTicked |= character != nullptr && character == generation::Member();
    }
    Count(state, character, viaTickInput);

    const void* owner = ReadPointer(character, facts::kCharacterEffectiveController);
    if (!pad_sticks::Enabled() || !pad_sticks::PartActs(pad_sticks::kPartPlayerOneMove) || !generation::Active() ||
        owner == nullptr ||
        owner != generation::PlayerOneController())
    {
        return engine(state, deltaTime);
    }
    const pad_sticks::StickScope sticks(pad_sticks::ControllerIdOf(owner));
    sticks.Held() ? pad_sticks::MoveCounters().Act() : pad_sticks::MoveCounters().Refuse();
    return engine(state, deltaTime);
}

std::uintptr_t Tick(void* state, float deltaTime)
{
    return RunForOwner(g_engineTick, state, deltaTime, false);
}

std::uintptr_t TickInput(void* state, float deltaTime)
{
    return RunForOwner(g_engineTickInput, state, deltaTime, true);
}

template <class Fn>
bool Find(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto live = platform::FindFunction(image, fact);
    out             = live ? reinterpret_cast<Fn>(*live) : nullptr;
    return live.has_value();
}

const wchar_t* RoleOf(const void* character) noexcept
{
    return character == generation::Member() ? L"player 2's" : L"player 1's";
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    bool gate  = Find(image, facts::kIsPlayerCore, g_gate.isPlayerCore);
    gate       = Find(image, facts::kIsPrimaryCharacter, g_gate.isPrimaryOnes) && gate;
    gate       = Find(image, facts::kSpecialMoveCharacter, g_gate.specialActor) && gate;
    gate       = Find(image, facts::kMoveStateControlOwner, g_gate.controlOwner) && gate;
    gate       = Find(image, facts::kHasStateTag, g_gate.hasStateTag) && gate;
    g_gateFound = gate;
    if (!gate)
    {
        platform::Log(L"the movement census counts ticks only: a gate test's bytes differ");
    }

    const auto tick      = platform::FindFunction(image, facts::kMoveStateTick.target);
    const auto tickInput = platform::FindFunction(image, facts::kMoveStateTickInput.target);
    g_engineTick         = tick ? reinterpret_cast<TickFn>(*tick) : nullptr;
    g_engineTickInput    = tickInput ? reinterpret_cast<TickFn>(*tickInput) : nullptr;
    const bool first  = tick && platform::ReplaceSlot(image, facts::kMoveStateTick,
                                                      reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot);
    const bool second = tickInput && platform::ReplaceSlot(image, facts::kMoveStateTickInput,
                                                           reinterpret_cast<std::uintptr_t>(&TickInput),
                                                           g_tickInputSlot);
    return first || second;
}

void Remove() noexcept
{
    for (auto* slot : {&g_tickSlot, &g_tickInputSlot})
    {
        if (!slot->Restore())
        {
            platform::Log(L"could not restore a move-state slot; the replacement stays in place");
        }
    }
}

EngineTicks TakeEngineTicks() noexcept
{
    const EngineTicks taken = g_engineTicks;
    g_engineTicks           = {};
    return taken;
}

OwnTick::OwnTick() noexcept
{
    g_ownTick = true;
}

OwnTick::~OwnTick()
{
    g_ownTick = false;
}

void LogCensus() noexcept
{
    for (Entry& entry : g_entries)
    {
        void* character = entry.character.load(std::memory_order_relaxed);
        if (character == nullptr)
        {
            continue;
        }
        const std::uint32_t ticks      = entry.ticks.exchange(0, std::memory_order_relaxed);
        const std::uint32_t inputTicks = entry.inputTicks.exchange(0, std::memory_order_relaxed);
        const std::uint32_t ownTicks   = entry.ownTicks.exchange(0, std::memory_order_relaxed);
        if (ticks + inputTicks > 0 && OfInterest(character))
        {
            platform::Log(
                L"  move state of {} ({}): ticks {} + {} direct, {} of them by this mod; driven by {}, "
                L"player-controlled {}; gate tests over {} samples: leader type {}, primary's character {}, special {}, "
                L"tag17 {}, tag22 {}, blocked {}, cooling {}",
                NameOf(character), RoleOf(character), ticks, inputTicks, ownTicks,
                NameOf(ReadPointer(character, facts::kCharacterEffectiveController)),
                static_cast<unsigned>(ReadField<std::uint8_t>(character, facts::kCharacterPlayerControlled)),
                entry.samples, entry.playerCore, entry.primaryOnes, entry.special, entry.tag17, entry.tag22, entry.blocked,
                entry.cooling);
        }
        entry.samples = entry.playerCore = entry.primaryOnes = entry.special = 0;
        entry.tag17 = entry.tag22 = entry.blocked = entry.cooling = 0;
        entry.character.store(nullptr, std::memory_order_relaxed);  // characters come and go with the area
    }
}

}  // namespace lss::player_two::move_state
