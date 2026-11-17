#include "feedback/pad_rumble.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/controller.h"
#include "facts/engine.h"
#include "input/pad_sticks.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/leader_scope.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::feedback::pad_rumble {

namespace {

using Clock = std::chrono::steady_clock;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;

// research/RE-p2-rumble.md (this build).
// SquareInput__NoteLastActiveDevice(owner, userIndex): the input layer's "last active device" for player 1. Entry:
// mov [rsp+8], rbx (the 5 bytes replaced), position-independent; no branch into +1..+4.
constexpr std::uint8_t   kNoteLastDeviceBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                   0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41};
constexpr platform::FunctionFact kNoteLastDevice{L"SquareInput__NoteLastActiveDevice", 0x140E936D4,
                                                 kNoteLastDeviceBytes};
constexpr std::size_t    kNoteLastDeviceFirst   = 5;
constexpr std::uint32_t  kOwnerPad      = 0x74;         // i32: the last XInput pad
using facts::kPlatformApplication;
using facts::kPlatformAppDeviceCode;
// AActor::PrimaryActorTick at +0x28; its Prerequisites TArray at +0x38, Num at +0x40 (stock 4.26 layout).
constexpr std::uint32_t kTickPrerequisites = 0x40;

constexpr auto kStatusEvery = std::chrono::seconds(60);

using NoteFn = void (*)(void* owner, std::int32_t userIndex);

// Set once at install.
platform::EntryRedirect g_note;
UFunction*              g_addPrerequisite = nullptr;  // /Script/Engine.Actor:AddTickPrerequisiteActor
std::uintptr_t          g_owner = 0;
std::uintptr_t          g_app   = 0;

// Game thread. Per seat (index 0, player 1's, unused): the seat's pad while in, the pair of controllers last ordered,
// the input events kept out of player 1's last device.
struct SeatPad
{
    std::int32_t  pad             = -1;  // the seat's ControllerId while in (and not player 1's), else -1
    void*         orderedOne      = nullptr;
    void*         orderedSeat     = nullptr;
    std::uint64_t orderedOneWeak  = 0;
    std::uint64_t orderedSeatWeak = 0;
    std::uint32_t skips           = 0;
};
SeatPad           g_seats[players::kMaxPlayers];
bool              g_orderBroken = false;
std::uint32_t     g_passes = 0;
std::int32_t      g_codeLogged = 0;
std::int32_t      g_padLogged  = -1;
bool              g_firstLogged = false;
platform::Every   g_status{kStatusEvery};

const void* Pointee(std::uintptr_t global) noexcept
{
    return reinterpret_cast<const void*>(ReadField<std::uintptr_t>(reinterpret_cast<const void*>(global), 0));
}

/// The game's last-device note; an event from any seat's pad leaves player 1's last device as it is.
void Note(void* owner, std::int32_t userIndex) noexcept
{
    const bool gameThread = players::generation::OnGameThread();
    for (int seat = 1; gameThread && userIndex >= 0 && seat <= players::kSeatsBuilt; ++seat)
    {
        if (userIndex == g_seats[seat].pad)
        {
            ++g_seats[seat].skips;
            return;
        }
    }
    g_passes += gameThread ? 1 : 0;  // the counters are the game thread's
    reinterpret_cast<NoteFn>(g_note.Original())(owner, userIndex);
}

bool Ordered(const SeatPad& record, void* one, void* controller) noexcept
{
    const auto still = [](void* raw, std::uint64_t weak, void* now) {
        return raw == now && (weak == 0 || scopes::leader_scope::StillNames(weak, now));
    };
    return still(record.orderedOne, record.orderedOneWeak, one) &&
           still(record.orderedSeat, record.orderedSeatWeak, controller);
}

/// Lever A: `seat`'s controller ticks after player 1's (the engine's own prerequisite), once per pair.
void Order(int seat, void* one, void* controller) noexcept
{
    SeatPad& record = g_seats[seat];
    if (g_addPrerequisite == nullptr || g_orderBroken || Ordered(record, one, controller))
    {
        return;
    }
    const auto before = ReadField<std::int32_t>(controller, kTickPrerequisites);
    struct
    {
        void* prerequisite;
    } params{one};
    const bool called = players::unreal::GuardedProcessEvent(static_cast<UObject*>(controller), g_addPrerequisite,
                                                             &params);
    const auto after  = ReadField<std::int32_t>(controller, kTickPrerequisites);
    record.orderedOne      = one;
    record.orderedSeat     = controller;
    record.orderedOneWeak  = scopes::leader_scope::WeakOf(one);
    record.orderedSeatWeak = scopes::leader_scope::WeakOf(controller);
    g_orderBroken          = !called;
    try
    {
        platform::Log(L"pad rumble: {}'s controller ticks after player 1's from now (the engine's "
                      L"AddTickPrerequisiteActor{}; its tick prerequisites {} -> {})",
                      players::SeatName(seat), called ? L"" : L" RAISED AN EXCEPTION: not called again", before, after);
    }
    catch (...)
    {
    }
}

