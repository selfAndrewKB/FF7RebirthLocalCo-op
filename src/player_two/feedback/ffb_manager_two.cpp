#include "player_two/feedback/ffb_manager_two.h"

#include "facts/engine.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/session/own_controller.h"
#include "players/unreal_access.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace lss::player_two::ffb_manager_two {

namespace {

// 1.004 addresses; 1.005 through replaced-ffb-manager.json (every fact read from both exes, 2026-10-12).
constexpr std::uintptr_t kGlobal       = 0x1490FEE18;  // g_EndFFBMgr: the singleton EndFFBMgr_Get makes and returns
constexpr std::uint8_t   kCtorBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kCtor{L"EndFFBMgr_Ctor", 0x1437DBAE0, kCtorBytes};  // EndFFBMgr_Ctor(memory) -> memory
// The component constructor's only call, in the AEndPlayerController constructor. The manager's constructor loads its
// effect list with FObjectFinder, which only a constructor may use: player 2's manager is made inside this call.
constexpr std::uintptr_t kCompCtorCall = 0x143D05AB7;
constexpr std::uintptr_t kCompCtor     = 0x1437DBC78;  // EndFFBComp_Ctor_PC758(component, owner) -> component
constexpr std::uint8_t   kBuildTracksBytes[] = {0x48, 0x89, 0x5C, 0x24};
// EndFFBMgr_BuildTracks90_FFBTr0to15(mgr): FNames only, no loads
constexpr platform::FunctionFact kBuildTracks{L"EndFFBMgr_BuildTracks90_FFBTr0to15", 0x1437E0568, kBuildTracksBytes};
constexpr std::uint8_t   kTickBytes[]  = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00};
// EndFFBMgr_Tick(mgr, dt xmm1): its state machine
constexpr platform::FunctionFact kTick{L"EndFFBMgr_Tick", 0x140C91268, kTickBytes};
constexpr std::uint8_t   kRequestBytes[] = {0x33, 0xC0, 0x4C, 0x8B, 0xC9, 0x48, 0x89, 0x41, 0x20, 0x39, 0x41, 0x08};
// EndStateMachine_RequestStateById(machine, id) -> found
constexpr platform::FunctionFact kRequest{L"EndStateMachine_RequestStateById", 0x140A0BE80, kRequestBytes};
constexpr std::uint8_t   kEntryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8],rbx (crosses a block: 1.005 MuteAll)
                                          0x48, 0x89, 0x6C, 0x24, 0x10};  // mov [rsp+10h],rbp
// EndFFBMgr_MuteAll_PrimaryPC(mgr, mute): fades, menus, states
constexpr platform::FunctionFact kMuteAll{L"EndFFBMgr_MuteAll_PrimaryPC", 0x1411F6AA4, kEntryBytes};
// EndFFBMgr_SetEnabled531(mgr, on): the Vibration option
constexpr platform::FunctionFact kSetEnabled{L"EndFFBMgr_SetEnabled531", 0x1422DE548, kEntryBytes};
constexpr std::size_t    kFirstInstruction  = 5;
constexpr std::size_t    kSecondInstruction = 5;
// The registry's TArray<component*> helpers, as BeginPlay (AddUnique, inline) and the component's destructor use them.
constexpr std::uint8_t   kContainsBytes[] = {0x4C, 0x8B, 0x01, 0x48, 0x63, 0x41, 0x08, 0x49, 0x8D, 0x0C, 0xC0, 0x4C};
// Contains(array, &element) -> bool
constexpr platform::FunctionFact kContains{L"TArray Contains(array, &element)", 0x140786200, kContainsBytes};
constexpr std::uint8_t   kGrowBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56};
// grow after Num++ (array); a first allocation holds 4
constexpr platform::FunctionFact kGrow{L"TArray grow after Num++", 0x1420DE464, kGrowBytes};
constexpr std::uint8_t   kRemoveAllBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x54, 0x41, 0x55};
// RemoveAll(array, &&element) -> removed
constexpr platform::FunctionFact kRemoveAll{L"TArray RemoveAll(array, &&element)", 0x1420DD440, kRemoveAllBytes};

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

