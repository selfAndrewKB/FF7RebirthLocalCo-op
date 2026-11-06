#include "session/own_controller.h"

#include "camera/camera_override.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/object_fields.h"
#include "players/seats.h"

#include <atomic>
#include <cstdint>

namespace lss::session::own_controller {

namespace {

using BeginPlayFn = void (*)(void* cameraManager);

// Set once at install, then only read.
platform::VTableEntry g_slot;
BeginPlayFn           g_engineBeginPlay     = nullptr;
std::uint64_t*        g_endCameraName       = nullptr;
std::uint64_t*        g_compositeName       = nullptr;
const void*           g_compositeNameGuard  = nullptr;
std::uintptr_t        g_playerControllerVTable = 0;
std::uintptr_t        g_cameraManagerVTable    = 0;

// Game thread: set by JoinScope for the length of CreatePlayer; the spawn runs inside it.
std::atomic<void*> g_joiningPlayerOne{nullptr};
std::atomic<int>   g_joiningSeat{kNoSeat};
Report             g_report{};

/// Fills PCOwner from Owner when it is still null. The owner must be an End player controller
/// that has no other camera manager (the engine publishes this one to it right after the spawn).
void FillOwner(void* cameraManager) noexcept
{
    if (ReadPointer(cameraManager, facts::kCameraManagerPcOwner) != nullptr)
    {
        return;
    }
    void*       owner       = ReadPointer(cameraManager, facts::kActorOwner);
    const void* ownersCamera = ReadPointer(owner, facts::kControllerCameraManager);
    if (VTableOf(owner) != g_playerControllerVTable || (ownersCamera != nullptr && ownersCamera != cameraManager))
    {
        g_report.ownerRefused = true;
        return;
    }
    WriteField<void*>(cameraManager, facts::kCameraManagerPcOwner, owner);
    g_report.ownerFilled = true;
}

/// Runs the engine's BeginPlay with both spawn names set to None. The names' only reader is inside
/// this call, so the change is invisible outside it. They are put back on every way out, including
/// a structured exception raised inside the engine (the join's guard further up catches it and the
/// game carries on, so every later camera manager must find the names intact). __finally runs
/// during that unwind; MSVC allows it only in a function with no objects that need unwinding.
void BeginPlayWithGeneratedNames(void* cameraManager)  // not noexcept: the exception must pass through
{
    const std::uint64_t endCameraName = *g_endCameraName;
    const std::uint64_t compositeName = *g_compositeName;
    __try
    {
        *g_endCameraName = facts::kNameNone;
        *g_compositeName = facts::kNameNone;
        // Its own NotifyOverride module, kept for the joining seat only.
        camera::camera_override::BeginPlayWithOwnModule(g_joiningSeat.load(std::memory_order_relaxed), g_engineBeginPlay,
                                                cameraManager);
    }
    __finally
    {
        *g_endCameraName = endCameraName;
        *g_compositeName = compositeName;
    }
}

/// Same signature as AEndPlayerCameraManager::BeginPlay.
void BeginPlay(void* cameraManager)
{
    void* playerOne = g_joiningPlayerOne.load(std::memory_order_relaxed);
    void* owner     = ReadPointer(cameraManager, facts::kActorOwner);
    if (playerOne == nullptr || owner == nullptr || owner == playerOne)
    {
        g_engineBeginPlay(cameraManager);
        return;
    }

    g_report               = {};
    g_report.cameraManager = cameraManager;
    g_report.namesSwapped  = true;
    FillOwner(cameraManager);
    BeginPlayWithGeneratedNames(cameraManager);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto endCamera  = image.Rebase(facts::kEndCameraActorNameGlobal, sizeof(std::uint64_t));
    const auto composite  = image.Rebase(facts::kCompositeCollisionNameGlobal, sizeof(std::uint64_t));
    const auto guard      = image.Rebase(facts::kCompositeCollisionNameGuard, sizeof(std::int32_t));
    const auto controller = image.Rebase(facts::kEndPlayerControllerVTable);
    const auto camera     = image.Rebase(facts::kCameraManagerVTable);
    const auto engineFn   = platform::FindFunction(image, facts::kCameraManagerBeginPlay.target);
    if (!endCamera || !composite || !guard || !controller || !camera || !engineFn)
    {
        return false;
    }
    g_cameraManagerVTable = *camera;
    g_endCameraName          = reinterpret_cast<std::uint64_t*>(*endCamera);
    g_compositeName          = reinterpret_cast<std::uint64_t*>(*composite);
    g_compositeNameGuard     = reinterpret_cast<const void*>(*guard);
    g_playerControllerVTable = *controller;
    g_engineBeginPlay        = reinterpret_cast<BeginPlayFn>(*engineFn);
    return platform::ReplaceSlot(image, facts::kCameraManagerBeginPlay, reinterpret_cast<std::uintptr_t>(&BeginPlay),
                                 g_slot);
}

void Remove() noexcept
{
    if (!g_slot.Restore())
    {
        platform::Log(L"could not restore the camera manager's BeginPlay slot; the replacement stays in place");
    }
}

bool Ready() noexcept
{
    return g_engineBeginPlay != nullptr && facts::NameGuardInitialised(g_compositeNameGuard) &&
           *g_endCameraName != facts::kNameNone && *g_compositeName != facts::kNameNone;
}

bool UsesNativeClasses(const void* playerOneController) noexcept
{
    return playerOneController != nullptr && VTableOf(playerOneController) == g_playerControllerVTable &&
           VTableOf(ReadPointer(playerOneController, facts::kControllerCameraManager)) == g_cameraManagerVTable;
}

void* JoiningPlayerOne() noexcept
{
    return g_joiningPlayerOne.load(std::memory_order_relaxed);
}

int JoiningSeat() noexcept
{
    return g_joiningSeat.load(std::memory_order_relaxed);
}

JoinScope::JoinScope(void* playerOneController, int seat) noexcept
{
    if (seat != kNoSeat && !players::SeatBuilt(seat))
    {
        // It would get a mirror view's treatment: no camera module of its own, the game's force-feedback manager.
        platform::LogWarning(L"own controller: a join names player index {} (player 1 is 0), which is not built; its player gets "
                             L"nothing of player 2's or 3's", seat);
    }
    g_report = {};
    g_joiningSeat.store(seat, std::memory_order_relaxed);
    g_joiningPlayerOne.store(playerOneController, std::memory_order_relaxed);
}

JoinScope::~JoinScope()
{
    g_joiningPlayerOne.store(nullptr, std::memory_order_relaxed);
    g_joiningSeat.store(kNoSeat, std::memory_order_relaxed);
}

Report JoinScope::Result() const noexcept
{
    return g_report;
}

}  // namespace lss::session::own_controller