/// The note naming the seat whose pad player 1's last device code is (code -2-3k for XInput pad k), or "".
const wchar_t* SeatPadNote(std::int32_t code) noexcept
{
    constexpr const wchar_t* kNotes[players::kMaxPlayers] = {L"", L": PLAYER 2'S PAD", L": PLAYER 3'S PAD"};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (code == -2 - 3 * g_seats[seat].pad)
        {
            return kNotes[seat];
        }
    }
    return L"";
}

/// Player 1's device code and last XInput pad on change, and the skips once a minute (while a seat is in): a line per
/// seat in.
void LogDevice() noexcept
{
    const auto code = ReadField<std::int32_t>(Pointee(g_app), kPlatformAppDeviceCode);
    const auto pad  = ReadField<std::int32_t>(Pointee(g_owner), kOwnerPad);
    try
    {
        if ((!g_firstLogged || code != g_codeLogged || pad != g_padLogged))
        {
            g_firstLogged = true;
            g_codeLogged  = code;
            g_padLogged   = pad;
            platform::Log(L"pad rumble: player 1's last device: code {} (XInput pad k = -2-3k; -1 keyboard), last XInput "
                          L"pad {}{}",
                          code, pad, SeatPadNote(code));
        }
        const auto now = Clock::now();
        if (g_status.Due(now))
        {
            for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
            {
                SeatPad& record = g_seats[seat];
                if ((record.skips != 0 || g_passes != 0) && players::generation::Active(seat))
                {
                    platform::LogDebug(L"pad rumble (60 s): {} input event(s) from {}'s pad (pad {}) kept out of "
                                       L"player 1's last device, {} passed",
                                       record.skips, players::SeatName(seat), record.pad, g_passes);
                }
                record.skips = 0;
            }
            g_passes = 0;
        }
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto owner = players::game_globals::Address(players::game_globals::Global::SquareInputOwner);
    const auto app   = image.Rebase(kPlatformApplication, sizeof(void*));
    const auto note  = platform::FindFunction(image, kNoteLastDevice);
    g_owner          = owner;
    g_app            = app ? *app : 0;
    const bool device = note &&
                        g_note.Install(*note,
                                       image.EntryInstruction(kNoteLastDevice.address,
                                                              kNoteLastDevice.entryBytes.first(kNoteLastDeviceFirst)),
                                       reinterpret_cast<std::uintptr_t>(&Note));
    const bool weaks = scopes::leader_scope::Installed();  // the weak values that tell a new pair of controllers apart
    try
    {
        g_addPrerequisite = RC::Unreal::UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Engine.Actor:AddTickPrerequisiteActor"));
        if (g_addPrerequisite != nullptr && g_addPrerequisite->GetParmsSize() != sizeof(void*))
        {
            g_addPrerequisite = nullptr;  // not the one-pointer parameter block the call builds
        }
    }
    catch (...)
    {
        g_addPrerequisite = nullptr;
    }
    if (!weaks)
    {
        platform::Log(L"pad rumble: weak object values unavailable: a controller re-made at the same address is not "
                      L"ordered again");
    }
    platform::Log(L"pad rumble: tick order {}; player 1's last device {}",
                  g_addPrerequisite != nullptr ? L"ready (player 2's and 3's controllers after player 1's, the engine's prerequisite)"
                                               : L"UNAVAILABLE (no AddTickPrerequisiteActor)",
                  device ? L"kept from players 2 and 3's pads (the game's note skipped for them)"
                         : L"NOT kept (the note's entry is not what the research read)");
}

void BeforeTick() noexcept
{
    void* const one  = players::generation::PlayerOneController();
    const bool  game = players::generation::OnGameThread();
    bool        any  = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)  // each seat's controller after player 1's, in seat order
    {
        void* const controller = players::generation::ControllerOf(seat);
        if (!game || !players::generation::Active(seat) || one == nullptr || controller == nullptr)
        {
            g_seats[seat].pad = -1;
            continue;
        }
        const auto oneId  = input::pad_sticks::ControllerIdOf(one);
        const auto seatId = input::pad_sticks::ControllerIdOf(controller);
        g_seats[seat].pad = seatId >= 0 && seatId != oneId ? seatId : -1;
        Order(seat, one, controller);
        any = true;
    }
    if (any)
    {
        LogDevice();
    }
}

void Remove() noexcept
{
    if (!g_note.Restore())
    {
        platform::Log(L"pad rumble: the last-device note's entry was changed after us; left in place (it passes "
                      L"through)");
    }
}

}  // namespace lss::feedback::pad_rumble