// Game thread.
std::atomic<void*> g_two{nullptr};      // player 2's manager, made once and kept for the session
void*              g_attached = nullptr;  // the controller whose component names player 2's manager (null: none)
bool               g_failed   = false;    // making player 2's manager failed: not tried again
bool               g_settled  = false;    // player 2's manager has run the game's state 1 (outside the constructor)
bool               g_noneLogged = false;  // player 2's controller was made outside a join: logged once
bool               g_waitLogged = false;  // waiting for player 2's controller's BeginPlay, logged once
bool               g_strangeLogged = false;
bool               g_offThreadLogged = false;
unsigned           g_optionMirrors = 0;
unsigned           g_muteMirrors   = 0;

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

/// Player 2's manager, made as the game's getter makes its own (inside a controller's constructor: the manager's
/// constructor finds its effects with FObjectFinder), with its 16 tracks built at once by the game's builder, so a
/// play never meets an empty track array. Its state 1 (the resident sounds, the ready flag) runs on the next tick.
void* MakeInConstructor()
{
    const auto  start  = std::chrono::steady_clock::now();
    void* const memory = g_malloc(kManagerSize);
    void* const two    = memory != nullptr ? g_ctor(memory) : nullptr;
    if (two == nullptr)
    {
        platform::Log(L"WARNING: ffb manager two: player 2's force-feedback manager could not be made; both pads keep "
                      L"the game's one manager");
        return nullptr;
    }
    g_buildTracks(two);
    const bool built = Count(two, kTracks) == kTrackCount;
    platform::Log(L"ffb manager two: player 2's force-feedback manager made at 0x{:X} inside player 2's controller's "
                  L"constructor ({} us): {} effects (the game's {}), {} tracks, PS5 haptic mode {}{}",
                  reinterpret_cast<std::uintptr_t>(two), Microseconds(std::chrono::steady_clock::now() - start),
                  Count(two, kEffects), Count(Global(), kEffects), Count(two, kTracks),
                  ReadField<std::uint8_t>(two, kDeviceMode),
                  built ? L"" : L"; TRACKS NOT BUILT: player 2 stays on the game's manager");
    return built ? two : nullptr;
}

/// Player 2's manager runs the game's state 1 (outside every constructor, as the game's own runs at map loads): the
/// resident sounds, the tracks it lacks (none), the ready flag, then state 2.
void Settle(void* two)
{
    g_settled        = true;
    const auto start = std::chrono::steady_clock::now();
    const bool asked = g_request(static_cast<std::byte*>(two) + kMachine, kLoadState);
    g_tick(two, 0.0f);  // enters state 1 and runs its tick, which requests state 2
    g_tick(two, 0.0f);  // enters state 2, where the game's manager rests
    platform::Log(L"ffb manager two: player 2's manager ran the game's state 1 ({} us): asked {}, state {}, ready {}, "
                  L"{} tracks",
                  Microseconds(std::chrono::steady_clock::now() - start), asked, StateOf(two),
                  ReadField<std::uint8_t>(two, kReady), Count(two, kTracks));
    platform::LogDebug(L"ffb manager two: player 2's tracks: {}", TrackNames(two));
}

/// The component constructor, inside the AEndPlayerController constructor. For the controller a join's CreatePlayer
/// constructs (player 2's), the component takes player 2's manager before BeginPlay, which then registers it there and
/// sets its Vibration flag, as it does for player 1's on the game's manager.
void* CompCtor(void* component, void* owner) noexcept
{
    void* const result = reinterpret_cast<CompCtorFn>(g_compCtorCall.Original())(component, owner);
    void* const one    = own_controller::JoiningPlayerOne();
    if (one == nullptr || owner == one || g_failed || !generation::OnGameThread())
    {
        return result;
    }
    try
    {
        void* two = g_two.load(std::memory_order_relaxed);
        if (two == nullptr)
        {
            two = MakeInConstructor();
            if (two == nullptr)
            {
                g_failed = true;
                return result;
            }
            g_two.store(two, std::memory_order_release);
        }
        WriteField<void*>(component, kManager, two);  // the field the constructor just set to the game's manager
        platform::LogDebug(L"ffb manager two: player 2's controller 0x{:X} is constructed: its component takes player "
                           L"2's manager",
                           reinterpret_cast<std::uintptr_t>(owner));
    }
    catch (...)
    {
    }
    return result;
}

