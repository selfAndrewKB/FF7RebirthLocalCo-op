#include "player_two/camera/camera_override.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/build_value.h"
#include "platform/byte_scope.h"
#include "platform/dev_switch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/camera/camera_access.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/scopes/primary_controller.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::camera_override {

namespace {

// Facts read from the database (RE-p2-battle-camera.md §5; this build).
constexpr std::uintptr_t kModuleGlobal   = 0x1491A4B78;  // TWeakPtr {object, reference controller}
constexpr std::uintptr_t kModuleVTable   = 0x1466B70D0;  // FEndNotifyOverrideCameraModule
constexpr std::uint32_t  kSlotFour       = 4;            // layer 4, NotifyOverride (player_two/camera/camera_access.h)
constexpr std::uint32_t  kModuleOwner    = 0x50;         // the owner operator
constexpr std::uint32_t  kModuleActive   = 0x44;         // entering / active bytes
constexpr std::uint32_t  kModuleRequests = 0x3F8;        // int32: request count
using facts::kCameraManagerBattleOperator;
constexpr std::uint32_t  kFieldOperator  = 0x2CB0;       // operator 12
// A player character's override delegates: the added one (fn at +0x17A0, payload at +0x17C8), the
// removed one (fn at +0x17E0, payload at +0x1808), and the requests it holds (+0x1770 count). 1.005 moved the Square
// Enix character fields by +0x20 (RE-1005-player-two.md: both fn fields read in code, CONFIRMED). The payloads:
// CONFIRMED, OnLeaderChanged 0x1412FB179..0x1412FB1F2 swaps each 0x40-byte delegate whole at +0x17C0 / +0x1800 (1.004:
// +0x17A0 / +0x17E0), so the payload keeps its place inside it. The count: CONFIRMED, RemoveCameraOverride 0x14102F3F5
// mov esi,[rcx+1790h] (1.004 0x141200B15 [rcx+1770h]); RE-1005-offsets-B.md.
constexpr platform::PerBuild<std::uint32_t> kAddedFn        = {0x17A0, 0x17C0};
constexpr platform::PerBuild<std::uint32_t> kAddedPayload   = {0x17C8, 0x17E8};
constexpr platform::PerBuild<std::uint32_t> kRemovedFn      = {0x17E0, 0x1800};
constexpr platform::PerBuild<std::uint32_t> kRemovedPayload = {0x1808, 0x1828};
constexpr platform::PerBuild<std::uint32_t> kHeldRequests   = {0x1770, 0x1790};

constexpr std::uint8_t kAddedBytes[]   = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x8D,
                                          0xAC, 0x24, 0x20, 0xFF, 0xFF, 0xFF, 0xB8, 0xE0, 0x01, 0x00, 0x00, 0xE8};
constexpr std::uint8_t kRemovedBytes[] = {0x40, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kAdded{L"NotifyOverride_OnSubjectCameraOverrideAdded_GlobalModule", 0x1423E30C0,
                                        kAddedBytes};
constexpr platform::FunctionFact kRemoved{L"NotifyOverride_OnSubjectCameraOverrideRemoved_GlobalModule", 0x1423E3730,
                                          kRemovedBytes};

struct Pair  // a TWeakPtr as the global holds it
{
    std::uintptr_t object     = 0;
    std::uintptr_t controller = 0;
    bool operator==(const Pair&) const = default;
};

using AddedFn   = void* (*)(void* out, void* payload, const void* params, const void* a4, const void* a5,
                          const void* a6, const void* a7);
using RemovedFn = void (*)(void* payload, const void* id);
using ReleaseFn = void (*)(std::uintptr_t controller);

// Set once at install.
bool           g_installed     = false;
Pair*          g_global        = nullptr;
std::uintptr_t g_moduleVTable  = 0;
AddedFn        g_engineAdded   = nullptr;
RemovedFn      g_engineRemoved = nullptr;
ReleaseFn      g_releaseWeak   = nullptr;

// Game thread, by seat (index 0, player 1's, unused).
std::array<Pair, players::kMaxPlayers>  g_seatModule{};  // the seat's camera's own module (one weak reference the mod's)
std::array<void*, players::kMaxPlayers> g_member{};      // the member whose delegates hold the thunks

std::size_t Index(int seat) noexcept
{
    return static_cast<std::size_t>(seat);
}

/// The seat whose thunk a delegate called with `payload`, or -1 for none. The game hands each handler the address of
/// the delegate's own storage inside the character, not the character (RE-seats-step6-camera-rumble.md §1): the added
/// delegate's at member+kAddedPayload (which holds the member: Tick checks it), the removed one's at
/// member+kRemovedPayload (which holds nothing the handler reads). So the seat is the one whose member that storage is
/// in. A payload in no seat's member (a delegate the game moved whole to another character keeps the thunk) goes to the
/// one seat with a module, as the two-player mod sent every thunk to player 2's; with several, to none (the engine's
/// handler unswapped).
int ThunkSeat(const void* payload, std::uint32_t offset) noexcept
{
    const auto at    = reinterpret_cast<std::uintptr_t>(payload);
    int        owner = -1, withModule = -1, modules = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto member = reinterpret_cast<std::uintptr_t>(g_member[Index(seat)]);
        if (member != 0 && at == member + offset)
        {
            owner = seat;
        }
        if (g_seatModule[Index(seat)].object != 0)
        {
            withModule = seat;
            ++modules;
        }
    }
    return owner > 0 ? owner : modules == 1 ? withModule : -1;
}

