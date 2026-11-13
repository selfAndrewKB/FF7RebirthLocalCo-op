#include "rides/chocobo_exits.h"

#include "facts/controller.h"
#include "input/member_movement.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "rides/chocobo_ride.h"
#include "scopes/primary_controller.h"

#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::rides::chocobo_exits {

namespace {

// The chocobo's glide (state 26) and water-jet (state 27) control states: the enter (state, param) and the exit
// (state) of each (research/RE-p2-chocobo-traversal.md, RE-chocobo-exits-pads.md). 1.005 slots and functions through
// replaced-chocobo-traversal.json.
constexpr std::uint8_t kGlideEnterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                             0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
constexpr std::uint8_t kGlideExitBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41};
constexpr std::uint8_t kJetEnterBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x18, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8D};
constexpr std::uint8_t kJetExitBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                             0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8};
constexpr platform::SlotFact kGlideEnterSlot{L"the chocobo glide state's enter slot", 0x145795408,
                                             {L"CharCtrlStateChocoboGlide_OnEnter_Vt18", 0x143427234, kGlideEnterBytes}};
constexpr platform::SlotFact kGlideExitSlot{L"the chocobo glide state's exit slot", 0x145795410,
                                            {L"CharCtrlStateChocoboGlide_OnExit_Vt20", 0x14342810C, kGlideExitBytes}};
constexpr platform::SlotFact kJetEnterSlot{L"the chocobo jet state's enter slot", 0x1457C5958,
                                           {L"CharCtrlStateChocoboJet_OnEnter_Vt18", 0x143432B04, kJetEnterBytes}};
constexpr platform::SlotFact kJetExitSlot{L"the chocobo jet state's exit slot", 0x1457C5960,
                                          {L"CharCtrlStateChocoboJet_OnExit_Vt20", 0x143433BE4, kJetExitBytes}};
using StateEnterFn = std::uintptr_t (*)(void* state, void* param);
using StateExitFn  = std::uintptr_t (*)(void* state);
// A control state's holder, and the holder's character (a weak).
constexpr std::uint32_t kStateHolder     = 0x8;
constexpr std::uint32_t kHolderCharacter = 0x10;

platform::VTableEntry              g_glideEnter, g_glideExit, g_jetEnter, g_jetExit;
std::optional<platform::GameImage> g_image;  // the exits' callers named by research address in the log

/// The chocobo the state belongs to, or null.
void* ChocoboOf(const void* state) noexcept
{
    const void* holder = ReadPointer(state, kStateHolder);
    return holder != nullptr ? players::weak_object::ObjectAt(static_cast<const std::byte*>(holder) + kHolderCharacter)
                             : nullptr;
}

/// Whose chocobo, for the log (game thread).
std::wstring Whose(const void* chocobo)
{
    if (chocobo == nullptr)
    {
        return L"an unread chocobo";
    }
    if (chocobo == players::actor_fields::Pawn(players::generation::PlayerOneController()))
    {
        return L"player 1's chocobo";
    }
    const int seat = chocobo_ride::SeatDriving(chocobo);
    return seat >= 1 ? std::format(L"{}'s chocobo", players::SeatName(seat))
                     : std::format(L"another chocobo ({})", players::NameOf(chocobo));
}

/// Whether a seat's scope is open for the enter's log (game thread).
const wchar_t* ScopeText(const void* chocobo) noexcept
{
    const int seat = chocobo_ride::SeatDriving(chocobo);
    if (seat < 1)
    {
        return L"as the game runs it";
    }
    const bool held = scopes::primary_controller::Is(players::generation::ControllerOf(seat));
    return held ? players::BySeat(seat, L"in player 2's scope", L"in player 3's scope")
                : players::BySeat(seat, L"outside player 2's scope", L"outside player 3's scope");
}

void LogStep(const wchar_t* state, const wchar_t* step, const void* chocobo, bool gameThread, std::uintptr_t from,
             const wchar_t* how) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"chocobo exits: the {} state {} for {} from 0x{:X}: {}", state, step,
                           gameThread ? Whose(chocobo) : std::wstring{L"a chocobo (off the game thread)"},
                           g_image ? g_image->ResearchAddress(from) : from, how);
    }
    catch (...)
    {
    }
}

/// `from`: the wrapper's return address, the game's caller.
std::uintptr_t Enter(const wchar_t* name, const platform::VTableEntry& slot, void* state, void* param,
                     std::uintptr_t from) noexcept
{
    const auto original   = reinterpret_cast<StateEnterFn>(slot.Original());
    const bool gameThread = players::generation::OnGameThread();
    const void* chocobo   = gameThread ? ChocoboOf(state) : nullptr;
    LogStep(name, L"enters", chocobo, gameThread, from, gameThread ? ScopeText(chocobo) : L"off the game thread");
    return original(state, param);
}

