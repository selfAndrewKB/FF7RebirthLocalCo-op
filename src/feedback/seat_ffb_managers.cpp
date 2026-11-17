#include "feedback/seat_ffb_managers.h"

#include "facts/engine.h"
#include "platform/call_redirect.h"
#include "platform/entry_install.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/primary_controller.h"
#include "session/own_controller.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::feedback::seat_ffb_managers {

namespace {

// 1.004 addresses; 1.005 through replaced-ffb-manager.json (every fact read from both exes).
constexpr std::uintptr_t kGlobal       = 0x148F325E8;  // g_EndFFBMgr: the singleton EndFFBMgr_Get makes and returns
constexpr std::uint8_t   kCtorBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kCtor{L"EndFFBMgr_Ctor", 0x143516D84, kCtorBytes};  // EndFFBMgr_Ctor(memory) -> memory
// The component constructor's only call, in the AEndPlayerController constructor. The manager's constructor loads its
// effect list with FObjectFinder, which only a constructor may use: a seat's manager is made inside this call.
constexpr std::uintptr_t kCompCtorCall = 0x143A674D7;
constexpr std::uintptr_t kCompCtor     = 0x143516F1C;  // EndFFBComp_Ctor_PC758(component, owner) -> component
constexpr std::uint8_t   kBuildTracksBytes[] = {0x48, 0x89, 0x5C, 0x24};
// EndFFBMgr_BuildTracks90_FFBTr0to15(mgr): FNames only, no loads
constexpr platform::FunctionFact kBuildTracks{L"EndFFBMgr_BuildTracks90_FFBTr0to15", 0x14351B8B8, kBuildTracksBytes};
constexpr std::uint8_t   kTickBytes[]  = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00};
// EndFFBMgr_Tick(mgr, dt xmm1): its state machine
constexpr platform::FunctionFact kTick{L"EndFFBMgr_Tick", 0x14097236C, kTickBytes};
constexpr std::uint8_t   kRequestBytes[] = {0x33, 0xC0, 0x4C, 0x8B, 0xC9, 0x48, 0x89, 0x41, 0x20, 0x39, 0x41, 0x08};
// EndStateMachine_RequestStateById(machine, id) -> found
constexpr platform::FunctionFact kRequest{L"EndStateMachine_RequestStateById", 0x140C9A160, kRequestBytes};
constexpr std::uint8_t   kEntryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8],rbx (crosses a block: 1.005 MuteAll)
                                          0x48, 0x89, 0x6C, 0x24, 0x10};  // mov [rsp+10h],rbp
// EndFFBMgr_MuteAll_PrimaryPC(mgr, mute): fades, menus, states
constexpr platform::FunctionFact kMuteAll{L"EndFFBMgr_MuteAll_PrimaryPC", 0x140FE11BC, kEntryBytes};
// EndFFBMgr_SetEnabled531(mgr, on): the Vibration option
constexpr platform::FunctionFact kSetEnabled{L"EndFFBMgr_SetEnabled531", 0x1421E13B8, kEntryBytes};
// The registry's TArray<component*> helpers, as BeginPlay (AddUnique, inline) and the component's destructor use them.
constexpr std::uint8_t   kContainsBytes[] = {0x4C, 0x8B, 0x01, 0x48, 0x63, 0x41, 0x08, 0x49, 0x8D, 0x0C, 0xC0, 0x4C};
// Contains(array, &element) -> bool
constexpr platform::FunctionFact kContains{L"TArray Contains(array, &element)", 0x140A6A1C8, kContainsBytes};
constexpr std::uint8_t   kGrowBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56};
// grow after Num++ (array); a first allocation holds 4
constexpr platform::FunctionFact kGrow{L"TArray grow after Num++", 0x14218D854, kGrowBytes};
constexpr std::uint8_t   kRemoveAllBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89};
// RemoveAll(array, &&element) -> removed
constexpr platform::FunctionFact kRemoveAll{L"TArray RemoveAll(array, &&element)", 0x142192ACC, kRemoveAllBytes};