// Game thread: the seat whose module the global holds now (0 none). Seats take turns: a thunk of another seat's member
// inside a held scope calls the engine's handler unswapped (the global is never nested across seats).
int g_globalSeat = 0;

/// Another seat than `seat` holds the global now.
bool HeldByOther(int seat) noexcept
{
    return g_globalSeat != 0 && g_globalSeat != seat;
}

/// Swaps the seat's module into the global for one engine call, with the seat's player as the primary
/// controller, and puts player 1's back after if the global is still the seat's (a Set range of
/// platform/byte_scope.h: a module the game itself wrote inside the call is kept). Its callers check HeldByOther first.
class GlobalScope
{
public:
    explicit GlobalScope(int seat) noexcept : primary_(seat)
    {
        using Range = platform::ByteContext::Range;
        const Range range{reinterpret_cast<std::uintptr_t>(g_global), sizeof(Pair), platform::ByteContext::Kind::Set};
        if (context_.Define(std::span<const Range>(&range, 1)))
        {
            std::memcpy(context_.Value(0).data(), &g_seatModule[Index(seat)], sizeof(Pair));
            held_ = context_.Open();
        }
        if (held_ && g_globalSeat == 0)
        {
            g_globalSeat = seat;  // the outermost scope owns it (the same seat's re-entry passes as before)
            owner_       = true;
        }
    }
    ~GlobalScope()
    {
        if (held_)
        {
            context_.Close();
        }
        if (owner_)
        {
            g_globalSeat = 0;
        }
    }
    GlobalScope(const GlobalScope&)            = delete;
    GlobalScope& operator=(const GlobalScope&) = delete;

private:
    const primary_controller::Scope      primary_;  // opened first, closed last
    platform::ByteStore<1, sizeof(Pair)> context_;
    bool                                 held_  = false;
    bool                                 owner_ = false;
};

// The thunks the member's delegates call (same signatures as the engine's handlers).
void* AddedThunk(void* out, void* payload, const void* params, const void* a4, const void* a5, const void* a6,
                 const void* a7)
{
    const int seat = ThunkSeat(payload, kAddedPayload);
    if (seat < 1 || g_seatModule[Index(seat)].object == 0 || HeldByOther(seat))
    {
        return g_engineAdded(out, payload, params, a4, a5, a6, a7);
    }
    static bool logged[players::kMaxPlayers] = {};
    if (!logged[Index(seat)])
    {
        logged[Index(seat)] = true;  // written before the call, so a failure inside it is placed by the log
        platform::Log(L"camera override: {}'s member's first camera override request, routed to {}'s module",
                      players::SeatName(seat), players::SeatName(seat));
    }
    const GlobalScope scope(seat);
    return g_engineAdded(out, payload, params, a4, a5, a6, a7);
}