struct ExitCall
{
    StateExitFn    fn;
    void*          state;
    std::uintptr_t result = 0;
};

void RunExit(void* context) noexcept
{
    auto& call  = *static_cast<ExitCall*>(context);
    call.result = call.fn(call.state);
}

/// A state's exit: a seat's chocobo's runs inside the seat's scope when it is not open already. `from`: the
/// wrapper's return address, the game's caller.
std::uintptr_t Exit(const wchar_t* name, const platform::VTableEntry& slot, void* state, std::uintptr_t from) noexcept
{
    const auto original = reinterpret_cast<StateExitFn>(slot.Original());
    if (!players::generation::OnGameThread())
    {
        LogStep(name, L"exits", nullptr, false, from, L"off the game thread: the game's call as it is");
        return original(state);
    }
    void*     chocobo = ChocoboOf(state);
    const int seat    = chocobo_ride::SeatDriving(chocobo);
    if (seat < 1)
    {
        LogStep(name, L"exits", chocobo, true, from, L"as the game runs it");
        return original(state);
    }
    if (scopes::primary_controller::Is(players::generation::ControllerOf(seat)))
    {
        LogStep(name, L"exits", chocobo, true, from, players::BySeat(seat, L"in player 2's scope", L"in player 3's scope"));
        return original(state);
    }
    ExitCall call{original, state};
    if (input::member_movement::InPrimaryScope(seat, players::generation::MemberOf(seat), &RunExit, &call))
    {
        LogStep(name, L"exits", chocobo, true, from,
                players::BySeat(seat, L"ran outside player 2's scope: wrapped in it",
                                L"ran outside player 3's scope: wrapped in it"));
        return call.result;
    }
    platform::Log(L"WARNING chocobo exits: the {} state exits for {}'s chocobo outside {}'s scope and the scope was "
                  L"refused: the game's call as it is (the jet's camera layer clears on player 1's camera, the glide's "
                  L"shake stops there)",
                  name, players::SeatName(seat), players::SeatName(seat));
    return original(state);
}

std::uintptr_t GlideEnter(void* state, void* param) noexcept
{
    return Enter(L"glide", g_glideEnter, state, param, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

std::uintptr_t GlideExit(void* state) noexcept
{
    return Exit(L"glide", g_glideExit, state, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

std::uintptr_t JetEnter(void* state, void* param) noexcept
{
    return Enter(L"water-jet", g_jetEnter, state, param, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

std::uintptr_t JetExit(void* state) noexcept
{
    return Exit(L"water-jet", g_jetExit, state, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image = image;
    // The two exits together or not at all (a seat's glide ending in a jet, or the reverse, would clear one camera and
    // not the other); the enters are the log's alone.
    const bool exits = platform::ReplaceSlot(image, kGlideExitSlot, reinterpret_cast<std::uintptr_t>(&GlideExit), g_glideExit) &&
                       platform::ReplaceSlot(image, kJetExitSlot, reinterpret_cast<std::uintptr_t>(&JetExit), g_jetExit);
    if (!exits)
    {
        (void)g_glideExit.Restore();
    }
    const bool enters = platform::ReplaceSlot(image, kGlideEnterSlot, reinterpret_cast<std::uintptr_t>(&GlideEnter), g_glideEnter) &
                        platform::ReplaceSlot(image, kJetEnterSlot, reinterpret_cast<std::uintptr_t>(&JetEnter), g_jetEnter);
    platform::Log(L"chocobo exits: {}; the enters {}",
                  exits ? L"player 2's or 3's chocobo leaves the glide and the water jet in that player's scope, whoever "
                          L"ends them (the jet's camera layer and the glide's shake on their own camera)"
                        : L"NOT placed (an exit slot differs): a forced end of player 2's or 3's glide or jet clears "
                          L"player 1's camera layer, as the game runs it",
                  enters ? L"logged" : L"NOT logged (a slot differs)");
}

void Remove() noexcept
{
    const bool restored = g_glideEnter.Restore() & g_glideExit.Restore() & g_jetEnter.Restore() & g_jetExit.Restore();
    if (!restored)
    {
        platform::Log(L"chocobo exits: a slot was changed after us and is left in place (it passes through)");
    }
}

}  // namespace lss::rides::chocobo_exits
