#include "player_two/camera_override.h"

#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

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
constexpr std::uint32_t  kModules        = 0x6E0;        // an operator's module table (TSharedPtr, 16 bytes each)
constexpr std::uint32_t  kSlotFour       = 4 * 16;       // layer 4, NotifyOverride
constexpr std::uint32_t  kModuleOwner    = 0x50;         // the owner operator
constexpr std::uint32_t  kModuleActive   = 0x44;         // entering / active bytes
constexpr std::uint32_t  kModuleRequests = 0x3F8;        // int32: request count
constexpr std::uint32_t  kBattleOperator = 0x2CA8;       // operator 11
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
constexpr std::uint8_t kReleaseBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x26, 0x74, 0x82, 0x01, 0x48,
                                          0x2B, 0xE0, 0x83, 0xC8, 0xFF, 0xF0, 0x0F, 0xC1, 0x41, 0x0C};  // lock xadd [rcx+0Ch], -1
constexpr platform::FunctionFact kAdded{L"NotifyOverride_OnSubjectCameraOverrideAdded_GlobalModule", 0x1423E30C0,
                                        kAddedBytes};
constexpr platform::FunctionFact kRemoved{L"NotifyOverride_OnSubjectCameraOverrideRemoved_GlobalModule", 0x1423E3730,
                                          kRemovedBytes};
constexpr platform::FunctionFact kReleaseWeak{L"TRefCtrlTS__ReleaseWeakRef", 0x140887B00, kReleaseBytes};

constexpr int kMaxLines = 300;

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

// Game thread.
Pair         g_playerTwo;          // player 2's camera's own module (one weak reference is the mod's)
void*        g_member = nullptr;   // the member whose delegates hold the thunks
int          g_lines = 0;

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

/// Swaps player 2's module into the global for one engine call, with player 2 as the primary
/// controller, and puts player 1's back after if the global is still player 2's.
class GlobalScope
{
public:
    GlobalScope() noexcept : saved_(*g_global) { *g_global = g_playerTwo; }
    ~GlobalScope()
    {
        if (*g_global == g_playerTwo)
        {
            *g_global = saved_;
        }
    }
    GlobalScope(const GlobalScope&)            = delete;
    GlobalScope& operator=(const GlobalScope&) = delete;

private:
    const primary_controller::Scope primary_;
    Pair                            saved_;
};

// The thunks the member's delegates call (same signatures as the engine's handlers).
void* AddedThunk(void* out, void* payload, const void* params, const void* a4, const void* a5, const void* a6,
                 const void* a7)
{
    if (g_playerTwo.object == 0)
    {
        return g_engineAdded(out, payload, params, a4, a5, a6, a7);
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;  // written before the call, so a failure inside it is placed by the log
        platform::Log(L"camera override: player 2's member's first camera override request, routed to player 2's module");
    }
    const GlobalScope scope;
    return g_engineAdded(out, payload, params, a4, a5, a6, a7);
}