void RemovedThunk(void* payload, const void* id)
{
    const int seat = ThunkSeat(payload, kRemovedPayload);
    if (seat < 1 || g_seatModule[Index(seat)].object == 0 || HeldByOther(seat))
    {
        g_engineRemoved(payload, id);
        return;
    }
    const GlobalScope scope(seat);
    g_engineRemoved(payload, id);
}

/// Drops one weak reference, guarded: the controller may belong to an object the game has since
/// freed (then the reference is dropped with it).
void ReleaseGuarded(std::uintptr_t controller) noexcept
{
    __try
    {
        g_releaseWeak(controller);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
    }
}

void Write(void* member, std::uint32_t offset, std::uintptr_t value) noexcept
{
    platform::WriteGuarded(static_cast<std::byte*>(member) + offset, &value, sizeof(value));
}

/// The seat's member's delegates back to the engine's handlers, where they still hold the thunks.
void RestoreMember(int seat) noexcept
{
    void*& member = g_member[Index(seat)];
    if (member == nullptr)
    {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(member);
    if (ReadAt<std::uintptr_t>(base + kAddedFn) == reinterpret_cast<std::uintptr_t>(&AddedThunk))
    {
        Write(member, kAddedFn, reinterpret_cast<std::uintptr_t>(g_engineAdded));
    }
    if (ReadAt<std::uintptr_t>(base + kRemovedFn) == reinterpret_cast<std::uintptr_t>(&RemovedThunk))
    {
        Write(member, kRemovedFn, reinterpret_cast<std::uintptr_t>(g_engineRemoved));
    }
    member = nullptr;
}

#if LSS_DEV
// The snapshot (dev tools, player 2's): both cameras' layer-4 modules and both characters' delegates, logged on change
// (Debug).

std::wstring DescribeCamera(void* controller)
{
    const auto camera = reinterpret_cast<std::uintptr_t>(ReadPointer(controller, facts::kControllerCameraManager));
    if (camera == 0)
    {
        return L"-";
    }
    std::wstring text;
    for (const std::uint32_t op : {kCameraManagerBattleOperator, kFieldOperator})
    {
        const auto oper   = ReadAt<std::uintptr_t>(camera + op);
        const auto module =
            reinterpret_cast<std::uintptr_t>(camera_access::ModuleAt(reinterpret_cast<const void*>(oper), kSlotFour));
        const auto owner  = module != 0 ? ReadAt<std::uintptr_t>(module + kModuleOwner) : 0;
        text += std::format(L" {} module {:#x} owner {} requests {} active {}{}",
                            op == kCameraManagerBattleOperator ? L"battle" : L"field", module,
                            owner == ReadAt<std::uintptr_t>(camera + kCameraManagerBattleOperator) ? L"own"
                                                                                                   : L"OTHER",
                            module != 0 ? ReadAt<std::int32_t>(module + kModuleRequests) : 0,
                            module != 0 ? ReadAt<std::uint8_t>(module + kModuleActive) : 0,
                            module != 0 ? ReadAt<std::uint8_t>(module + kModuleActive + 1) : 0);
    }
    return text;
}

std::wstring DescribeCharacter(std::uintptr_t character)
{
    const auto added   = ReadAt<std::uintptr_t>(character + kAddedFn);
    const auto removed = ReadAt<std::uintptr_t>(character + kRemovedFn);
    const auto name    = [](std::uintptr_t fn, std::uintptr_t engine, std::uintptr_t thunk) {
        return fn == 0 ? L"unbound" : fn == engine ? L"engine" : fn == thunk ? L"thunk" : L"other";
    };
    return std::format(L"added {} removed {} payloads {}/{} held {}",
                       name(added, reinterpret_cast<std::uintptr_t>(g_engineAdded), reinterpret_cast<std::uintptr_t>(&AddedThunk)),
                       name(removed, reinterpret_cast<std::uintptr_t>(g_engineRemoved),
                            reinterpret_cast<std::uintptr_t>(&RemovedThunk)),
                       ReadAt<std::uintptr_t>(character + kAddedPayload) == character ? L"self" : L"other",
                       ReadAt<std::uintptr_t>(character + kRemovedPayload) == character ? L"self" : L"other",
                       ReadAt<std::int32_t>(character + kHeldRequests));
}

/// The raw values the log shows, compared each frame; the text is built only when they change.
struct Snapshot
{
    std::uintptr_t global    = 0;
    std::uintptr_t playerTwo = 0;
    std::uintptr_t cameras[2][2]{};     // each controller's battle / field layer-4 module
    std::uintptr_t owners[2][2]{};
    std::int32_t   requests[2][2]{};
    std::uint16_t  active[2][2]{};
    std::uintptr_t delegates[2][4]{};   // each character's added / removed fn and payloads
    std::int32_t   held[2]{};
    bool operator==(const Snapshot&) const = default;
};

Snapshot Take(void* member) noexcept
{
    Snapshot   now{g_global->object, g_seatModule[players::kPlayerTwoSeat].object};
    void*      controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    const auto characters     = std::array<std::uintptr_t, 2>{
        reinterpret_cast<std::uintptr_t>(ReadPointer(controllers[0], facts::kControllerPawn)), reinterpret_cast<std::uintptr_t>(member)};
    for (int i = 0; i < 2; ++i)
    {
        const auto camera = reinterpret_cast<std::uintptr_t>(ReadPointer(controllers[i], facts::kControllerCameraManager));
        for (int k = 0; k < 2 && camera != 0; ++k)
        {
            const auto oper =
                ReadAt<std::uintptr_t>(camera + (k == 0 ? kCameraManagerBattleOperator : kFieldOperator));
            const auto module = reinterpret_cast<std::uintptr_t>(
                camera_access::ModuleAt(reinterpret_cast<const void*>(oper), kSlotFour));
            now.cameras[i][k]  = module;
            now.owners[i][k]   = module != 0 ? ReadAt<std::uintptr_t>(module + kModuleOwner) : 0;
            now.requests[i][k] = module != 0 ? ReadAt<std::int32_t>(module + kModuleRequests) : 0;
            now.active[i][k]   = module != 0 ? ReadAt<std::uint16_t>(module + kModuleActive) : 0;
        }
        const std::uintptr_t character = characters[i];
        if (character != 0)
        {
            now.delegates[i][0] = ReadAt<std::uintptr_t>(character + kAddedFn);
            now.delegates[i][1] = ReadAt<std::uintptr_t>(character + kRemovedFn);
            now.delegates[i][2] = ReadAt<std::uintptr_t>(character + kAddedPayload);
            now.delegates[i][3] = ReadAt<std::uintptr_t>(character + kRemovedPayload);
            now.held[i]         = ReadAt<std::int32_t>(character + kHeldRequests);
        }
    }
    return now;
}

Snapshot g_snapshot;  // game thread: as last logged

void Log(void* member)
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_snapshot = Snapshot{};  // off: the next line starts fresh (as at start)
        return;
    }
    const Snapshot now = Take(member);
    if (now == g_snapshot)
    {
        return;
    }
    g_snapshot                 = now;
    void*      playerOne = generation::PlayerOneController();
    const auto oneChar   = reinterpret_cast<std::uintptr_t>(ReadPointer(playerOne, facts::kControllerPawn));
    platform::LogDebug(L"camera override: global {:#x} P2's {:#x} | P1 camera{} | P2 camera{} | P1 {} | P2 {}", g_global->object,
                  g_seatModule[players::kPlayerTwoSeat].object, DescribeCamera(playerOne),
                  DescribeCamera(generation::PlayerTwoController()),
                  DescribeCharacter(oneChar), DescribeCharacter(reinterpret_cast<std::uintptr_t>(member)));
}
#else
void Log(void*) {}
#endif

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto global  = image.Rebase(kModuleGlobal, sizeof(Pair));
    const auto vtable  = image.Rebase(kModuleVTable);
    const auto added   = platform::FindFunction(image, kAdded);
    const auto removed = platform::FindFunction(image, kRemoved);
    const auto release = platform::FindFunction(image, facts::kReleaseWeakReference);
    g_installed = global && vtable && added && removed && release && primary_controller::Installed();
    if (g_installed)
    {
        g_global        = reinterpret_cast<Pair*>(*global);
        g_moduleVTable  = *vtable;
        g_engineAdded   = reinterpret_cast<AddedFn>(*added);
        g_engineRemoved = reinterpret_cast<RemovedFn>(*removed);
        g_releaseWeak   = reinterpret_cast<ReleaseFn>(*release);
    }
    platform::Log(L"camera override: {}", g_installed ? L"ready (player 2's camera gets its own NotifyOverride module)"
                                                     : L"UNAVAILABLE (a check failed); the players' cameras share one");
    return g_installed;
}