constexpr std::size_t   kManagerSize  = 0x538;  // the getter's FMemory_Malloc(0x538)
constexpr std::uint32_t kComponent    = 0x758;  // the controller's force-feedback component (embedded)
constexpr std::uint32_t kManager      = 0x8;    // ... its manager
constexpr std::uint32_t kEffects      = 0x28;   // the manager's effect assets (TArray; Num at +8)
constexpr std::uint32_t kTracks       = 0x90;   // ... its 16 tracks (TArray of 64 bytes; Num at +8)
constexpr std::uint32_t kTrackSize    = 0x40;
constexpr std::uint32_t kTrackName    = 0x4;    // ... a track's own name (FFB_TrN)
constexpr std::uint32_t kRegistry     = 0xA0;   // ... its components (TArray<component*>: data, Num +8, Max +0xC)
constexpr std::uint32_t kMachine      = 0x130;  // ... its state machine (current state at +0x18, its id first)
constexpr std::uint32_t kCurrentState = 0x18;
constexpr std::uint32_t kReady        = 0x530;  // ... set by state 1 once the tracks are built
constexpr std::uint32_t kEnabled      = 0x531;  // ... the Vibration option
constexpr std::uint32_t kDeviceMode   = 0x22;   // ... 1 = a PS5 haptic pad (left at the constructor's 0)
constexpr std::uint32_t kArrayNum     = 0x8;
constexpr std::uint32_t kArrayMax     = 0xC;
constexpr int           kTrackCount   = 16;
constexpr int           kLoadState    = 1;      // state 1: load the resident sounds, build the tracks, then state 2

using MallocFn     = void* (*)(std::size_t size);
using CtorFn       = void* (*)(void* memory);
using CompCtorFn   = void* (*)(void* component, void* owner);
using BuildTracksFn = void (*)(void* manager);
using TickFn       = void (*)(void* manager, float dt);  // dt in xmm1
using RequestFn    = bool (*)(void* machine, int id);
using SetEnabledFn = void (*)(void* manager, std::uint8_t on);
using MuteAllFn    = void (*)(void* manager, std::uint8_t mute);
using ContainsFn   = bool (*)(void* array, void* const* element);
using GrowFn       = void (*)(void* array);
using RemoveAllFn  = std::int32_t (*)(void* array, void* const* const* element);

platform::CallRedirect  g_compCtorCall;
platform::EntryRedirect g_setEnabledEntry;
platform::EntryRedirect g_muteAllEntry;
void**                  g_global    = nullptr;  // where the game keeps its manager
MallocFn                g_malloc    = nullptr;
CtorFn                  g_ctor      = nullptr;
BuildTracksFn           g_buildTracks = nullptr;
TickFn                  g_tick      = nullptr;
RequestFn               g_request   = nullptr;
ContainsFn              g_contains  = nullptr;
GrowFn                  g_grow      = nullptr;
RemoveAllFn             g_removeAll = nullptr;
bool                    g_installed = false;

// Game thread. One seat's manager and its state (by seat; index 0, player 1's, unused: player 1 keeps the game's).
struct SeatManager
{
    std::atomic<void*> manager{nullptr};   // the seat's manager, made once and kept for the session
    void*              attached = nullptr;  // the controller whose component names the seat's manager (null: none)
    bool               failed   = false;    // making the seat's manager failed: not tried again
    bool               settled  = false;    // the seat's manager has run the game's state 1 (outside the constructor)
    bool               noneLogged    = false;  // the seat's controller was made outside a join: logged once
    bool               waitLogged    = false;  // waiting for the seat's controller's BeginPlay, logged once
    bool               strangeLogged = false;
};
SeatManager g_seats[players::kMaxPlayers];
bool        g_offThreadLogged = false;
unsigned    g_optionMirrors   = 0;  // every seat's repeats, for the log
unsigned    g_muteMirrors     = 0;

void* Global() noexcept
{
    return g_global != nullptr ? *g_global : nullptr;
}

void* Registry(void* manager) noexcept
{
    return static_cast<std::byte*>(manager) + kRegistry;
}

std::int32_t Count(const void* manager, std::uint32_t array) noexcept
{
    return manager != nullptr ? ReadField<std::int32_t>(manager, array + kArrayNum) : 0;
}

int StateOf(const void* manager) noexcept
{
    const void* const state = ReadPointer(manager, kMachine + kCurrentState);
    return state != nullptr ? ReadField<std::int32_t>(state, 0) : -1;
}

