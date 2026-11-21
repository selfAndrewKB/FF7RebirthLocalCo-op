#include "battle/action_locks.h"

#include "facts/world.h"
#include "input/climb_context.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/leader_answer.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace lss::battle::action_locks {

namespace {

// Facts read from the database (research/RE-p2-menu-forbid-per-player.md; this build). The on/off table's counting
// calls, (game state, entry, reason, second counter): the lock (SetOn reaches it by a tail jump, so its return address
// is the lock site's) and the release (SetOff calls it). Each first instruction saves a register into the caller's
// home area.
constexpr std::uint8_t kLockBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                       0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x20, 0x00, 0x00};
constexpr platform::FunctionFact kLock{L"EndStateMgr_OnOff_IncAndQueueEdge", 0x1410D1590, kLockBytes};
constexpr std::uint8_t kReleaseBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x4C, 0x89,
                                          0x44, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRelease{L"EndStateMgr_OnOff_DecAndQueueEdge", 0x1410D1498, kReleaseBytes};
constexpr std::size_t            kFirstInstruction = 5;
constexpr std::uint8_t           kMenuLock         = 11;  // the on/off entry the characters' actions lock the menu with
// The applier's shut on the lock's first count: it sets the UI system's lock bit and tail-calls the menu-allowed setter
// on the UI system's controller, so the setter returns to the instruction after this call.
constexpr std::uint8_t kShutBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD2, 0x19, 0x93, 0x01, 0x48, 0x2B,
                                       0xE0, 0xE8, 0xD6, 0xB9, 0x06, 0x00, 0x45, 0x33, 0xC0, 0x33, 0xD2, 0x80};
constexpr platform::FunctionFact kShut{L"EndUI_MenuLockBit0_Set_DisallowMenu5E1", 0x14083A1A4, kShutBytes};
constexpr std::uintptr_t         kShutCall = 0x1410D1B2A;  // in the on/off table's applier, an E8 to kShut
// The reasons a character's own action locks with (its class's name, the research file's table of lock sites, each
// gated there on the player's character): any other reason is one of the game's own uses.
constexpr std::wstring_view kCharacterReasons[] = {L"FEndBehavior",
                                                   L"FEndCharacterControllerState",
                                                   L"FEndFieldActionExecuter",
                                                   L"FieldActionExecuter",
                                                   L"FEndHubTreasure",
                                                   L"UEndPlayerChocoboCoordinator",
                                                   L"UEndPlayerBuggyCoordinator",
                                                   L"FEndPartyChocoboManager"};
// A player's own lock held this long without its release is logged (no action of theirs keeps the menu shut that
// long; a release attributed elsewhere would).
constexpr std::chrono::seconds kLongHeld{10};

using CountFn = void (*)(void* state, std::uint8_t entry, std::uint64_t reason, std::uint8_t second);

// Set once at install.
platform::EntryRedirect g_lock, g_release;

// Any thread: both counting calls are ours (set once both are placed, cleared first at removal); while it is clear,
// a call left in place passes straight to the game's.
std::atomic<bool> g_active{false};

// The return address of the applier's shut (0: its call did not check, and no shut is told apart). Set once at install.
std::uintptr_t g_shutReturn = 0;

// Any thread, by seat (index 0 unused): the locks each seat's own actions hold, and since when (steady clock, in
// milliseconds) the first of them.
std::array<std::atomic<int>, players::kMaxPlayers>          g_held{};
std::array<std::atomic<std::int64_t>, players::kMaxPlayers> g_heldSince{};

// Any thread: the share of the game's count player 1's own actions hold (never above the game's count), and the game
// state the counts were last seen on (the table's owner).
std::atomic<int>         g_playerOne{0};
std::atomic<const void*> g_state{nullptr};

// Game thread, by seat: a long hold was logged (until the seat's locks are all released).
std::array<bool, players::kMaxPlayers> g_longSaid{};

std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The seat whose own context is open on this thread (its climb's, its thread's or its character tick's player), or 0.
int SeatActingHere() noexcept
{
    if (const int seat = input::climb_context::SeatHere(); players::SeatBuilt(seat))
    {
        return seat;
    }
    return input::climb_context::SeatOfType(scopes::leader_answer::ActingType());
}

/// The game's own count of the menu lock (its two counters), read outside its lock.
int GameCount(const void* state) noexcept
{
    return ReadField<std::int32_t>(state, facts::kGameStateEventCounterA) +
           ReadField<std::int32_t>(state, facts::kGameStateEventCounterB);
}

/// The one seat holding a lock (0: none). Two holding: the first, `both` set.
int OnlyHolder(bool& both) noexcept
{
    int found = 0;
    both      = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_held[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed) > 0)
        {
            both  = found != 0;
            found = found != 0 ? found : seat;
        }
    }
    return found;
}