void BeginPlayWithOwnModule(int seat, void (*engineBeginPlay)(void*), void* cameraManager)
{
    if (!g_installed)
    {
        engineBeginPlay(cameraManager);
        return;
    }
    const bool seated = players::SeatBuilt(seat);
    if (seated && g_seatModule[Index(seat)].controller != 0)
    {
        ReleaseGuarded(g_seatModule[Index(seat)].controller);  // a module left from an earlier join
        g_seatModule[Index(seat)] = {};
    }
    const Pair saved = *g_global;
    Pair       built;  // what the joining camera's operators wrote to the global (trivially destructible: SEH allows it)
    bool       own   = false;
    *g_global        = {};
    __try
    {
        engineBeginPlay(cameraManager);
    }
    __finally
    {
        // The joining camera's operators built their own module and wrote it to the global: it is the seat's now
        // (with the weak reference the global took), and player 1's goes back. For a camera of no seat's the mod
        // drops the global's weak reference: the module lives on with the operators that hold it.
        built     = *g_global;
        *g_global = saved;
        own       = built.object != 0 && ReadAt<std::uintptr_t>(built.object) == g_moduleVTable;
        if (seated && own)
        {
            g_seatModule[Index(seat)] = built;
        }
        else if (built.controller != 0)
        {
            ReleaseGuarded(built.controller);
        }
    }
    if (!seated)
    {
        platform::LogDebug(L"camera override: a camera of no seat's (the mirror view) built NotifyOverride module {:#x} "
                           L"({}); the mod keeps no reference to it, player 1's {:#x} is the global again",
                           built.object, own ? L"the module's own class" : L"not the module's class", saved.object);
        return;
    }
    platform::Log(L"camera override: {}'s camera {} its own NotifyOverride module {:#x} (player 1's {:#x})",
                  players::SeatName(seat), own ? L"built" : L"did NOT build", built.object, saved.object);
}