/// The component into the manager's registry, as BeginPlay adds it (Contains, else Num++ and grow, then the slot).
void AddUnique(void* manager, void* component) noexcept
{
    void* const array = Registry(manager);
    if (g_contains(array, &component))
    {
        return;
    }
    const std::int32_t index = ReadField<std::int32_t>(array, kArrayNum);
    WriteField<std::int32_t>(array, kArrayNum, index + 1);
    if (index + 1 > ReadField<std::int32_t>(array, kArrayMax))
    {
        g_grow(array);
    }
    void** const data = static_cast<void**>(ReadPointer(array, 0));
    data[index]       = component;
}

/// The component out of the manager's registry, as the component's destructor removes it.
void RemoveFrom(void* manager, void* component) noexcept
{
    void* const array = Registry(manager);
    if (g_contains(array, &component))
    {
        void* const* const element = &component;
        g_removeAll(array, &element);
    }
}

SetEnabledFn SetEnabled() noexcept
{
    return reinterpret_cast<SetEnabledFn>(g_setEnabledEntry.Original());
}

MuteAllFn MuteAll() noexcept
{
    return reinterpret_cast<MuteAllFn>(g_muteAllEntry.Original());
}

std::wstring TrackNames(const void* manager)
{
    std::wstring text;
    const void* const tracks = ReadPointer(manager, kTracks);
    const int         count  = Count(manager, kTracks);
    for (int i = 0; tracks != nullptr && i < count && i < kTrackCount; ++i)
    {
        const auto name = ReadField<std::uint64_t>(tracks, static_cast<std::uint32_t>(i * kTrackSize + kTrackName));
        text += (i == 0 ? L"" : L", ") + players::unreal::NameText(name);
    }
    return text;
}

std::int64_t Microseconds(std::chrono::steady_clock::duration d) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

/// `seat`'s manager, made as the game's getter makes its own (inside a controller's constructor: the manager's
/// constructor finds its effects with FObjectFinder), with its 16 tracks built at once by the game's builder, so a
/// play never meets an empty track array. Its state 1 (the resident sounds, the ready flag) runs on the next tick.
void* MakeInConstructor(int seat)
{
    const wchar_t* const name    = players::SeatName(seat);
    const auto           start   = std::chrono::steady_clock::now();
    void* const          memory  = g_malloc(kManagerSize);
    void* const          manager = memory != nullptr ? g_ctor(memory) : nullptr;
    if (manager == nullptr)
    {
        platform::Log(L"WARNING: ffb managers: {}'s force-feedback manager could not be made; every pad keeps "
                      L"the game's one manager",
                      name);
        return nullptr;
    }
    g_buildTracks(manager);
    const bool built = Count(manager, kTracks) == kTrackCount;
    platform::Log(L"ffb managers: {}'s force-feedback manager made at 0x{:X} inside {}'s controller's "
                  L"constructor ({} us): {} effects (the game's {}), {} tracks, PS5 haptic mode {}{}",
                  name, reinterpret_cast<std::uintptr_t>(manager), name,
                  Microseconds(std::chrono::steady_clock::now() - start), Count(manager, kEffects),
                  Count(Global(), kEffects), Count(manager, kTracks), ReadField<std::uint8_t>(manager, kDeviceMode),
                  built ? std::wstring() : std::format(L"; TRACKS NOT BUILT: {} stays on the game's manager", name));
    return built ? manager : nullptr;
}

/// `seat`'s manager runs the game's state 1 (outside every constructor, as the game's own runs at map loads): the
/// resident sounds, the tracks it lacks (none), the ready flag, then state 2.
void Settle(int seat, void* manager)
{
    g_seats[seat].settled = true;
    const auto start      = std::chrono::steady_clock::now();
    const bool asked      = g_request(static_cast<std::byte*>(manager) + kMachine, kLoadState);
    g_tick(manager, 0.0f);  // enters state 1 and runs its tick, which requests state 2
    g_tick(manager, 0.0f);  // enters state 2, where the game's manager rests
    platform::Log(L"ffb managers: {}'s manager ran the game's state 1 ({} us): asked {}, state {}, ready {}, "
                  L"{} tracks",
                  players::SeatName(seat), Microseconds(std::chrono::steady_clock::now() - start), asked,
                  StateOf(manager), ReadField<std::uint8_t>(manager, kReady), Count(manager, kTracks));
    platform::LogDebug(L"ffb managers: {}'s tracks: {}", players::SeatName(seat), TrackNames(manager));
}