/// Takes one from `count`; false if it held none.
bool TakeOne(std::atomic<int>& count) noexcept
{
    int now = count.load(std::memory_order_relaxed);
    while (now > 0 && !count.compare_exchange_weak(now, now - 1, std::memory_order_relaxed))
    {
    }
    return now > 0;
}

/// A lock outside every seat's context is player 1's own when its reason is a character action's (the game's own uses
/// pass their system's name, or none).
bool PlayerOneOwn(std::uint64_t reason) noexcept
{
    if (reason == 0)
    {
        return false;
    }
    const std::wstring text = players::unreal::NameText(reason);
    for (const std::wstring_view prefix : kCharacterReasons)
    {
        if (text.starts_with(prefix))
        {
            return true;
        }
    }
    return false;
}

/// Player 1's share brought down to the game's count: a release given to the game that was player 1's after all
/// leaves the share no higher than what the game holds, and it is empty whenever the game's count is.
void FitPlayerOne(const void* state) noexcept
{
    const int game = std::max(GameCount(state), 0);
    int       now  = g_playerOne.load(std::memory_order_relaxed);
    while (now > game && !g_playerOne.compare_exchange_weak(now, game, std::memory_order_relaxed))
    {
    }
}

/// A Debug line about one count change: `what` (whose and what), the reason's name, the caller, the thread and every
/// count, made only when the line is written.
void Note(const std::wstring& what, std::uint64_t reason, const void* from, const void* state) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        std::wstring held = std::format(L", player 1's share {}", g_playerOne.load(std::memory_order_relaxed));
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            held += std::format(L", {}'s {}", players::SeatName(seat),
                                g_held[static_cast<std::size_t>(seat)].load(std::memory_order_relaxed));
        }
        platform::LogDebug(L"action locks: {} ({}; from {:#x}, {}; the game's count {}{})", what,
                           reason != 0 ? players::unreal::NameText(reason) : std::wstring(L"no reason"),
                           reinterpret_cast<std::uintptr_t>(from),
                           players::generation::OnGameThread() ? L"the game thread" : L"a worker thread",
                           GameCount(state), held);
    }
    catch (...)
    {
    }
}

/// `seat`'s name and `rest`, for a Debug line only (empty while Debug is off).
std::wstring Whose(int seat, const wchar_t* rest) noexcept
{
    try
    {
        return platform::Enabled(platform::Level::Debug) ? std::format(L"{}'s {}", players::SeatName(seat), rest)
                                                         : std::wstring();
    }
    catch (...)
    {
        return {};
    }
}

/// Replaces the lock: a seat's own lock is counted for the seat; every other one is the game's, player 1's own counted
/// as their share of it.
void Lock(void* state, std::uint8_t entry, std::uint64_t reason, std::uint8_t second) noexcept
{
    const auto original = reinterpret_cast<CountFn>(g_lock.Original());
    const bool ours     = entry == kMenuLock && g_active.load(std::memory_order_acquire);
    const int  seat     = ours ? SeatActingHere() : 0;
    if (seat == 0)
    {
        original(state, entry, reason, second);
        if (ours)
        {
            g_state.store(state, std::memory_order_relaxed);
            const bool own = PlayerOneOwn(reason);
            if (own)
            {
                g_playerOne.fetch_add(1, std::memory_order_relaxed);
            }
            FitPlayerOne(state);
            Note(own ? L"player 1's own action locks their command menu" : L"the game's command menu lock", reason,
                 _ReturnAddress(), state);
        }
        return;
    }
    const auto index = static_cast<std::size_t>(seat);
    if (g_held[index].fetch_add(1, std::memory_order_relaxed) == 0)
    {
        g_heldSince[index].store(NowMs(), std::memory_order_relaxed);
    }
    Note(Whose(seat, L"own action locks their command menu"), reason, _ReturnAddress(), state);
}

