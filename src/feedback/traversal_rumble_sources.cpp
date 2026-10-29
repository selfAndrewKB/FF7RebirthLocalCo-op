#include "feedback/traversal_rumble.h"

#include "facts/engine.h"
#include "feedback/traversal_rumble_code.h"
#include "feedback/traversal_rumble_shared.h"
#include "platform/build_value.h"
#include "platform/code_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_answer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>

// The rumble sources that run outside the seats' scopes (feedback/traversal_rumble.h): the scooter move
// state's slots, the sky glide's delegate, the jump box's OnEnded, the drift and brake actions. Each names a seat as
// the owner of its adds and stops while it runs for the seat's scooter or chocobo (OwnerTwo; the seat from the
// scooter's controller or the chocobo's weak pointer); the drift and brake also answer the leader test for the seat's
// member on their thread. Their slots are placed from one table, a source's slots all or none; the jump box (an entry
// redirect, and 1.004's inline list load) is placed on its own.

namespace lss::feedback::traversal_rumble {

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

/// Names `seat` as the owner of this thread's adds and stops while it lives (nesting keeps the outer owner).
class OwnerTwo
{
public:
    explicit OwnerTwo(int seat) noexcept : saved_(t_ownerSeat) { t_ownerSeat = seat; }
    ~OwnerTwo() { t_ownerSeat = saved_; }
    OwnerTwo(const OwnerTwo&)            = delete;
    OwnerTwo& operator=(const OwnerTwo&) = delete;

private:
    int saved_;
};

/// The built seat with a list whose chocobo the game's weak pointer `weak` names (this tick's), or 0. Any thread.
int SeatOfChocoboWeak(std::uint64_t weak) noexcept
{
    for (int seat = 1; weak != 0 && seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::ChocoboWeakOf(seat) == weak && g_listTwo[seat].load(std::memory_order_acquire) != nullptr)
        {
            return seat;
        }
    }
    return 0;
}

