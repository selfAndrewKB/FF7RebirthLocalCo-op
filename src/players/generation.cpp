#include "players/generation.h"

#include "facts/controller.h"
#include "players/actor_fields.h"
#include "players/object_fields.h"
#include "players/weak_object.h"

#include <atomic>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::players::generation {

namespace {

std::atomic<DWORD> g_gameThread{0};

/// One seat's generation. Index 0's controller is player 1's; its other fields stay unused.
struct SeatRecord
{
    std::atomic<bool>           active{false};
    std::atomic<void*>          controller{nullptr};
    std::atomic<void*>          member{nullptr};
    std::atomic<void*>          rideMember{nullptr};  // SetRideMember
    std::atomic<const wchar_t*> fault{nullptr};

    // The tick's identities (BeginTick), for the wrappers on any thread.
    std::atomic<std::uint64_t> memberWeak{0};
    std::atomic<const void*>   chocobo{nullptr};
    std::atomic<std::uint64_t> chocoboWeak{0};

    void ClearIdentities() noexcept
    {
        memberWeak.store(0);
        chocobo.store(nullptr);
        chocoboWeak.store(0);
    }
};

SeatRecord                 g_seats[kMaxPlayers];
std::atomic<std::uint64_t> g_playerOnePawnWeak{0};

/// Player 1's pawn is published while any seat is active (it is read only to tell a seat's member from it).
void ClearPlayerOneUnlessActive() noexcept
{
    if (!AnyActive())
    {
        g_playerOnePawnWeak.store(0);
    }
}

void WithdrawSeat(SeatRecord& seat) noexcept
{
    seat.active.store(false);
    seat.ClearIdentities();
}

}  // namespace

void Begin(int seat, void* playerOneController, void* controller) noexcept
{
    if (!SeatIndex(seat))
    {
        return;
    }
    SeatRecord& state = g_seats[seat];
    state.active.store(false);  // wrappers see no half-written generation
    g_seats[0].controller.store(playerOneController);
    state.controller.store(controller);
    state.member.store(nullptr);
    state.rideMember.store(nullptr);
    state.fault.store(nullptr);
    state.ClearIdentities();
    state.active.store(playerOneController != nullptr && controller != nullptr);
}

void SetMember(int seat, void* member) noexcept
{
    if (!SeatIndex(seat))
    {
        return;
    }
    SeatRecord& state = g_seats[seat];
    state.member.store(member);
    state.memberWeak.store(member != nullptr && state.active.load() ? weak_object::WeakOf(member) : 0);
}

void SetRideMember(int seat, void* member) noexcept
{
    if (SeatIndex(seat))
    {
        g_seats[seat].rideMember.store(member);
    }
}

void* RideMemberOf(int seat) noexcept
{
    return SeatIndex(seat) ? g_seats[seat].rideMember.load() : nullptr;
}

void* CameraMemberOf(int seat) noexcept
{
    void* const member = MemberOf(seat);
    return member != nullptr ? member : RideMemberOf(seat);
}

void Withdraw(int seat) noexcept
{
    if (SeatIndex(seat))
    {
        WithdrawSeat(g_seats[seat]);
        ClearPlayerOneUnlessActive();
    }
}

void End(int seat) noexcept
{
    if (!SeatIndex(seat))
    {
        return;
    }
    WithdrawSeat(g_seats[seat]);
    g_seats[seat].controller.store(nullptr);
    g_seats[seat].member.store(nullptr);
    g_seats[seat].rideMember.store(nullptr);
    ClearPlayerOneUnlessActive();
}

void Fault(int seat, const wchar_t* reason) noexcept
{
    if (!SeatIndex(seat))
    {
        return;
    }
    WithdrawSeat(g_seats[seat]);
    const wchar_t* none = nullptr;
    g_seats[seat].fault.compare_exchange_strong(none, reason);
    ClearPlayerOneUnlessActive();
}

void Fault(const wchar_t* reason) noexcept
{
    for (int seat = 1; seat < kMaxPlayers; ++seat)
    {
        if (g_seats[seat].controller.load() != nullptr)
        {
            Fault(seat, reason);
        }
    }
}