/// Replaces the release. A seat's own release takes the seat's lock; one outside every seat's context, with the game's
/// count empty, takes the lock of the one seat holding any (its action ended where no context names it); the game gets
/// the rest, but never a release its count does not hold (that would leave its count below zero, and its next lock
/// would never shut a menu). A release the game gets is player 1's own when only player 1's share holds the game's
/// count, or when it ends in player 1's climb context. A release names no reason (its argument is not one). The game's
/// count is read outside its lock: a lock it takes meanwhile can be missed.
void Release(void* state, std::uint8_t entry, std::uint64_t, std::uint8_t second) noexcept
{
    const auto original = reinterpret_cast<CountFn>(g_release.Original());
    if (entry != kMenuLock || !g_active.load(std::memory_order_acquire))
    {
        return original(state, entry, 0, second);
    }
    const int  game      = GameCount(state);
    const bool gameHolds = game > 0;
    int        seat      = SeatActingHere();
    bool       both      = false;
    if (seat == 0 && !gameHolds)
    {
        seat = OnlyHolder(both);
        if (both)
        {
            platform::Log(L"action locks: a command menu release with no player's context while players 2 and 3 both "
                          L"hold their own: taken as player 2's");
        }
    }
    if (seat != 0 && TakeOne(g_held[static_cast<std::size_t>(seat)]))
    {
        Note(Whose(seat, L"own action releases their command menu"), 0, _ReturnAddress(), state);
        return;
    }
    if (!gameHolds)
    {
        platform::Log(L"action locks: a command menu release no one holds ({}); dropped",
                      seat != 0 ? players::SeatName(seat) : L"no player's context");
        return;
    }
    // Decided before the game's release, taken after it: the game's count never stands above the share meanwhile.
    const bool mine = game <= g_playerOne.load(std::memory_order_relaxed) || input::climb_context::PlayerHere() == 1;
    original(state, entry, 0, second);
    const bool own = mine && TakeOne(g_playerOne);
    g_state.store(state, std::memory_order_relaxed);
    FitPlayerOne(state);
    Note(own ? L"player 1's own action releases their command menu" : L"the game's command menu release", 0,
         _ReturnAddress(), state);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto lock    = platform::FindFunction(image, kLock);
    const auto release = platform::FindFunction(image, kRelease);
    // The lock first: a release taken without its lock would take the game's releases for locks it never counted.
    const bool ok =
        lock && release &&
        g_lock.Install(*lock, std::span(kLockBytes).first(kFirstInstruction), reinterpret_cast<std::uintptr_t>(&Lock)) &&
        g_release.Install(*release, std::span(kReleaseBytes).first(kFirstInstruction),
                          reinterpret_cast<std::uintptr_t>(&Release));
    if (!ok && g_lock.IsInstalled() && !g_lock.Restore())
    {
        platform::Log(L"action locks: the lock was changed after us; left in place (it passes through)");
    }
    // The applier's shut: its call must be the E8 to the shut the research read, so the setter's return address names it.
    const auto shut = platform::FindFunction(image, kShut);
    const auto call = image.Rebase(kShutCall, 5);
    if (ok && shut && call && ReadAt<std::uint8_t>(*call) == 0xE8 &&
        *call + 5 + static_cast<std::intptr_t>(ReadAt<std::int32_t>(*call + 1)) == *shut)
    {
        g_shutReturn = *call + 5;
    }
    g_active.store(ok, std::memory_order_release);
    platform::Log(L"action locks: {}; player 1's own {}",
                  ok ? L"players 2 and 3's own actions lock their own command menu"
                     : L"UNAVAILABLE (a check failed): every action locks every player's menu",
                  g_shutReturn != 0 ? L"lock their own menu only"
                                    : L"still lock every player's (the applier's shut did not check)");
    return ok;
}

void Remove() noexcept
{
    g_active.store(false, std::memory_order_release);
    for (platform::EntryRedirect* redirect : {&g_lock, &g_release})
    {
        if (redirect->IsInstalled() && !redirect->Restore())
        {
            platform::Log(L"action locks: a counting call was changed after us; left in place (it passes through)");
        }
    }
    for (auto& held : g_held)
    {
        held.store(0, std::memory_order_relaxed);
    }
    g_playerOne.store(0, std::memory_order_relaxed);
    g_state.store(nullptr, std::memory_order_relaxed);
    g_shutReturn = 0;
    g_longSaid   = {};
}

bool FromShut(const void* returnAddress) noexcept
{
    return g_shutReturn != 0 && reinterpret_cast<std::uintptr_t>(returnAddress) == g_shutReturn;
}

bool OnlyPlayerOneHolds() noexcept
{
    const void* state = g_state.load(std::memory_order_relaxed);
    if (state == nullptr || g_shutReturn == 0 || !g_active.load(std::memory_order_acquire))
    {
        return false;
    }
    const int game = GameCount(state);
    return game > 0 && game <= g_playerOne.load(std::memory_order_relaxed);
}

int Held(int seat) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return 0;
    }
    const auto index = static_cast<std::size_t>(seat);
    const int  held  = g_held[index].load(std::memory_order_relaxed);
    if (!players::generation::OnGameThread())
    {
        return held;
    }
    if (held == 0)
    {
        g_longSaid[index] = false;
        return 0;
    }
    if (!g_longSaid[index] &&
        NowMs() - g_heldSince[index].load(std::memory_order_relaxed) >
            std::chrono::duration_cast<std::chrono::milliseconds>(kLongHeld).count())
    {
        g_longSaid[index] = true;
        platform::Log(L"action locks: {}'s own command menu lock has been held {} s without its release ({} lock(s)): "
                      L"their menu stays shut until it comes",
                      players::SeatName(seat), kLongHeld.count(), held);
    }
    return held;
}

void SeatOut(int seat) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return;
    }
    const auto index = static_cast<std::size_t>(seat);
    g_longSaid[index] = false;
    if (const int held = g_held[index].exchange(0, std::memory_order_relaxed); held != 0)
    {
        platform::Log(L"action locks: {} left holding {} lock(s) of their own command menu; forgotten",
                      players::SeatName(seat), held);
    }
}

}  // namespace lss::battle::action_locks