void RemovedThunk(void* payload, const void* id)
{
    if (g_playerTwo.object == 0)
    {
        g_engineRemoved(payload, id);
        return;
    }
    const GlobalScope scope;
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

/// The member's delegates back to the engine's handlers, where they still hold the thunks.
void RestoreMember() noexcept
{
    if (g_member == nullptr)
    {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(g_member);
    if (Field<std::uintptr_t>(base + kAddedFn) == reinterpret_cast<std::uintptr_t>(&AddedThunk))
    {
        Write(g_member, kAddedFn, reinterpret_cast<std::uintptr_t>(g_engineAdded));
    }
    if (Field<std::uintptr_t>(base + kRemovedFn) == reinterpret_cast<std::uintptr_t>(&RemovedThunk))
    {
        Write(g_member, kRemovedFn, reinterpret_cast<std::uintptr_t>(g_engineRemoved));
    }
    g_member = nullptr;
}

std::wstring DescribeCamera(void* controller)
{
    const auto camera = reinterpret_cast<std::uintptr_t>(ReadPointer(controller, facts::kControllerCameraManager));
    if (camera == 0)
    {
        return L"-";
    }
    std::wstring text;
    for (const std::uint32_t op : {kBattleOperator, kFieldOperator})
    {
        const auto oper   = Field<std::uintptr_t>(camera + op);
        const auto slots  = oper != 0 ? Field<std::uintptr_t>(oper + kModules) : 0;
        const auto module = slots != 0 ? Field<std::uintptr_t>(slots + kSlotFour) : 0;
        const auto owner  = module != 0 ? Field<std::uintptr_t>(module + kModuleOwner) : 0;
        text += std::format(L" {} module {:#x} owner {} requests {} active {}{}", op == kBattleOperator ? L"battle" : L"field",
                            module, owner == Field<std::uintptr_t>(camera + kBattleOperator) ? L"own" : L"OTHER",
                            module != 0 ? Field<std::int32_t>(module + kModuleRequests) : 0,
                            module != 0 ? Field<std::uint8_t>(module + kModuleActive) : 0,
                            module != 0 ? Field<std::uint8_t>(module + kModuleActive + 1) : 0);
    }
    return text;
}

std::wstring DescribeCharacter(std::uintptr_t character)
{
    const auto added   = Field<std::uintptr_t>(character + kAddedFn);
    const auto removed = Field<std::uintptr_t>(character + kRemovedFn);
    const auto name    = [](std::uintptr_t fn, std::uintptr_t engine, std::uintptr_t thunk) {
        return fn == 0 ? L"unbound" : fn == engine ? L"engine" : fn == thunk ? L"thunk" : L"other";
    };
    return std::format(L"added {} removed {} payloads {}/{} held {}",
                       name(added, reinterpret_cast<std::uintptr_t>(g_engineAdded), reinterpret_cast<std::uintptr_t>(&AddedThunk)),
                       name(removed, reinterpret_cast<std::uintptr_t>(g_engineRemoved),
                            reinterpret_cast<std::uintptr_t>(&RemovedThunk)),
                       Field<std::uintptr_t>(character + kAddedPayload) == character ? L"self" : L"other",
                       Field<std::uintptr_t>(character + kRemovedPayload) == character ? L"self" : L"other",
                       Field<std::int32_t>(character + kHeldRequests));
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
    Snapshot   now{g_global->object, g_playerTwo.object};
    void*      controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    const auto characters     = std::array<std::uintptr_t, 2>{
        reinterpret_cast<std::uintptr_t>(ReadPointer(controllers[0], facts::kControllerPawn)), reinterpret_cast<std::uintptr_t>(member)};
    for (int i = 0; i < 2; ++i)
    {
        const auto camera = reinterpret_cast<std::uintptr_t>(ReadPointer(controllers[i], facts::kControllerCameraManager));
        for (int k = 0; k < 2 && camera != 0; ++k)
        {
            const auto oper   = Field<std::uintptr_t>(camera + (k == 0 ? kBattleOperator : kFieldOperator));
            const auto slots  = oper != 0 ? Field<std::uintptr_t>(oper + kModules) : 0;
            const auto module = slots != 0 ? Field<std::uintptr_t>(slots + kSlotFour) : 0;
            now.cameras[i][k]  = module;
            now.owners[i][k]   = module != 0 ? Field<std::uintptr_t>(module + kModuleOwner) : 0;
            now.requests[i][k] = module != 0 ? Field<std::int32_t>(module + kModuleRequests) : 0;
            now.active[i][k]   = module != 0 ? Field<std::uint16_t>(module + kModuleActive) : 0;
        }
        const std::uintptr_t character = characters[i];
        if (character != 0)
        {
            now.delegates[i][0] = Field<std::uintptr_t>(character + kAddedFn);
            now.delegates[i][1] = Field<std::uintptr_t>(character + kRemovedFn);
            now.delegates[i][2] = Field<std::uintptr_t>(character + kAddedPayload);
            now.delegates[i][3] = Field<std::uintptr_t>(character + kRemovedPayload);
            now.held[i]         = Field<std::int32_t>(character + kHeldRequests);
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
    if (g_lines >= kMaxLines)
    {
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
    ++g_lines;
    platform::LogDebug(L"camera override: global {:#x} P2's {:#x} | P1 camera{} | P2 camera{} | P1 {} | P2 {}", g_global->object,
                  g_playerTwo.object, DescribeCamera(playerOne), DescribeCamera(generation::PlayerTwoController()),
                  DescribeCharacter(oneChar), DescribeCharacter(reinterpret_cast<std::uintptr_t>(member)));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto global  = image.Rebase(kModuleGlobal, sizeof(Pair));
    const auto vtable  = image.Rebase(kModuleVTable);
    const auto added   = platform::FindFunction(image, kAdded);
    const auto removed = platform::FindFunction(image, kRemoved);
    const auto release = platform::FindFunction(image, kReleaseWeak);
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

void BeginPlayWithOwnModule(void (*engineBeginPlay)(void*), void* cameraManager)
{
    if (!g_installed)
    {
        engineBeginPlay(cameraManager);
        return;
    }
    if (g_playerTwo.controller != 0)
    {
        ReleaseGuarded(g_playerTwo.controller);  // a module left from an earlier join
        g_playerTwo = {};
    }
    const Pair saved = *g_global;
    *g_global        = {};
    __try
    {
        engineBeginPlay(cameraManager);
    }
    __finally
    {
        // Player 2's operators built their own module and wrote it to the global: it is player 2's
        // now (with the weak reference the global took), and player 1's goes back.
        const Pair built = *g_global;
        *g_global        = saved;
        const bool own   = built.object != 0 && Field<std::uintptr_t>(built.object) == g_moduleVTable;
        if (own)
        {
            g_playerTwo = built;
        }
        else if (built.controller != 0)
        {
            ReleaseGuarded(built.controller);
        }
    }
    platform::Log(L"camera override: player 2's camera {} its own NotifyOverride module {:#x} (player 1's {:#x})",
                  g_playerTwo.object != 0 ? L"built" : L"did NOT build", g_playerTwo.object, saved.object);
}

void Tick(void* member) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    if (member != g_member)
    {
        RestoreMember();  // a different member (or none): the old one's delegates back to the engine's
    }
    if (member != nullptr && g_playerTwo.object != 0)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(member);
        // The camera manager that follows the member binds these; a rebind puts the engine's back,
        // and this puts the thunks on again.
        if (Field<std::uintptr_t>(base + kAddedFn) == reinterpret_cast<std::uintptr_t>(g_engineAdded) &&
            Field<std::uintptr_t>(base + kAddedPayload) == base)
        {
            Write(member, kAddedFn, reinterpret_cast<std::uintptr_t>(&AddedThunk));
            g_member = member;
        }
        if (Field<std::uintptr_t>(base + kRemovedFn) == reinterpret_cast<std::uintptr_t>(g_engineRemoved))
        {
            Write(member, kRemovedFn, reinterpret_cast<std::uintptr_t>(&RemovedThunk));
            g_member = member;
        }
    }
    try
    {
        Log(member);
    }
    catch (...)
    {
    }
}

void MemberDestroyed() noexcept
{
    g_member = nullptr;
}

void PlayerTwoOut() noexcept
{
    RestoreMember();
    if (g_playerTwo.controller != 0 && g_releaseWeak != nullptr)
    {
        ReleaseGuarded(g_playerTwo.controller);
    }
    g_playerTwo = {};
}

}  // namespace lss::player_two::camera_override
