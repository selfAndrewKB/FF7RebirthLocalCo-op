#include "input/move_census.h"

#include "facts/controller.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "platform/every.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace lss::input::move_census {

namespace {

using CharacterTestFn = std::uint8_t (*)(const void* character);
using ActorFn         = void* (*)();
using ControlOwnerFn  = void* (*)(const void* state);
using HasStateTagFn   = std::uint8_t (*)(const void* controlOwner, std::uint8_t tag);

struct Gate
{
    CharacterTestFn isPlayerCore  = nullptr;  // IsUCPC_Core
    CharacterTestFn isPrimaryOnes = nullptr;  // the primary controller's character
    ActorFn         specialActor  = nullptr;  // the third alternative's character
    ControlOwnerFn  controlOwner  = nullptr;  // the object the state tags are read from
    HasStateTagFn   hasStateTag   = nullptr;
};

// Set once at install.
Gate g_gate;
bool g_gateFound = false;

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
std::array<Entry, players::kMaxPlayers + 2> g_entries;  // the characters of interest (OfInterest), and one to spare

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

/// The seats' members and player 1's pawn and leader: the only characters counted.
bool OfInterest(const void* character) noexcept
{
    void* one = players::generation::PlayerOneController();
    return character != nullptr &&
           (players::SeatIndex(players::generation::SeatOfMember(character)) ||
            character == ReadPointer(one, facts::kControllerPawn) || character == ReadPointer(one, facts::kControllerLeader));
}

/// Evaluates the per-character and per-state gate tests one by one.
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

const wchar_t* RoleOf(const void* character) noexcept
{
    const int seat = players::generation::SeatOfMember(character);
    return players::SeatIndex(seat) ? players::BySeat(seat, L"player 2's", L"player 3's") : L"player 1's";
}

template <class Fn>
bool Find(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto live = platform::FindFunction(image, fact);
    out             = live ? reinterpret_cast<Fn>(*live) : nullptr;
    return live.has_value();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    bool gate   = Find(image, facts::kIsPlayerCore, g_gate.isPlayerCore);
    gate        = Find(image, facts::kIsActiveRiddenVehicle, g_gate.isPrimaryOnes) && gate;
    gate        = Find(image, facts::kSpecialMoveCharacter, g_gate.specialActor) && gate;
    gate        = Find(image, facts::kMoveStateControlOwner, g_gate.controlOwner) && gate;
    gate        = Find(image, facts::kHasStateTag, g_gate.hasStateTag) && gate;
    g_gateFound = gate;
    if (!gate)
    {
        platform::Log(L"the movement census counts ticks only: a gate test's bytes differ");
    }
    return gate;
}

void Count(void* state, void* character, bool viaTickInput, bool ownTick) noexcept
{
    Entry* entry = OfInterest(character) ? EntryFor(character) : nullptr;
    if (entry == nullptr)
    {
        return;
    }
    if (ownTick)
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

void Log() noexcept
{
    for (Entry& entry : g_entries)
    {
        void* character = entry.character.load(std::memory_order_relaxed);
        if (character == nullptr)
        {
            continue;
        }
        const std::uint32_t ticks      = platform::Take(entry.ticks);
        const std::uint32_t inputTicks = platform::Take(entry.inputTicks);
        const std::uint32_t ownTicks   = platform::Take(entry.ownTicks);
        if (ticks + inputTicks > 0 && OfInterest(character))
        {
            platform::Log(
                L"  move state of {} ({}): ticks {} + {} direct, {} of them by this mod; driven by {}, "
                L"player-controlled {}; gate tests over {} samples: leader type {}, primary's character {}, special {}, "
                L"tag17 {}, tag22 {}, blocked {}, cooling {}",
                players::NameOf(character), RoleOf(character), ticks, inputTicks, ownTicks,
                players::NameOf(ReadPointer(character, facts::kCharacterEffectiveController)),
                static_cast<unsigned>(ReadField<std::uint8_t>(character, facts::kCharacterPlayerControlled)),
                entry.samples, entry.playerCore, entry.primaryOnes, entry.special, entry.tag17, entry.tag22, entry.blocked,
                entry.cooling);
        }
        entry.samples = entry.playerCore = entry.primaryOnes = entry.special = 0;
        entry.tag17 = entry.tag22 = entry.blocked = entry.cooling = 0;
        entry.character.store(nullptr, std::memory_order_relaxed);  // characters come and go with the area
    }
}

}  // namespace lss::input::move_census
