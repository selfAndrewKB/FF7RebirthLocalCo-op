#include "player_two/input/move_type_two.h"

#include "platform/dev_switch.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/scopes/primary_controller.h"

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

namespace lss::player_two::move_type_two {

namespace {

// 1.004 addresses; 1.005 through replaced-walk-layer2.json (every slot and function read from both exes).

// PrimaryPC_SetByte890(layer cl, value dl): the primary controller's +0x890[layer] (a leaf). Its entry instruction sits
// inside one block in 1.005 only (1.004 %16 = 0xC): the write log (dev tools) is 1.005's.
constexpr std::uint8_t kSetterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kSetter{L"PrimaryPC_SetByte890", 0x1408A801C, kSetterBytes};
using SetterFn = void (*)(std::uint8_t layer, std::uint8_t value);

// The game's own layer-2 clear (the event commands' too): SetByte890(2, 6) + SetByte8A4(2, 0) on the primary; its gate
// reads the field leader, not the primary controller.
constexpr std::uint8_t kClearBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kClear{L"PrimaryPC_ClearMoveTypeLayer2_IfFieldLeaderPC", 0x1412E4484, kClearBytes};
using ClearFn = void (*)();

// The walk step: phase-table entry 4 of the behavior action vt 0x1458DFA88 (move rcx, event dl, dt xmm2).
constexpr std::uint8_t kPhaseFourBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                            0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41};
const platform::SlotFact kPhaseFour{L"the walk step's phase-4 handler (phase table entry 4)", 0x1470B98C0,
                                    {L"FieldActMove58DFA88_Phase4_WalkCapL2", 0x1432A4C74, kPhaseFourBytes}};
using PhaseFn = std::uintptr_t (*)(void* move, std::uint32_t event, float dt, std::uintptr_t fourth);
constexpr std::uint8_t kPhaseEnter = 0;
constexpr std::uint8_t kPhaseExit  = 11;

// Red XIII's chain door: Begin sets Walk (game thread), End clears it (anim worker).
constexpr std::uint8_t kRedDoorEndBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                             0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
const platform::SlotFact kRedDoorEnd{L"the chain-door action's End slot (+0x120)", 0x145CE5898,
                                     {L"RedChainDoorAction_End_Vt120_ClearL2", 0x1432BDE98, kRedDoorEndBytes}};
using EndFn = std::uintptr_t (*)(void* action, std::uintptr_t second);

constexpr std::uint32_t kActionCharacter = 0x450;  // FWeakObjectPtr {index, serial}
constexpr std::uint32_t kLayerTwo        = 0x892;  // controller +0x890[2]
constexpr std::uint8_t  kLayerEmpty      = 6;

std::optional<platform::GameImage> g_image;
platform::VTableEntry              g_phaseFour;
platform::VTableEntry              g_redDoorEnd;
bool                               g_phasePlaced = false;
bool                               g_redPlaced   = false;
ClearFn                            g_clear       = nullptr;

// Written on the game thread each tick, read on any thread (compared only).
std::atomic<const void*>   g_controllerOne{nullptr};
std::atomic<const void*>   g_controllerTwo{nullptr};

// Player 2's walk step: the controller its cap is on now (0 none, 1, 2). Its enter lands on player 2's controller from
// Begin (game thread, scoped) or on player 1's from Advance (worker); its exit clears whichever controller is primary
// then. When the two differ, the game tick clears the controller the cap is on (bit 1: player 1's, bit 2: player 2's)
// and, for a cap on player 1's while player 2's step is live, puts it on player 2's with the game's setter.
std::atomic<int>           g_stepOn{0};
std::atomic<int>           g_clearFor{0};      // bits 1 << player
std::atomic<bool>          g_moveToTwo{false};
// Player 1's own walk step is live (between its enter and exit): its cap on player 1's controller is the game's own,
// so a queued clear for player 1's controller waits for it (its exit clears it natively).
std::atomic<bool>          g_oneStepLive{false};
SetterFn                   g_setByte = nullptr;  // the setter's live entry (through the log when it is placed)
int                        g_layerSeen = -1;    // game thread: player 2's controller's layer 2 last logged
#if LSS_DEV
// The layer-2 write log (dev tools): the setter's entry redirected, each write logged with its caller.
constexpr std::size_t      kSetterFirst = 5;  // mov [rsp+8], rbx
platform::EntryRedirect    g_setter;
std::atomic<std::uint64_t> g_lastWrite{~0ull};  // the setter's last logged {caller, value, controller}
#endif

const wchar_t* ThreadText() noexcept
{
    return generation::OnGameThread() ? L"game" : L"worker";
}

/// 2: player 2's controller is the primary now, 1: player 1's, 0: another.
int PrimaryNow() noexcept
{
    if (primary_controller::Is(g_controllerTwo.load(std::memory_order_acquire)))
    {
        return 2;
    }
    return primary_controller::Is(g_controllerOne.load(std::memory_order_acquire)) ? 1 : 0;
}

const wchar_t* ControllerText(int who) noexcept
{
    return who == 2 ? L"player 2's controller" : who == 1 ? L"player 1's controller" : L"another controller";
}

const wchar_t* GaitText(int value) noexcept
{
    switch (value)
    {
    case 0: return L"Walk";
    case 1: return L"HighWalk";
    case 2: return L"Jog";
    case 3: return L"Run";
    case 4: return L"None";
    case kLayerEmpty: return L"off";
    default: return L"?";
    }
}

bool ActionOf(const void* action, std::uint64_t weak) noexcept
{
    return action != nullptr && weak != 0 && ReadField<std::uint64_t>(action, kActionCharacter) == weak;
}

bool PlayerTwosAction(const void* action) noexcept
{
    return ActionOf(action, generation::MemberWeak());
}

#if LSS_DEV
/// The setter (any thread): layer-2 writes logged, then the game's write.
void Setter(std::uint8_t layer, std::uint8_t value) noexcept
{
    const auto original = reinterpret_cast<SetterFn>(g_setter.Original());
    if (layer == 2 && generation::Active())
    {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto where  = g_image ? g_image->ResearchAddress(caller) : caller;
        const int  who    = PrimaryNow();
        const auto key    = ((where & 0xFFFFFFFFFFull) << 16) | (std::uint64_t{value} << 8) | static_cast<std::uint64_t>(who);
        if (g_lastWrite.exchange(key, std::memory_order_relaxed) != key)
        {
            platform::LogDebug(L"walk cap: layer 2 := {} ({}) on {} from 0x{:X} ({} thread)", value, GaitText(value),
                               ControllerText(who), where, ThreadText());
        }
        else
        {
            platform::LogTrace(L"walk cap: layer 2 := {} on {} from 0x{:X} ({} thread)", value, ControllerText(who),
                               where, ThreadText());
        }
    }
    original(layer, value);
}

/// Places the write log on the setter's entry; what the install line says of it.
const wchar_t* InstallWriteLog(const platform::GameImage& image) noexcept
{
    const auto at     = platform::FindFunction(image, kSetter);
    const auto first  = kSetter.entryBytes.first(kSetterFirst);
    const bool logged = at &&
                        g_setter.Install(*at, image.EntryInstruction(kSetter.address, first),
                                         reinterpret_cast<std::uintptr_t>(&Setter));
    return logged ? L"on (Debug)" : L"unavailable in this build (the setter's entry crosses a block)";
}

bool RemoveWriteLog() noexcept
{
    return g_setter.IsInstalled() && !g_setter.Restore();  // true: left in place
}
#else
const wchar_t* InstallWriteLog(const platform::GameImage&) noexcept { return L"off (dev tools)"; }
bool           RemoveWriteLog() noexcept { return false; }
#endif

/// Player 2's action ended its walk cap on another controller than the cap is on: that one's clear follows.
void AskClear(const wchar_t* what, int capOn, int clearedOn) noexcept
{
    g_clearFor.fetch_or(1 << capOn, std::memory_order_acq_rel);
    platform::LogDebug(L"walk cap: player 2's {} ends on the {} thread; the game's clear went to {}, the cap is on {}: "
                       L"its clear follows on the next game tick",
                       what, ThreadText(), ControllerText(clearedOn), ControllerText(capOn));
}

/// The walk step's phase-4 handler (game thread at Begin, anim worker at Tick and End).
std::uintptr_t PhaseFour(void* move, std::uint32_t event, float dt, std::uintptr_t fourth) noexcept
{
    const auto original = reinterpret_cast<PhaseFn>(g_phaseFour.Original());
    const auto code     = static_cast<std::uint8_t>(event & 0xFF);
    if (code != kPhaseEnter && code != kPhaseExit)
    {
        return original(move, event, dt, fourth);
    }
    // An enter's owner is published before the game's write, an exit's after its clear: the game tick never sees a
    // live step's cap without its owner (it would take it for a stray cap and clear it).
    if (ActionOf(move, generation::PlayerOnePawnWeak()))
    {
        if (code == kPhaseEnter)
        {
            g_oneStepLive.store(true, std::memory_order_release);
        }
        const auto result = original(move, event, dt, fourth);
        if (code == kPhaseExit)
        {
            g_oneStepLive.store(false, std::memory_order_release);
        }
        return result;
    }
    if (!PlayerTwosAction(move))
    {
        return original(move, event, dt, fourth);
    }
    const int who = PrimaryNow();  // the primary does not change inside the call
    if (code == kPhaseEnter)
    {
        g_stepOn.store(who, std::memory_order_release);
        if (who != 0)
        {
            // This step's cap is the live one now: a clear still queued for that controller would end it early.
            g_clearFor.fetch_and(~(1 << who), std::memory_order_acq_rel);
        }
        const auto result = original(move, event, dt, fourth);
        if (who == 1)  // after the write: the move clears player 1's controller
        {
            g_moveToTwo.store(true, std::memory_order_release);
        }
        platform::LogDebug(L"walk cap: player 2's walk step starts ({} thread): its Walk cap went to {}{}", ThreadText(),
                           ControllerText(who), who == 1 ? L"; it moves to player 2's on the next game tick" : L"");
        return result;
    }
    const auto result = original(move, event, dt, fourth);
    const int  on     = g_stepOn.exchange(0, std::memory_order_acq_rel);
    if (on != 0 && on != who)
    {
        AskClear(L"walk step", on, who);
        return result;
    }
    platform::LogDebug(L"walk cap: player 2's walk step ends ({} thread): its clear went to {}", ThreadText(),
                       ControllerText(who));
    return result;
}

/// Red XIII's chain-door End (anim worker on a finish, game thread on an abort).
std::uintptr_t RedDoorEnd(void* action, std::uintptr_t second) noexcept
{
    const auto original = reinterpret_cast<EndFn>(g_redDoorEnd.Original());
    const auto result   = original(action, second);
    const int who = PlayerTwosAction(action) ? PrimaryNow() : 2;
    if (who != 2)
    {
        AskClear(L"chain door", 2, who);  // its Begin runs on the game thread, in player 2's scope
    }
    return result;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image.emplace(image);
    const auto clear  = platform::FindFunction(image, kClear);
    g_clear           = clear ? reinterpret_cast<ClearFn>(*clear) : nullptr;
    const auto setter = platform::FindFunction(image, kSetter);
    g_setByte         = setter ? reinterpret_cast<SetterFn>(*setter) : nullptr;
    if (g_clear != nullptr)
    {
        g_phasePlaced = platform::ReplaceSlot(image, kPhaseFour, reinterpret_cast<std::uintptr_t>(&PhaseFour), g_phaseFour);
        g_redPlaced   = platform::ReplaceSlot(image, kRedDoorEnd, reinterpret_cast<std::uintptr_t>(&RedDoorEnd),
                                              g_redDoorEnd);
    }
    platform::Log(L"walk cap: player 2's walk step {}, chain door {} (their walk cap ends on player 2's controller, as "
                  L"player 1's does); layer-2 write log {}",
                  g_phasePlaced ? L"yes" : L"NO", g_redPlaced ? L"yes" : L"NO", InstallWriteLog(image));
}

void Remove() noexcept
{
    bool kept = false;
    kept |= g_phasePlaced && !g_phaseFour.Restore();
    kept |= g_redPlaced && !g_redDoorEnd.Restore();
    kept |= RemoveWriteLog();
    g_phasePlaced = g_redPlaced = false;
    g_clearFor.store(0, std::memory_order_release);
    g_moveToTwo.store(false, std::memory_order_release);
    g_stepOn.store(0, std::memory_order_release);
    g_oneStepLive.store(false, std::memory_order_release);
    if (kept)
    {
        platform::Log(L"walk cap: a slot or entry was changed after us; left in place (it passes through)");
    }
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    void* member = generation::Active() ? generation::Member() : nullptr;
    void* two    = member != nullptr ? generation::PlayerTwoController() : nullptr;
    if (member == nullptr || two == nullptr)
    {
        // A cap of player 2's step still on player 1's controller when the generation ends: no exit of player 2's
        // would clear it any more, so the game's clear runs now (only while that controller is the live primary).
        const bool oneCapped = (g_clearFor.load(std::memory_order_acquire) & (1 << 1)) != 0 ||
                               g_stepOn.load(std::memory_order_acquire) == 1 ||
                               g_moveToTwo.load(std::memory_order_acquire);
        const void* one = g_controllerOne.load(std::memory_order_acquire);  // the last active tick's
        if (oneCapped && !g_oneStepLive.load(std::memory_order_acquire) && g_clear != nullptr && one != nullptr &&
            primary_controller::Is(one) && ReadField<std::uint8_t>(one, kLayerTwo) != kLayerEmpty)
        {
            const int before = ReadField<std::uint8_t>(one, kLayerTwo);
            if (platform::CallGuarded(g_clear))
            {
                platform::LogDebug(L"walk cap: player 2 left with their walk step's cap on player 1's controller; the "
                                   L"game's layer-2 clear ran on it: {} -> {}",
                                   GaitText(before), GaitText(ReadField<std::uint8_t>(one, kLayerTwo)));
            }
        }
        g_oneStepLive.store(false, std::memory_order_release);
        g_controllerTwo.store(nullptr, std::memory_order_release);
        g_controllerOne.store(nullptr, std::memory_order_release);
        g_stepOn.store(0, std::memory_order_release);
        g_clearFor.store(0, std::memory_order_release);
        g_moveToTwo.store(false, std::memory_order_release);
        g_layerSeen = -1;
        return;
    }
    g_controllerTwo.store(two, std::memory_order_release);
    g_controllerOne.store(generation::PlayerOneController(), std::memory_order_release);

    void* one = generation::PlayerOneController();
    // A cap of player 2's step on player 1's controller: player 1's is cleared, and player 2's step, if still live,
    // gets its cap on player 2's controller.
    if (g_moveToTwo.exchange(false, std::memory_order_acq_rel))
    {
        g_clearFor.fetch_or(1 << 1, std::memory_order_acq_rel);  // player 2's step never caps player 1
        if (g_setByte != nullptr && g_stepOn.load(std::memory_order_acquire) == 1)
        {
            bool set = false;
            {
                const primary_controller::Scope scope;
                set = scope.Held() && platform::CallGuarded(g_setByte, std::uint8_t{2}, std::uint8_t{0});
            }
            int expected = 1;
            if (set)
            {
                g_stepOn.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
                platform::LogDebug(L"walk cap: player 2's live walk step's Walk cap put on player 2's controller");
            }
            else
            {
                g_moveToTwo.store(true, std::memory_order_release);  // a refused scope: again next tick
                platform::Log(L"walk cap: player 2's walk step's cap could not move to player 2's controller this tick "
                              L"(the primary scope was refused); retried next tick");
            }
        }
    }
    const int clearFor = g_clearFor.exchange(0, std::memory_order_acq_rel);
    for (int player = 1; player <= 2; ++player)
    {
        if ((clearFor & (1 << player)) == 0)
        {
            continue;
        }
        void*     controller = player == 2 ? two : one;
        const int before     = controller != nullptr ? ReadField<std::uint8_t>(controller, kLayerTwo) : kLayerEmpty;
        if (before == kLayerEmpty)
        {
            platform::LogDebug(L"walk cap: player {}'s controller's layer 2 is already off; no clear needed", player);
            continue;
        }
        // A live step owns this controller's cap now: player 1's own step on player 1's, or player 2's on player 2's.
        // Its own exit clears it; the queued clear is dropped. Asked again right before the call (a worker's enter can
        // land in between).
        const auto owned = [player] {
            return player == 1 ? g_oneStepLive.load(std::memory_order_acquire)
                               : g_stepOn.load(std::memory_order_acquire) == 2;
        };
        bool ran     = false;
        bool dropped = false;
        if (player == 2)
        {
            const primary_controller::Scope scope;
            dropped = owned();
            ran     = !dropped && scope.Held() && g_clear != nullptr && platform::CallGuarded(g_clear);
        }
        else
        {
            dropped = owned();
            ran     = !dropped && PrimaryNow() == 1 && g_clear != nullptr && platform::CallGuarded(g_clear);
        }
        if (dropped)
        {
            platform::LogDebug(L"walk cap: player {}'s controller's cap belongs to a live walk step now; the queued "
                               L"clear is dropped (that step's exit clears it)",
                               player);
        }
        else if (ran)
        {
            platform::LogDebug(L"walk cap: the game's layer-2 clear ran on player {}'s controller: {} -> {}", player,
                               GaitText(before), GaitText(ReadField<std::uint8_t>(controller, kLayerTwo)));
        }
        else
        {
            g_clearFor.fetch_or(1 << player, std::memory_order_acq_rel);  // again next tick
            platform::Log(L"walk cap: player {}'s clear could not run this tick (the primary controller was not "
                          L"theirs); retried next tick",
                          player);
        }
    }
    const int now = ReadField<std::uint8_t>(two, kLayerTwo);
    if (now != g_layerSeen)
    {
        if (g_layerSeen >= 0 || now != kLayerEmpty)
        {
            platform::LogDebug(L"walk cap: player 2's controller's layer 2 (action move type) now {}", GaitText(now));
        }
        g_layerSeen = now;
    }
}

}  // namespace lss::player_two::move_type_two