const wchar_t* FaultReason(int seat) noexcept
{
    return SeatIndex(seat) ? g_seats[seat].fault.load() : nullptr;
}

void SetGameThread() noexcept
{
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

bool OnGameThread() noexcept
{
    return GetCurrentThreadId() == g_gameThread.load(std::memory_order_relaxed);
}

bool GameThreadKnown() noexcept
{
    return g_gameThread.load(std::memory_order_relaxed) != 0;
}

bool Active(int seat) noexcept
{
    return SeatIndex(seat) && g_seats[seat].active.load();
}

bool AnyActive() noexcept
{
    for (int seat = 1; seat < kMaxPlayers; ++seat)
    {
        if (g_seats[seat].active.load())
        {
            return true;
        }
    }
    return false;
}

void* ControllerOf(int seat) noexcept
{
    return seat >= 0 && seat < kMaxPlayers ? g_seats[seat].controller.load() : nullptr;
}

void* MemberOf(int seat) noexcept
{
    return SeatIndex(seat) ? g_seats[seat].member.load() : nullptr;
}

int SeatOfController(const void* controller) noexcept
{
    if (controller == nullptr)
    {
        return -1;
    }
    for (int seat = 1; seat < kMaxPlayers; ++seat)
    {
        if (g_seats[seat].controller.load() == controller)
        {
            return seat;
        }
    }
    // Player 1's is kept after every seat left (End), and may be freed by a world change: named only while a seat is
    // active, when it is this world's.
    return AnyActive() && g_seats[0].controller.load() == controller ? 0 : -1;
}

int SeatOfMember(const void* member) noexcept
{
    if (member == nullptr)
    {
        return -1;
    }
    for (int seat = 1; seat < kMaxPlayers; ++seat)
    {
        if (g_seats[seat].member.load() == member)
        {
            return seat;
        }
    }
    return -1;
}

void BeginTick(const void* const (&drivenChocobo)[kMaxPlayers]) noexcept
{
    for (int seat = 1; seat < kMaxPlayers; ++seat)
    {
        SeatRecord& state = g_seats[seat];
        if (!state.active.load())
        {
            state.ClearIdentities();
            continue;
        }
        state.memberWeak.store(weak_object::WeakOf(state.member.load()));
        state.chocobo.store(drivenChocobo[seat]);
        state.chocoboWeak.store(weak_object::WeakOf(const_cast<void*>(drivenChocobo[seat])));
    }
    g_playerOnePawnWeak.store(
        AnyActive() ? weak_object::WeakOf(players::actor_fields::Pawn(g_seats[0].controller.load())) : 0);
}

std::uint64_t MemberWeakOf(int seat) noexcept
{
    return SeatIndex(seat) ? g_seats[seat].memberWeak.load() : 0;
}

int PlayerOfCharacter(const void* character) noexcept
{
    if (character == nullptr)
    {
        return -1;
    }
    const int seat = SeatOfMember(character);
    if (seat >= 1 && Active(seat))
    {
        return seat;
    }
    return character == actor_fields::Pawn(PlayerOneController()) ? 0 : -1;
}

int SeatOfMemberWeak(std::uint64_t weak) noexcept
{
    if (weak == 0)
    {
        return -1;
    }
    for (int seat = 1; seat < kMaxPlayers; ++seat)
    {
        if (g_seats[seat].memberWeak.load() == weak)
        {
            return seat;
        }
    }
    return -1;
}

std::uint64_t PlayerOnePawnWeak() noexcept
{
    return g_playerOnePawnWeak.load();
}

const void* ChocoboOf(int seat) noexcept
{
    return SeatIndex(seat) ? g_seats[seat].chocobo.load() : nullptr;
}

std::uint64_t ChocoboWeakOf(int seat) noexcept
{
    return SeatIndex(seat) ? g_seats[seat].chocoboWeak.load() : 0;
}

}  // namespace lss::players::generation