/// The component constructor, inside the AEndPlayerController constructor. For the controller a join's CreatePlayer
/// constructs (a seat's), the component takes the seat's manager before BeginPlay, which then registers it there and
/// sets its Vibration flag, as it does for player 1's on the game's manager. The seat is the one the join names
/// (session::own_controller::JoiningSeat); a join of no seat's (the mirror view) keeps the game's manager.
void* CompCtor(void* component, void* owner) noexcept
{
    void* const result = reinterpret_cast<CompCtorFn>(g_compCtorCall.Original())(component, owner);
    void* const one    = session::own_controller::JoiningPlayerOne();
    const int   seat   = session::own_controller::JoiningSeat();
    if (one == nullptr || owner == one || !players::SeatBuilt(seat) || !players::generation::OnGameThread())
    {
        return result;
    }
    SeatManager& record = g_seats[seat];
    if (record.failed)
    {
        return result;
    }
    try
    {
        void* manager = record.manager.load(std::memory_order_relaxed);
        if (manager == nullptr)
        {
            manager = MakeInConstructor(seat);
            if (manager == nullptr)
            {
                record.failed = true;
                return result;
            }
            record.manager.store(manager, std::memory_order_release);
        }
        WriteField<void*>(component, kManager, manager);  // the field the constructor just set to the game's manager
        platform::LogDebug(L"ffb managers: {}'s controller 0x{:X} is constructed: its component takes {}'s manager",
                           players::SeatName(seat), reinterpret_cast<std::uintptr_t>(owner), players::SeatName(seat));
    }
    catch (...)
    {
    }
    return result;
}

/// Notes `seat`'s component on the seat's manager (game thread, after its BeginPlay). A controller made outside a join
/// that has the seat's manager already is moved onto it the way BeginPlay and the destructor keep the registries.
void Attach(int seat, void* controller)
{
    SeatManager&         record    = g_seats[seat];
    const wchar_t* const name      = players::SeatName(seat);
    void* const          component = static_cast<std::byte*>(controller) + kComponent;
    void* const          current   = ReadPointer(component, kManager);
    void* const          global    = Global();
    void* const          manager   = record.manager.load(std::memory_order_relaxed);
    if (current != nullptr && current == manager)
    {
        void* const registered = component;
        if (!g_contains(Registry(manager), &registered))
        {
            return;  // BeginPlay has not run yet
        }
        record.attached = controller;
        platform::Log(L"ffb managers: {}'s component plays through {}'s own manager (components: the "
                      L"game's {}, {}'s {}; Vibration {})",
                      name, name, Count(global, kRegistry), name, Count(manager, kRegistry),
                      ReadField<std::uint8_t>(manager, kEnabled));
        return;
    }
    if (manager == nullptr)
    {
        if (!record.noneLogged)
        {
            record.noneLogged = true;
            platform::Log(L"ffb managers: {}'s controller was not constructed by a join here; it shares the "
                          L"game's manager (no shared-ride copy)",
                          name);
        }
        return;
    }
    if (global == nullptr || current != global)
    {
        if (!record.strangeLogged)
        {
            record.strangeLogged = true;
            platform::Log(L"ffb managers: {}'s component names manager 0x{:X}, not the game's (0x{:X}); left "
                          L"alone",
                          name, reinterpret_cast<std::uintptr_t>(current), reinterpret_cast<std::uintptr_t>(global));
        }
        return;
    }
    void* const registered = component;
    if (Count(global, kTracks) != kTrackCount || !g_contains(Registry(global), &registered))
    {
        if (!record.waitLogged)
        {
            record.waitLogged = true;
            platform::LogDebug(L"ffb managers: waiting for {}'s controller's BeginPlay (the game's manager "
                               L"has {} tracks, {} components)",
                               name, Count(global, kTracks), Count(global, kRegistry));
        }
        return;
    }
    RemoveFrom(global, component);
    WriteField<void*>(component, kManager, manager);
    AddUnique(manager, component);
    SetEnabled()(manager, ReadField<std::uint8_t>(global, kEnabled));  // as BeginPlay sets it: resets the seat's
                                                                       // component
    record.attached = controller;
    platform::Log(L"ffb managers: {}'s component moved onto {}'s own manager (components: the game's "
                  L"{}, {}'s {}; Vibration {})",
                  name, name, Count(global, kRegistry), name, Count(manager, kRegistry),
                  ReadField<std::uint8_t>(manager, kEnabled));
}