/// Notes player 2's component on player 2's manager (game thread, after its BeginPlay). A controller made outside a
/// join that has player 2's manager already is moved onto it the way BeginPlay and the destructor keep the registries.
void Attach(void* controller)
{
    void* const component = static_cast<std::byte*>(controller) + kComponent;
    void* const current   = ReadPointer(component, kManager);
    void* const global    = Global();
    void* const two       = g_two.load(std::memory_order_relaxed);
    if (current != nullptr && current == two)
    {
        void* const registered = component;
        if (!g_contains(Registry(two), &registered))
        {
            return;  // BeginPlay has not run yet
        }
        g_attached = controller;
        platform::Log(L"ffb manager two: player 2's component plays through player 2's own manager (components: the "
                      L"game's {}, player 2's {}; Vibration {})",
                      Count(global, kRegistry), Count(two, kRegistry), ReadField<std::uint8_t>(two, kEnabled));
        return;
    }
    if (two == nullptr)
    {
        if (!g_noneLogged)
        {
            g_noneLogged = true;
            platform::Log(L"ffb manager two: player 2's controller was not constructed by a join here; it shares the "
                          L"game's manager (no shared-ride copy)");
        }
        return;
    }
    if (global == nullptr || current != global)
    {
        if (!g_strangeLogged)
        {
            g_strangeLogged = true;
            platform::Log(L"ffb manager two: player 2's component names manager 0x{:X}, not the game's (0x{:X}); left "
                          L"alone",
                          reinterpret_cast<std::uintptr_t>(current), reinterpret_cast<std::uintptr_t>(global));
        }
        return;
    }
    void* const registered = component;
    if (Count(global, kTracks) != kTrackCount || !g_contains(Registry(global), &registered))
    {
        if (!g_waitLogged)
        {
            g_waitLogged = true;
            platform::LogDebug(L"ffb manager two: waiting for player 2's controller's BeginPlay (the game's manager "
                               L"has {} tracks, {} components)",
                               Count(global, kTracks), Count(global, kRegistry));
        }
        return;
    }
    RemoveFrom(global, component);
    WriteField<void*>(component, kManager, two);
    AddUnique(two, component);
    SetEnabled()(two, ReadField<std::uint8_t>(global, kEnabled));  // as BeginPlay sets it: resets player 2's component
    g_attached = controller;
    platform::Log(L"ffb manager two: player 2's component moved onto player 2's own manager (components: the game's "
                  L"{}, player 2's {}; Vibration {})",
                  Count(global, kRegistry), Count(two, kRegistry), ReadField<std::uint8_t>(two, kEnabled));
}

/// Player 2's component back on the game's manager, as BeginPlay left it (game thread).
void Detach(const wchar_t* why)
{
    void* const controller = g_attached;
    g_attached             = nullptr;
    if (controller == nullptr)
    {
        return;
    }
    void* const two = g_two.load(std::memory_order_relaxed);
    if (controller != generation::PlayerTwoController())
    {
        // The controller cannot be shown to be live: its component, if it is ever destroyed, leaves player 2's
        // manager by its own destructor; if it lives on, the next join finds it attached.
        platform::Log(L"ffb manager two: {}: player 2's controller is no longer the generation's; its component is "
                      L"left on player 2's manager",
                      why);
        return;
    }
    void* const component = static_cast<std::byte*>(controller) + kComponent;
    void* const global    = Global();
    if (global == nullptr || ReadPointer(component, kManager) != two)
    {
        return;
    }
    RemoveFrom(two, component);
    WriteField<void*>(component, kManager, global);
    AddUnique(global, component);
    platform::Log(L"ffb manager two: {}: player 2's component is back on the game's manager (components: the game's {}, "
                  L"player 2's {}; option mirrors {}, stop-all mirrors {})",
                  why, Count(global, kRegistry), Count(two, kRegistry), g_optionMirrors, g_muteMirrors);
}

