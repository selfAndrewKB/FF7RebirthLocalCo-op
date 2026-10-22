#include "player_two/rumble_list_two.h"

#include "facts/engine.h"
#include "platform/build_value.h"
#include "platform/code_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/generation.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/rumble_list_code.h"
#include "player_two/rumble_list_shared.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>

// The rumble sources that run outside player 2's scopes (player_two/rumble_list_two.h): the scooter move state's
// slots, the sky glide's delegate, the jump box's OnEnded, the drift and brake actions. Each names player 2 as the
// owner of its adds and stops while it runs for player 2's scooter or chocobo (OwnerTwo); the drift and brake also
// answer the leader test for player 2's member on their thread. Their slots are placed from one table, a source's
// slots all or none; the jump box (an entry redirect, and 1.004's inline list load) is placed on its own.

namespace lss::player_two::rumble_list_two {

namespace {

// The rumble sources that run outside player 2's scopes (RE-p2-rumble-owner-chocobo.md §9; slots and code read from
// both exes, 2026-10-12). Each names player 2 as the owner of its adds while it runs for player 2's scooter or chocobo.
constexpr std::uintptr_t kScooterTickSlot = 0x1466B9D90;  // SBScooterMoveState slot 1: hit and loop rumble (actor tick)
constexpr std::uintptr_t kScooterTick     = 0x14344F38C;
constexpr std::uintptr_t kScooterOffSlot  = 0x1466B9DC8;  // ... slot 8, Deactivate: stops the loop
constexpr std::uintptr_t kScooterOff      = 0x14344A350;
constexpr std::uintptr_t kGlideSafeSlot   = 0x145CE5C48;  // the sky-glide rumble delegate's ExecuteIfSafe (+0x50, Broadcast)
constexpr std::uintptr_t kGlideSafe       = 0x142247D60;
constexpr std::uintptr_t kGlideRunSlot    = 0x145CE5C40;  // ... its Execute (+0x48)
constexpr std::uintptr_t kGlideRun        = 0x142247D30;
constexpr std::uint8_t   kJumpEndedBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00};  // mov eax,28h
// the jump box's OnEnded TFunction invoker (often the worker)
constexpr platform::FunctionFact kJumpEnded{L"the jump box's OnEnded invoker", 0x1422ADF00, kJumpEndedBytes};
constexpr std::uintptr_t kDriftSlot       = 0x146692AF0;  // the drift action's state 1 tick (state table; anim worker)
constexpr std::uintptr_t kDrift           = 0x14328EFA4;
constexpr std::uintptr_t kBrakeOneSlot    = 0x1470FCDE0;  // the brake move's phase 1 and 2 (phase table; anim worker)
constexpr std::uintptr_t kBrakeOne        = 0x14328B9FC;
constexpr std::uintptr_t kBrakeTwoSlot    = 0x1470FCDE8;
constexpr std::uintptr_t kBrakeTwo        = 0x14328A1A0;
constexpr std::uint32_t  kStateOwner      = 0x8;    // the scooter move state's scooter; the glide state's move owner
constexpr std::uint32_t  kOwnerWeak       = 0x8;    // ... whose weak at +8 is the chocobo
constexpr std::uint32_t  kGlideState      = 0x18;   // the glide delegate instance's captured glide state
constexpr std::uint32_t  kJumpWeak        = 0x8;    // the jump box payload's weak character (the chocobo)
constexpr std::uint32_t  kDriftChocobo    = 0x4B0;  // the drift action's chocobo, a weak

using ScooterTickFn = std::uint64_t (*)(void* state, float dt);  // dt in xmm1
using ScooterOffFn  = std::uint64_t (*)(void* state);
using GlideSafeFn   = bool (*)(void* instance);
using GlideRunFn    = void (*)(void* instance);
using JumpEndedFn   = void (*)(void* payload);
using DriftFn       = std::uint64_t (*)(void* action, double dt);  // dt in xmm1
using BrakeFn       = std::uint64_t (*)(void* move, std::uint64_t event, double xmm2, double xmm3);

platform::VTableEntry   g_scooterTickSlot;
platform::VTableEntry   g_scooterOffSlot;
platform::VTableEntry   g_glideSafeSlot;
platform::VTableEntry   g_glideRunSlot;
platform::EntryRedirect g_jumpEnded;
platform::CodeRedirect  g_jumpListLoad;  // 1.004 only
platform::VTableEntry   g_driftSlot;
platform::VTableEntry   g_brakeOneSlot;
platform::VTableEntry   g_brakeTwoSlot;

/// Names player 2 as the owner of this thread's adds and stops while it lives (nesting keeps the outer owner).
class OwnerTwo
{
public:
    OwnerTwo() noexcept : saved_(t_ownerTwo) { t_ownerTwo = true; }
    ~OwnerTwo() { t_ownerTwo = saved_; }
    OwnerTwo(const OwnerTwo&)            = delete;
    OwnerTwo& operator=(const OwnerTwo&) = delete;

private:
    bool saved_;
};

/// Player 2's chocobo, by the weak the game keeps for it (any thread).
bool ChocoboTwo(std::uint64_t weak) noexcept
{
    const std::uint64_t two = generation::ChocoboWeak();
    return two != 0 && weak == two && g_listTwo.load(std::memory_order_acquire) != nullptr;
}

/// Counts a source run as player 2's, and whether it ran off the game thread.
void NoteSource(std::atomic<unsigned>& count) noexcept
{
    count.fetch_add(1, std::memory_order_relaxed);
    if (!generation::OnGameThread())
    {
        g_counts.sourceWorker.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Drift and brake are expected on the anim worker: the per-thread leader answer is then the worker's alone. Counted
/// when one runs on the game thread instead (RE-p2-rumble-owner-chocobo.md §9.5, thread ASSUMPTION).
void NoteAction() noexcept
{
    if (generation::OnGameThread())
    {
        g_counts.actionsOnGame.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Player 2's scooter: its move state's scooter is possessed by player 2's controller (any thread).
bool ScooterTwo(const void* state) noexcept
{
    const void* const two     = generation::PlayerTwoController();
    const void* const scooter = two != nullptr ? ReadPointer(state, kStateOwner) : nullptr;
    return scooter != nullptr && ReadPointer(scooter, facts::kPawnController) == two &&
           g_listTwo.load(std::memory_order_acquire) != nullptr;
}

/// The scooter move state's slot 1 (every scooter's, from its actor tick, outside every scope of the mod): player 2's
/// scooter's hit and loop rumble, and the loop's stop, are player 2's.
std::uint64_t ScooterTick(void* state, float dt) noexcept
{
    const auto original = reinterpret_cast<ScooterTickFn>(g_scooterTickSlot.Original());
    if (!ScooterTwo(state))
    {
        return original(state, dt);
    }
    NoteSource(g_counts.scooterTwo);
    const OwnerTwo owner;
    return original(state, dt);
}

/// ... and its slot 8 (Deactivate), which stops the loop.
std::uint64_t ScooterOff(void* state) noexcept
{
    const auto original = reinterpret_cast<ScooterOffFn>(g_scooterOffSlot.Original());
    if (!ScooterTwo(state))
    {
        return original(state);
    }
    NoteSource(g_counts.scooterTwo);
    const OwnerTwo owner;
    return original(state);
}

/// The sky glide's rumble delegate (the move owner's "back to normal movement" broadcast): player 2's chocobo's is
/// player 2's. The chocobo is the weak at +8 of the glide state's move owner.
bool GlideTwo(void* instance) noexcept
{
    const void* const state = ReadPointer(instance, kGlideState);
    const void* const mover = state != nullptr ? ReadPointer(state, kStateOwner) : nullptr;
    return mover != nullptr && ChocoboTwo(ReadField<std::uint64_t>(mover, kOwnerWeak));
}

bool GlideSafe(void* instance) noexcept
{
    const auto original = reinterpret_cast<GlideSafeFn>(g_glideSafeSlot.Original());
    if (!GlideTwo(instance))
    {
        return original(instance);
    }
    NoteSource(g_counts.glideTwo);
    const OwnerTwo owner;
    return original(instance);
}

void GlideRun(void* instance) noexcept
{
    const auto original = reinterpret_cast<GlideRunFn>(g_glideRunSlot.Original());
    if (!GlideTwo(instance))
    {
        original(instance);
        return;
    }
    NoteSource(g_counts.glideTwo);
    const OwnerTwo owner;
    original(instance);
}

/// 1.004: names this thread for the jump box's inline list load while it lives (the code there takes player 2's list).
class JumpRoute
{
public:
    JumpRoute() noexcept
    {
        std::uint32_t none = 0;
        named_ = g_jumpListLoad.IsInstalled() &&
                 g_jumpThread.compare_exchange_strong(none, GetCurrentThreadId(), std::memory_order_relaxed);
    }
    ~JumpRoute()
    {
        if (named_)
        {
            g_jumpThread.store(0, std::memory_order_relaxed);
        }
    }
    JumpRoute(const JumpRoute&)            = delete;
    JumpRoute& operator=(const JumpRoute&) = delete;

private:
    bool named_ = false;
};

/// The jump box's OnEnded (the jump action's finish or abort): player 2's chocobo's landing rumble is player 2's.
void JumpEnded(void* payload) noexcept
{
    const auto original = reinterpret_cast<JumpEndedFn>(g_jumpEnded.Original());
    if (!ChocoboTwo(ReadField<std::uint64_t>(payload, kJumpWeak)))
    {
        original(payload);
        return;
    }
    NoteSource(g_counts.jumpTwo);
    const OwnerTwo  owner;
    const JumpRoute route;
    original(payload);
}

/// The drift action's state 1 tick (anim worker): player 2's chocobo's drift is player 2's, and the game's gate (is the
/// chocobo's rider the player?) answers yes for player 2's member on this thread.
std::uint64_t Drift(void* action, double dt) noexcept
{
    const auto original = reinterpret_cast<DriftFn>(g_driftSlot.Original());
    if (!ChocoboTwo(ReadField<std::uint64_t>(action, kDriftChocobo)))
    {
        return original(action, dt);
    }
    NoteSource(g_counts.driftTwo);
    NoteAction();
    const OwnerTwo                               owner;
    const member_movement::ThreadLeaderAnswer leader(g_memberType.load(std::memory_order_relaxed));
    return original(action, dt);
}

/// The brake move's phases 1 and 2 (anim worker), as the drift.
std::uint64_t Brake(platform::VTableEntry& slot, void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    const auto original = reinterpret_cast<BrakeFn>(slot.Original());
    if (!ChocoboTwo(ReadField<std::uint64_t>(move, kMoveCharacter)))
    {
        return original(move, event, xmm2, xmm3);
    }
    NoteSource(g_counts.brakeTwo);
    NoteAction();
    const OwnerTwo                               owner;
    const member_movement::ThreadLeaderAnswer leader(g_memberType.load(std::memory_order_relaxed));
    return original(move, event, xmm2, xmm3);
}

std::uint64_t BrakeOne(void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    return Brake(g_brakeOneSlot, move, event, xmm2, xmm3);
}

std::uint64_t BrakeTwo(void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    return Brake(g_brakeTwoSlot, move, event, xmm2, xmm3);
}

/// The 1.004 jump box's list load, then: on the thread JumpEnded named (g_jumpThread, against the TEB's thread id at
/// gs:[48h]), player 2's list when it exists (rumble_list_code.h). It runs under the game's lock, which DropList also
/// takes to clear g_listTwo, so the list it takes is live for the whole add. rax, rcx and the flags are dead at that
/// point (overwritten before any read: 0x1422AD988, 0x1422AD997, 0x1422AD99B). The code reads this DLL's globals,
/// which stay mapped: the DLL is pinned (platform/module_pin.h).
bool PlaceJumpListLoad(const platform::GameImage& image) noexcept
{
    using namespace rumble_list_code;
    if (platform::RunningBuild() != platform::BuildIndex::Research)
    {
        return false;
    }
    const auto site = image.Rebase(kJumpListLoad, sizeof(kJumpListLoadBytes));
    if (!site)
    {
        return false;
    }
    const auto code = JumpListLoadCode(&g_jumpThread, &g_listTwo);
    return g_jumpListLoad.Install(*site, kJumpListLoadBytes, code, *site + sizeof(kJumpListLoadBytes));
}

/// A slot of a source, checked against the function the research read. Logged by the caller.
bool Slot(const platform::GameImage& image, platform::VTableEntry& entry, std::uintptr_t slot, std::uintptr_t function,
          std::uintptr_t replacement) noexcept
{
    const auto at = image.Rebase(slot, sizeof(void*));
    const auto fn = image.Rebase(function);
    return at && fn && entry.Replace(*at, *fn, replacement);
}

/// A source's slot: the entry, the research slot and function, and the replacement.
struct SourceSlot
{
    platform::VTableEntry* entry;
    std::uintptr_t         slot;
    std::uintptr_t         function;
    std::uintptr_t         replacement;
};

/// A source's slots, all or none: the loop added in the scooter's slot 1 is stopped by its slot 8 at getoff;
/// Broadcast calls the glide delegate's one slot, a direct call the other; the brake's two phases belong together.
struct Source
{
    const wchar_t*              name;
    std::span<const SourceSlot> slots;
};

const SourceSlot kScooterSlots[] = {
    {&g_scooterTickSlot, kScooterTickSlot, kScooterTick, reinterpret_cast<std::uintptr_t>(&ScooterTick)},
    {&g_scooterOffSlot, kScooterOffSlot, kScooterOff, reinterpret_cast<std::uintptr_t>(&ScooterOff)},
};
const SourceSlot kGlideSlots[] = {
    {&g_glideSafeSlot, kGlideSafeSlot, kGlideSafe, reinterpret_cast<std::uintptr_t>(&GlideSafe)},
    {&g_glideRunSlot, kGlideRunSlot, kGlideRun, reinterpret_cast<std::uintptr_t>(&GlideRun)},
};
const SourceSlot kDriftSlots[] = {
    {&g_driftSlot, kDriftSlot, kDrift, reinterpret_cast<std::uintptr_t>(&Drift)},
};
const SourceSlot kBrakeSlots[] = {
    {&g_brakeOneSlot, kBrakeOneSlot, kBrakeOne, reinterpret_cast<std::uintptr_t>(&BrakeOne)},
    {&g_brakeTwoSlot, kBrakeTwoSlot, kBrakeTwo, reinterpret_cast<std::uintptr_t>(&BrakeTwo)},
};
// Placed and removed in this order (removal reversed); the install line names them around the jump box.
const Source kSources[] = {
    {L"scooter", kScooterSlots},
    {L"glide", kGlideSlots},
    {L"drift", kDriftSlots},
    {L"brake", kBrakeSlots},
};
constexpr std::size_t kScooterAt = 0, kGlideAt = 1, kDriftAt = 2, kBrakeAt = 3;
static_assert(std::size(kSources) == 4);

/// Places a source's slots, all or none (a slot placed before a later one fails is put back).
bool PlaceSource(const platform::GameImage& image, const Source& source) noexcept
{
    for (const SourceSlot& slot : source.slots)
    {
        if (!Slot(image, *slot.entry, slot.slot, slot.function, slot.replacement))
        {
            for (const SourceSlot& placed : source.slots)
            {
                if (placed.entry->Original() != 0)
                {
                    placed.entry->Restore();
                }
            }
            return false;
        }
    }
    return true;
}

}  // namespace

namespace sources {

std::wstring Install(const platform::GameImage& image) noexcept
{
    std::array<bool, std::size(kSources)> on{};
    for (std::size_t i = 0; i < std::size(kSources); ++i)
    {
        on[i] = PlaceSource(image, kSources[i]);
    }
    // 1.004's add is inline: its list load must follow the invoker, so the invoker waits for it there.
    const bool research = platform::RunningBuild() == platform::BuildIndex::Research;
    const bool load     = research && PlaceJumpListLoad(image);
    const auto jumpAt   = platform::FindFunction(image, kJumpEnded);
    const bool jump     = (load || !research) && jumpAt &&
                      g_jumpEnded.Install(*jumpAt, image.EntryInstruction(kJumpEnded.address, kJumpEnded.entryBytes),
                                          reinterpret_cast<std::uintptr_t>(&JumpEnded));
    if (!jump && g_jumpListLoad.IsInstalled())
    {
        g_jumpListLoad.Restore();
    }
    auto part = [](bool placed) { return placed ? L"on" : L"OFF (not what the research read)"; };
    try
    {
        return std::format(L"player 2's sources outside their scopes: scooter {}, glide {}, jump box {}{}, drift {}, "
                           L"brake {}",
                           part(on[kScooterAt]), part(on[kGlideAt]), part(jump),
                           research && jump ? L" (1.004: its inline add's list load)" : L"", part(on[kDriftAt]),
                           part(on[kBrakeAt]));
    }
    catch (...)
    {
        return {};  // formatting fails only out of memory: the install line then goes without this part
    }
}

void Remove() noexcept
{
    for (std::size_t i = std::size(kSources); i-- > 0;)
    {
        const std::span<const SourceSlot> slots = kSources[i].slots;
        for (std::size_t j = slots.size(); j-- > 0;)
        {
            if (slots[j].entry->Original() != 0)
            {
                slots[j].entry->Restore();
            }
        }
    }
    if (g_jumpEnded.IsInstalled())
    {
        g_jumpEnded.Restore();
    }
    if (g_jumpListLoad.IsInstalled())
    {
        g_jumpListLoad.Restore();
    }
}

}  // namespace sources

}  // namespace lss::player_two::rumble_list_two