/// `seat`'s component back on the game's manager, as BeginPlay left it (game thread).
void Detach(int seat, const wchar_t* why)
{
    SeatManager&         record     = g_seats[seat];
    const wchar_t* const name       = players::SeatName(seat);
    void* const          controller = record.attached;
    record.attached                 = nullptr;
    if (controller == nullptr)
    {
        return;
    }
    void* const manager = record.manager.load(std::memory_order_relaxed);
    if (controller != players::generation::ControllerOf(seat))
    {
        // The controller cannot be shown to be live: its component, if it is ever destroyed, leaves the seat's
        // manager by its own destructor; if it lives on, the next join finds it attached.
        platform::Log(L"ffb managers: {}: {}'s controller is no longer the generation's; its component is "
                      L"left on {}'s manager",
                      why, name, name);
        return;
    }
    void* const component = static_cast<std::byte*>(controller) + kComponent;
    void* const global    = Global();
    if (global == nullptr || ReadPointer(component, kManager) != manager)
    {
        return;
    }
    RemoveFrom(manager, component);
    WriteField<void*>(component, kManager, global);
    AddUnique(global, component);
    platform::Log(L"ffb managers: {}: {}'s component is back on the game's manager (components: the game's {}, "
                  L"{}'s {}; option mirrors {}, stop-all mirrors {})",
                  why, name, Count(global, kRegistry), name, Count(manager, kRegistry), g_optionMirrors, g_muteMirrors);
}

/// The Vibration option's setter: on the game's manager, then the same on each seat's, in seat order.
void SetEnabledHook(void* manager, std::uint8_t on) noexcept
{
    SetEnabled()(manager, on);
    if (manager != Global())
    {
        return;
    }
    int first = 0, count = 0;  // the first built seat with a manager, and how many have one
    for (int seat = players::kSeatsBuilt; seat >= 1; --seat)
    {
        const bool has = g_seats[seat].manager.load(std::memory_order_acquire) != nullptr;
        first          = has ? seat : first;
        count += has ? 1 : 0;
    }
    if (first == 0)
    {
        return;
    }
    if (!players::generation::OnGameThread())
    {
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::Log(L"ffb managers: the Vibration option was set off the game thread; not repeated on {}'s "
                          L"manager{}",
                          players::SeatName(first), count > 1 ? L" (nor the other players')" : L"");
        }
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* const theirs = g_seats[seat].manager.load(std::memory_order_acquire);
        if (theirs == nullptr)
        {
            continue;
        }
        SetEnabled()(theirs, on);
        ++g_optionMirrors;
        platform::LogDebug(L"ffb managers: Vibration {} on the game's manager, repeated on {}'s", on,
                           players::SeatName(seat));
    }
}

/// The stop-all (fades, menus, state changes): the game's on the primary controller, then each seat's manager's with
/// the seat as the primary player, while the seat's component is on it; the seats in turn, each scope closed before
/// the next.
void MuteAllHook(void* manager, std::uint8_t mute) noexcept
{
    MuteAll()(manager, mute);
    if (manager != Global() || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* const theirs = g_seats[seat].manager.load(std::memory_order_acquire);
        if (theirs == nullptr || g_seats[seat].attached == nullptr || !players::generation::Active(seat) ||
            Count(theirs, kRegistry) == 0)
        {
            continue;
        }
        const scopes::primary_controller::Scope primary(seat);
        if (!primary.Held())
        {
            continue;
        }
        MuteAll()(theirs, mute);
        ++g_muteMirrors;
        platform::LogDebug(L"ffb managers: stop-all ({}) repeated on {}'s controller", mute ? L"mute" : L"unmute",
                           players::SeatName(seat));
    }
}

