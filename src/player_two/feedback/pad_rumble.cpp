#include "player_two/feedback/pad_rumble.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/object_fields.h"
#include "player_two/scopes/leader_scope.h"
#include "players/unreal_access.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::player_two::pad_rumble {

namespace {

using Clock = std::chrono::steady_clock;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;

// RE-p2-rumble.md §9 (this build).
// SquareInput__NoteLastActiveDevice(owner, userIndex): the input layer's "last active device" for player 1. Entry:
// mov [rsp+8], rbx (the 5 bytes replaced), position-independent; no branch into +1..+4.
constexpr std::uint8_t   kNoteLastDeviceBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                   0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41};
constexpr platform::FunctionFact kNoteLastDevice{L"SquareInput__NoteLastActiveDevice", 0x1415F4900,
                                                 kNoteLastDeviceBytes};
constexpr std::size_t    kNoteLastDeviceFirst   = 5;
using facts::kSquareInputGlobal;
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

// Game thread.
std::int32_t      g_twoPad = -1;  // player 2's ControllerId while in (and not player 1's), else -1
void*             g_orderedOne = nullptr;
void*             g_orderedTwo = nullptr;
std::uint64_t     g_orderedOneWeak = 0;
std::uint64_t     g_orderedTwoWeak = 0;
bool              g_orderBroken = false;
std::uint32_t     g_skips  = 0;
std::uint32_t     g_passes = 0;
std::int32_t      g_codeLogged = 0;
std::int32_t      g_padLogged  = -1;
bool              g_firstLogged = false;
platform::Every   g_status{kStatusEvery};

const void* Pointee(std::uintptr_t global) noexcept
{
    return reinterpret_cast<const void*>(ReadField<std::uintptr_t>(reinterpret_cast<const void*>(global), 0));
}

/// The game's last-device note; an event from player 2's pad leaves player 1's last device as it is.
void Note(void* owner, std::int32_t userIndex) noexcept
{
    const bool gameThread = generation::OnGameThread();
    if (gameThread && userIndex >= 0 && userIndex == g_twoPad)
    {
        ++g_skips;
        return;
    }
    g_passes += gameThread ? 1 : 0;  // the counters are the game thread's
    reinterpret_cast<NoteFn>(g_note.Original())(owner, userIndex);
}

bool Ordered(void* one, void* two) noexcept
{
    const auto still = [](void* raw, std::uint64_t weak, void* now) {
        return raw == now && (weak == 0 || leader_scope::StillNames(weak, now));
    };
    return still(g_orderedOne, g_orderedOneWeak, one) && still(g_orderedTwo, g_orderedTwoWeak, two);
}

/// Lever A: player 2's controller ticks after player 1's (the engine's own prerequisite), once per pair.
void Order(void* one, void* two) noexcept
{
    if (g_addPrerequisite == nullptr || g_orderBroken || Ordered(one, two))
    {
        return;
    }
    const auto before = ReadField<std::int32_t>(two, kTickPrerequisites);
    struct
    {
        void* prerequisite;
    } params{one};
    const bool called = players::unreal::GuardedProcessEvent(static_cast<UObject*>(two), g_addPrerequisite, &params);
    const auto after  = ReadField<std::int32_t>(two, kTickPrerequisites);
    g_orderedOne      = one;
    g_orderedTwo      = two;
    g_orderedOneWeak  = leader_scope::WeakOf(one);
    g_orderedTwoWeak  = leader_scope::WeakOf(two);
    g_orderBroken     = !called;
    try
    {
        platform::Log(L"pad rumble: player 2's controller ticks after player 1's from now (the engine's "
                      L"AddTickPrerequisiteActor{}; its tick prerequisites {} -> {})",
                      called ? L"" : L" RAISED AN EXCEPTION: not called again", before, after);
    }
    catch (...)
    {
    }
}

/// Player 1's device code and last XInput pad on change, and the skips once a minute (while player 2 is in).
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
                          code, pad, code == -2 - 3 * g_twoPad ? L": PLAYER 2'S PAD" : L"");
        }
        const auto now = Clock::now();
        if (g_status.Due(now))
        {
            if (g_skips != 0 || g_passes != 0)
            {
                platform::LogDebug(L"pad rumble (60 s): {} input event(s) from player 2's pad (pad {}) kept out of player 1's "
                              L"last device, {} passed",
                              g_skips, g_twoPad, g_passes);
            }
            g_skips  = 0;
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
    const auto owner = image.Rebase(kSquareInputGlobal, sizeof(void*));
    const auto app   = image.Rebase(kPlatformApplication, sizeof(void*));
    const auto note  = platform::FindFunction(image, kNoteLastDevice);
    g_owner          = owner ? *owner : 0;
    g_app            = app ? *app : 0;
    const bool device = note &&
                        g_note.Install(*note,
                                       image.EntryInstruction(kNoteLastDevice.address,
                                                              kNoteLastDevice.entryBytes.first(kNoteLastDeviceFirst)),
                                       reinterpret_cast<std::uintptr_t>(&Note));
    const bool weaks = leader_scope::Installed();  // the weak values that tell a new pair of controllers apart
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
                  g_addPrerequisite != nullptr ? L"ready (player 2's controller after player 1's, the engine's prerequisite)"
                                               : L"UNAVAILABLE (no AddTickPrerequisiteActor)",
                  device ? L"kept from player 2's pad (the game's note skipped for it)"
                         : L"NOT kept (the note's entry is not what the research read)");
}

void BeforeTick() noexcept
{
    void* one = generation::PlayerOneController();
    void* two = generation::PlayerTwoController();
    if (!generation::OnGameThread() || !generation::Active() || one == nullptr || two == nullptr)
    {
        g_twoPad = -1;
        return;
    }
    const auto oneId = pad_sticks::ControllerIdOf(one);
    const auto twoId = pad_sticks::ControllerIdOf(two);
    g_twoPad         = twoId >= 0 && twoId != oneId ? twoId : -1;
    Order(one, two);
    LogDevice();
}

void Remove() noexcept
{
    if (!g_note.Restore())
    {
        platform::Log(L"pad rumble: the last-device note's entry was changed after us; left in place (it passes "
                      L"through)");
    }
}

}  // namespace lss::player_two::pad_rumble