void Tick(int seat, void* member) noexcept
{
    if (!g_installed || !generation::OnGameThread() || !players::SeatBuilt(seat))
    {
        return;
    }
    if (member != g_member[Index(seat)])
    {
        RestoreMember(seat);  // a different member (or none): the old one's delegates back to the engine's
    }
    if (member != nullptr && g_seatModule[Index(seat)].object != 0)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(member);
        // The camera manager that follows the member binds these; a rebind puts the engine's back,
        // and this puts the thunks on again.
        if (ReadAt<std::uintptr_t>(base + kAddedFn) == reinterpret_cast<std::uintptr_t>(g_engineAdded) &&
            ReadAt<std::uintptr_t>(base + kAddedPayload) == base)
        {
            Write(member, kAddedFn, reinterpret_cast<std::uintptr_t>(&AddedThunk));
            g_member[Index(seat)] = member;
        }
        if (ReadAt<std::uintptr_t>(base + kRemovedFn) == reinterpret_cast<std::uintptr_t>(g_engineRemoved))
        {
            Write(member, kRemovedFn, reinterpret_cast<std::uintptr_t>(&RemovedThunk));
            g_member[Index(seat)] = member;
        }
    }
    if (seat != players::kPlayerTwoSeat)
    {
        return;  // the snapshot is a dev tool, player 2's
    }
    try
    {
        Log(member);
    }
    catch (...)
    {
    }
}

void MemberDestroyed(int seat) noexcept
{
    if (players::SeatBuilt(seat))
    {
        g_member[Index(seat)] = nullptr;
    }
}

void PlayerTwoOut(int seat) noexcept
{
    if (!players::SeatBuilt(seat))
    {
        return;
    }
    RestoreMember(seat);
    Pair& module = g_seatModule[Index(seat)];
    if (module.controller != 0 && g_releaseWeak != nullptr)
    {
        ReleaseGuarded(module.controller);
    }
    module = {};
}

}  // namespace lss::player_two::camera_override