template <class Fn>
bool Find(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto at = platform::FindFunction(image, fact);
    out           = at ? reinterpret_cast<Fn>(*at) : nullptr;
    return out != nullptr;
}

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, const platform::FunctionFact& fact,
           std::uintptr_t replacement) noexcept
{
    return platform::InstallAtEntryOrSecond(redirect, image, fact, replacement);
}

void RemoveEntries() noexcept
{
    if (g_compCtorCall.IsInstalled())
    {
        g_compCtorCall.Restore();
    }
    for (platform::EntryRedirect* entry : {&g_muteAllEntry, &g_setEnabledEntry})
    {
        if (entry->IsInstalled())
        {
            entry->Restore();
        }
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto global    = image.Rebase(kGlobal, sizeof(void*));
    g_global             = global ? reinterpret_cast<void**>(*global) : nullptr;
    const bool functions = g_global != nullptr && Find(image, facts::kMalloc, g_malloc) && Find(image, kCtor, g_ctor) &&
                           Find(image, kTick, g_tick) && Find(image, kRequest, g_request) &&
                           Find(image, kContains, g_contains) && Find(image, kGrow, g_grow) &&
                           Find(image, kRemoveAll, g_removeAll) && Find(image, kBuildTracks, g_buildTracks);
    const auto site      = image.Rebase(kCompCtorCall, 5);
    const auto ctor      = image.Rebase(kCompCtor);
    // The option and the stop-all first: a component on a seat's manager must never miss them.
    g_installed = functions && site && ctor &&
                  Entry(image, g_setEnabledEntry, kSetEnabled, reinterpret_cast<std::uintptr_t>(&SetEnabledHook)) &&
                  Entry(image, g_muteAllEntry, kMuteAll, reinterpret_cast<std::uintptr_t>(&MuteAllHook)) &&
                  g_compCtorCall.Install(*site, *ctor, reinterpret_cast<std::uintptr_t>(&CompCtor));
    if (!g_installed)
    {
        RemoveEntries();
    }
    platform::Log(L"ffb managers: {}",
                  g_installed ? L"player 2's and 3's controllers each get their own force-feedback manager at join (the Vibration option "
                                L"and the stop-all repeated on them)"
                              : L"UNCHANGED, every controller shares the game's force-feedback manager (a function is not "
                                L"what the research read)");
}

void Remove() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)  // only a built seat's component is attached
    {
        if (g_installed && players::generation::OnGameThread())
        {
            try
            {
                Detach(seat, L"the mod is removed");
            }
            catch (...)
            {
            }
        }
        else if (g_installed && g_seats[seat].attached != nullptr)
        {
            platform::Log(L"WARNING: ffb managers: removed off the game thread; {}'s component stays on {}'s "
                          L"manager (the Vibration option and the stop-all no longer reach it)",
                          players::SeatName(seat), players::SeatName(seat));
        }
    }
    g_installed = false;
    RemoveEntries();
}

void Tick() noexcept
{
    if (!g_installed || !players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        try
        {
            SeatManager& record  = g_seats[seat];
            void* const  manager = record.manager.load(std::memory_order_relaxed);
            if (manager != nullptr && !record.settled)
            {
                Settle(seat, manager);  // the tick after the constructor made it
            }
            void* const controller = players::generation::Active(seat) ? players::generation::ControllerOf(seat) : nullptr;
            if (controller == nullptr)
            {
                record.attached   = nullptr;  // the generation is gone: see Detach for what is left
                record.waitLogged = false;
                continue;
            }
            if (record.attached != controller && !record.failed)
            {
                Attach(seat, controller);
            }
        }
        catch (...)
        {
        }
    }
}

void SeatOut(int seat) noexcept
{
    if (g_installed && players::generation::OnGameThread() && players::SeatIndex(seat))
    {
        try
        {
            Detach(seat, players::BySeat(seat, L"player 2 leaves", L"player 3 leaves"));
        }
        catch (...)
        {
        }
    }
}

bool Attached(int seat) noexcept
{
    return players::SeatIndex(seat) && g_seats[seat].attached != nullptr;
}

}  // namespace lss::feedback::seat_ffb_managers