/// The Vibration option's setter: on the game's manager, then the same on player 2's.
void SetEnabledHook(void* manager, std::uint8_t on) noexcept
{
    SetEnabled()(manager, on);
    void* const two = g_two.load(std::memory_order_acquire);
    if (two == nullptr || manager != Global())
    {
        return;
    }
    if (!generation::OnGameThread())
    {
        if (!g_offThreadLogged)
        {
            g_offThreadLogged = true;
            platform::Log(L"ffb manager two: the Vibration option was set off the game thread; not repeated on player "
                          L"2's manager");
        }
        return;
    }
    SetEnabled()(two, on);
    ++g_optionMirrors;
    platform::LogDebug(L"ffb manager two: Vibration {} on the game's manager, repeated on player 2's", on);
}

/// The stop-all (fades, menus, state changes): the game's on the primary controller, then player 2's manager's with
/// player 2 as the primary player, while player 2's component is on it.
void MuteAllHook(void* manager, std::uint8_t mute) noexcept
{
    MuteAll()(manager, mute);
    void* const two = g_two.load(std::memory_order_acquire);
    if (two == nullptr || manager != Global() || !generation::OnGameThread() || g_attached == nullptr ||
        !generation::Active() || Count(two, kRegistry) == 0)
    {
        return;
    }
    const primary_controller::Scope primary(players::kPlayerTwoSeat);
    if (!primary.Held())
    {
        return;
    }
    MuteAll()(two, mute);
    ++g_muteMirrors;
    platform::LogDebug(L"ffb manager two: stop-all ({}) repeated on player 2's controller", mute ? L"mute" : L"unmute");
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
    const auto bytes = fact.entryBytes;
    const auto at    = platform::FindFunction(image, fact);
    if (!at)
    {
        return false;
    }
    return redirect.Install(*at, image.EntryInstruction(fact.address, bytes.first(kFirstInstruction)), replacement) ||
           redirect.InstallAtSecondInstruction(*at, bytes.first(kFirstInstruction),
                                               bytes.subspan(kFirstInstruction, kSecondInstruction), replacement);
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
    // The option and the stop-all first: a component on player 2's manager must never miss them.
    g_installed = functions && site && ctor &&
                  Entry(image, g_setEnabledEntry, kSetEnabled, reinterpret_cast<std::uintptr_t>(&SetEnabledHook)) &&
                  Entry(image, g_muteAllEntry, kMuteAll, reinterpret_cast<std::uintptr_t>(&MuteAllHook)) &&
                  g_compCtorCall.Install(*site, *ctor, reinterpret_cast<std::uintptr_t>(&CompCtor));
    if (!g_installed)
    {
        RemoveEntries();
    }
    platform::Log(L"ffb manager two: {}",
                  g_installed ? L"player 2's controller gets its own force-feedback manager at join (the Vibration option "
                                L"and the stop-all repeated on it)"
                              : L"UNCHANGED, both controllers share the game's force-feedback manager (a function is not "
                                L"what the research read)");
}

void Remove() noexcept
{
    if (g_installed && generation::OnGameThread())
    {
        try
        {
            Detach(L"the mod is removed");
        }
        catch (...)
        {
        }
    }
    else if (g_installed && g_attached != nullptr)
    {
        platform::Log(L"WARNING: ffb manager two: removed off the game thread; player 2's component stays on player 2's "
                      L"manager (the Vibration option and the stop-all no longer reach it)");
    }
    g_installed = false;
    RemoveEntries();
}

void Tick() noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    try
    {
        void* const two = g_two.load(std::memory_order_relaxed);
        if (two != nullptr && !g_settled)
        {
            Settle(two);  // the tick after the constructor made it
        }
        void* const controller = generation::Active() ? generation::PlayerTwoController() : nullptr;
        if (controller == nullptr)
        {
            g_attached   = nullptr;  // the generation is gone: see Detach for what is left
            g_waitLogged = false;
            return;
        }
        if (g_attached != controller && !g_failed)
        {
            Attach(controller);
        }
    }
    catch (...)
    {
    }
}

void PlayerTwoOut() noexcept
{
    if (g_installed && generation::OnGameThread())
    {
        try
        {
            Detach(L"player 2 leaves");
        }
        catch (...)
        {
        }
    }
}

bool Attached() noexcept
{
    return g_attached != nullptr;
}

}  // namespace lss::player_two::ffb_manager_two