/// Counts a source run as `seat`'s, and whether it ran off the game thread.
void NoteSource(int seat, std::atomic<unsigned> Counts::*count) noexcept
{
    (g_counts[seat].*count).fetch_add(1, std::memory_order_relaxed);
    if (!players::generation::OnGameThread())
    {
        g_counts[seat].sourceWorker.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Drift and brake are expected on the anim worker: the per-thread leader answer is then the worker's alone. Counted
/// when one runs on the game thread instead (RE-p2-rumble-owner-chocobo.md §9.5, thread ASSUMPTION).
void NoteAction(int seat) noexcept
{
    if (players::generation::OnGameThread())
    {
        g_counts[seat].actionsOnGame.fetch_add(1, std::memory_order_relaxed);
    }
}

/// The built seat with a list whose controller possesses the move state's scooter, or 0 (any thread).
int SeatOfScooter(const void* state) noexcept
{
    const void* scooter = nullptr;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* const controller = players::generation::ControllerOf(seat);
        if (controller == nullptr)
        {
            continue;
        }
        scooter = scooter != nullptr ? scooter : ReadPointer(state, kStateOwner);
        if (scooter != nullptr && ReadPointer(scooter, facts::kPawnController) == controller &&
            g_listTwo[seat].load(std::memory_order_acquire) != nullptr)
        {
            return seat;
        }
    }
    return 0;
}

/// The scooter move state's slot 1 (every scooter's, from its actor tick, outside every scope of the mod): a seat's
/// scooter's hit and loop rumble, and the loop's stop, are the seat's.
std::uint64_t ScooterTick(void* state, float dt) noexcept
{
    const auto original = reinterpret_cast<ScooterTickFn>(g_scooterTickSlot.Original());
    const int  seat     = SeatOfScooter(state);
    if (seat == 0)
    {
        return original(state, dt);
    }
    NoteSource(seat, &Counts::scooterTwo);
    const OwnerTwo owner(seat);
    return original(state, dt);
}

/// ... and its slot 8 (Deactivate), which stops the loop.
std::uint64_t ScooterOff(void* state) noexcept
{
    const auto original = reinterpret_cast<ScooterOffFn>(g_scooterOffSlot.Original());
    const int  seat     = SeatOfScooter(state);
    if (seat == 0)
    {
        return original(state);
    }
    NoteSource(seat, &Counts::scooterTwo);
    const OwnerTwo owner(seat);
    return original(state);
}

/// The sky glide's rumble delegate (the move owner's "back to normal movement" broadcast): a seat's chocobo's is the
/// seat's (0: no seat's). The chocobo is the weak at +8 of the glide state's move owner.
int GlideSeat(void* instance) noexcept
{
    const void* const state = ReadPointer(instance, kGlideState);
    const void* const mover = state != nullptr ? ReadPointer(state, kStateOwner) : nullptr;
    return mover != nullptr ? SeatOfChocoboWeak(ReadField<std::uint64_t>(mover, kOwnerWeak)) : 0;
}

bool GlideSafe(void* instance) noexcept
{
    const auto original = reinterpret_cast<GlideSafeFn>(g_glideSafeSlot.Original());
    const int  seat     = GlideSeat(instance);
    if (seat == 0)
    {
        return original(instance);
    }
    NoteSource(seat, &Counts::glideTwo);
    const OwnerTwo owner(seat);
    return original(instance);
}

void GlideRun(void* instance) noexcept
{
    const auto original = reinterpret_cast<GlideRunFn>(g_glideRunSlot.Original());
    const int  seat     = GlideSeat(instance);
    if (seat == 0)
    {
        original(instance);
        return;
    }
    NoteSource(seat, &Counts::glideTwo);
    const OwnerTwo owner(seat);
    original(instance);
}

/// 1.004: names this thread and `seat`'s list in a free route slot for the jump box's inline list load while it lives
/// (the code there takes the list of the slot naming its thread). Claimed under the game's lock, so the list it names
/// is live (a list's drop clears it from every slot under the same lock); its list is cleared before its thread is
/// released. One slot per further player, so two landings on two animation workers at once each have theirs
/// (RE-seats-step6-camera-rumble.md §2). A landing inside another on the same thread takes over that thread's slot for
/// its length and gives the outer one's list back after (the code reads the first slot naming the thread). With no
/// slot free (not expected) the landing's rumble goes to the game's list, logged.
class JumpRoute
{
public:
    explicit JumpRoute(int seat) noexcept
    {
        if (!g_jumpListLoad.IsInstalled())
        {
            return;
        }
        bool refused = false;
        {
            const ListLock      lock;
            void* const         list   = g_listTwo[seat].load(std::memory_order_acquire);
            const std::uint32_t thread = GetCurrentThreadId();
            for (auto& slot : g_jumpSlots)
            {
                if (list != nullptr && slot.thread.load(std::memory_order_acquire) == thread)
                {
                    outer_ = slot.list.exchange(list, std::memory_order_acq_rel);  // nested on this thread
                    slot_  = &slot;
                    nested_ = true;
                    break;
                }
            }
            for (auto& slot : g_jumpSlots)
            {
                std::uint32_t none = 0;
                if (slot_ == nullptr && list != nullptr &&
                    slot.thread.compare_exchange_strong(none, thread, std::memory_order_acq_rel))
                {
                    slot.list.store(list, std::memory_order_release);
                    slot_ = &slot;
                }
            }
            refused = list != nullptr && slot_ == nullptr;
        }
        if (refused)
        {
            // Every slot names another landing in flight: this one goes to the game's list. Info the first time, Debug
            // after.
            static std::atomic<unsigned> s_refusals{0};
            const auto level = s_refusals.fetch_add(1, std::memory_order_relaxed) == 0 ? platform::Level::Info
                                                                                      : platform::Level::Debug;
            platform::LogAt(level, L"rumble list: {}'s jump box landing found every 1.004 route held by other threads; "
                                   L"its rumble goes to the game's list",
                            players::SeatName(seat));
        }
    }
    ~JumpRoute()
    {
        if (slot_ == nullptr)
        {
            return;
        }
        if (nested_)
        {
            // The outer landing's list back, under the lock, only while it is still a seat's (a drop meanwhile left it
            // cleared, and it stays so).
            const ListLock lock;
            bool           live = false;
            for (int seat = 1; outer_ != nullptr && seat <= players::kSeatsBuilt; ++seat)
            {
                live = live || g_listTwo[seat].load(std::memory_order_acquire) == outer_;
            }
            slot_->list.store(live ? outer_ : nullptr, std::memory_order_release);
            return;
        }
        slot_->list.store(nullptr, std::memory_order_release);
        slot_->thread.store(0, std::memory_order_release);
    }
    JumpRoute(const JumpRoute&)            = delete;
    JumpRoute& operator=(const JumpRoute&) = delete;

private:
    traversal_rumble_code::JumpSlot* slot_   = nullptr;
    void*                       outer_  = nullptr;  // nested: the outer landing's list in the slot
    bool                        nested_ = false;
};
static_assert(traversal_rumble_code::kJumpSlots == players::kMaxPlayers - 1, "one route slot per further player");

/// The jump box's OnEnded (the jump action's finish or abort): a seat's chocobo's landing rumble is the seat's.
void JumpEnded(void* payload) noexcept
{
    const auto original = reinterpret_cast<JumpEndedFn>(g_jumpEnded.Original());
    const int  seat     = SeatOfChocoboWeak(ReadField<std::uint64_t>(payload, kJumpWeak));
    if (seat == 0)
    {
        original(payload);
        return;
    }
    NoteSource(seat, &Counts::jumpTwo);
    const OwnerTwo  owner(seat);
    const JumpRoute route(seat);
    original(payload);
}

/// The drift action's state 1 tick (anim worker): a seat's chocobo's drift is the seat's, and the game's gate (is the
/// chocobo's rider the player?) answers yes for the seat's member on this thread.
std::uint64_t Drift(void* action, double dt) noexcept
{
    const auto original = reinterpret_cast<DriftFn>(g_driftSlot.Original());
    const int  seat     = SeatOfChocoboWeak(ReadField<std::uint64_t>(action, kDriftChocobo));
    if (seat == 0)
    {
        return original(action, dt);
    }
    NoteSource(seat, &Counts::driftTwo);
    NoteAction(seat);
    const OwnerTwo                   owner(seat);
    const scopes::leader_answer::ThreadScope leader(g_memberType[seat].load(std::memory_order_relaxed));
    return original(action, dt);
}

/// The brake move's phases 1 and 2 (anim worker), as the drift.
std::uint64_t Brake(platform::VTableEntry& slot, void* move, std::uint64_t event, double xmm2, double xmm3) noexcept
{
    const auto original = reinterpret_cast<BrakeFn>(slot.Original());
    const int  seat     = SeatOfChocoboWeak(ReadField<std::uint64_t>(move, kMoveCharacter));
    if (seat == 0)
    {
        return original(move, event, xmm2, xmm3);
    }
    NoteSource(seat, &Counts::brakeTwo);
    NoteAction(seat);
    const OwnerTwo                   owner(seat);
    const scopes::leader_answer::ThreadScope leader(g_memberType[seat].load(std::memory_order_relaxed));
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

/// The 1.004 jump box's list load, then: on a thread a JumpEnded named (a slot of g_jumpSlots, against the TEB's thread
/// id at gs:[48h]), the list it named for that thread's seat when it is set (traversal_rumble_code.h). It runs under
/// the game's lock, which DropList also takes to clear the route's list, so the list it takes is live for the whole
/// add. rax, rcx and the flags are dead at that
/// point (overwritten before any read: 0x1422AD988, 0x1422AD997, 0x1422AD99B). The code reads this DLL's globals,
/// which stay mapped: the DLL is pinned (platform/module_pin.h).
bool PlaceJumpListLoad(const platform::GameImage& image) noexcept
{
    using namespace traversal_rumble_code;
    if (platform::RunningBuild() != platform::BuildIndex::Research)
    {
        return false;
    }
    const auto site = image.Rebase(kJumpListLoad, sizeof(kJumpListLoadBytes));
    if (!site)
    {
        return false;
    }
    const auto code = JumpListLoadCode(&g_jumpSlots);
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

}  // namespace lss::feedback::traversal_rumble
