#include "views/whole_screen.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/unreal_access.h"

#include <array>
#include <cstdint>
#include <string>

namespace lss::views {

namespace {

// RE-p2-single-view.md §2 (pass S-SV1) and its S-SV2 verification; this build.
//
// The game state through the game's own getter, which also checks its class (null otherwise): it
// reads g_AEndGameMode 0x1490FF2B8 +0x288, takes no arguments, and changes nothing.
constexpr std::uintptr_t kGetEndGameState      = 0x140A2E4B4;
constexpr std::uint8_t   kGetEndGameStateBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                    0x70, 0x0A, 0x68, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr std::uint32_t  kState    = 0x278;  // uint8: the EndGameState state (+0x279 is its history)
constexpr std::uint32_t  kMenuKind = 0x3BC;  // uint8: EEndSystemPauseMenuKind

// The on/off table: its owner at [[0x1490FEDE8]+0x170] (a native game object the game mode creates
// and frees), two int32 counts per id; an id is on while their sum is not zero.
constexpr std::uintptr_t kGameRoot     = 0x1490FEDE8;
constexpr std::uint32_t  kStateManager = 0x170;
constexpr std::uint32_t  kOnOffCountA  = 0x1A1238C;
constexpr std::uint32_t  kOnOffCountB  = 0x1A1263C;

// EndFieldOnOffTable: ShowMainMenu, ShowShopMenu, ShowMapJournal, ShowSkillTreeMenu,
// ShowMainMenuInPV, ShowMainMenu_CardGameDecks.
constexpr std::array<std::int32_t, 6> kMenuIds{147, 148, 149, 154, 155, 156};
// EndFieldOnOffTable ShowSkipPauseMenu: held by the dialogue ("Dialogue") and camera-planner ("PlannerEvent")
// scenes while they can be skipped (§7.2).
constexpr std::int32_t kSkippableSceneId = 12;

// The job board (§7.1): EndUISystem's slot +0x1E8 (a static) points to the board's 8-byte FWeakObjectPtr;
// the board's script command keeps running while the game's FWeakObjectPtr_IsValid says it is valid.
constexpr std::uintptr_t kJobBoardSlot          = 0x1493498C8;
constexpr std::uintptr_t kWeakIsValid           = 0x1418EB3D4;
constexpr std::uint8_t   kWeakIsValidBytes[]    = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45,
                                                   0x85, 0xC9, 0x74, 0x4F, 0x8B, 0x01, 0x85, 0xC0};

// The game state's current location (an 8-byte FName; +0x30C requested, +0x314 previous) and the
// title's, which the game compares against itself (§7.3).
constexpr std::uint32_t  kLocation  = 0x304;
constexpr const wchar_t* kTitleName = L"0005-TITLB";

// The cutscene manager (a pointer global; the game creates the manager on first use, so the check
// below is called only once it exists) and the game's own "a sequence of type 0 or 2 plays". The
// check takes no arguments, writes nothing, and walks the manager's list on the game thread.
constexpr std::uintptr_t kCutsceneManager     = 0x1490FEB30;
constexpr std::uint32_t  kSkipping            = 0xA60;  // uint8: a cutscene skip under way
constexpr std::uintptr_t kIsAnyPlaying        = 0x140A514AC;
constexpr std::uint8_t   kIsAnyPlayingBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0xDA,
                                                 0x65, 0x01, 0x48, 0x2B, 0xE0, 0xE8, 0x32, 0x00};

using GetEndGameStateFn = void* (*)();
using IsAnyPlayingFn    = bool (*)();
using WeakIsValidFn     = bool (*)(const void* weak);

// Set once at install.
GetEndGameStateFn g_getEndGameState = nullptr;
std::uintptr_t    g_gameRoot        = 0;  // the address holding the on/off table's root
std::uintptr_t    g_cutsceneManager = 0;  // the address holding the cutscene manager
IsAnyPlayingFn    g_isAnyPlaying    = nullptr;
std::uintptr_t    g_jobBoardSlot    = 0;  // the address holding the job board's weak pointer (0: not watched)
WeakIsValidFn     g_weakIsValid     = nullptr;
bool              g_installed       = false;

// Game thread.
ScreenSignals g_logged{};
bool          g_stateCallFailed    = false;
bool          g_cutsceneCallFailed = false;
bool          g_boardCallFailed    = false;
std::uint64_t g_location           = 0;      // the last location FName read
bool          g_locationRead       = false;
bool          g_atTitle            = false;  // g_location is the title's

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return address != 0 && platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

std::uintptr_t Pointer(std::uintptr_t address) noexcept
{
    std::uintptr_t value = 0;
    return Read(address, value) ? value : 0;
}

bool OnOff(std::uintptr_t stateManager, std::int32_t id) noexcept
{
    std::int32_t a = 0;
    std::int32_t b = 0;
    const auto   offset = static_cast<std::uintptr_t>(id) * sizeof(std::int32_t);
    return Read(stateManager + kOnOffCountA + offset, a) && Read(stateManager + kOnOffCountB + offset, b) && a + b != 0;
}

bool AnyMenuId(std::uintptr_t stateManager) noexcept
{
    for (const std::int32_t id : kMenuIds)
    {
        if (OnOff(stateManager, id))
        {
            return true;
        }
    }
    return false;
}

bool JobBoardOpen() noexcept
{
    const std::uintptr_t weak = g_boardCallFailed ? 0 : Pointer(g_jobBoardSlot);
    bool                 valid = false;
    if (weak != 0 && !platform::CallGuarded(g_weakIsValid, valid, reinterpret_cast<const void*>(weak)))
    {
        g_boardCallFailed = true;  // not called again this run
        platform::Log(L"whole screen: the job board's validity test raised an exception; the board no longer switches "
                      L"the view");
        return false;
    }
    return valid;
}

/// Logs the new location (a location jump; rare) and says whether it is the title.
bool IsTitleName(std::uint64_t name) noexcept
{
    const std::wstring text = players::unreal::NameText(name);  // guarded: "?" if the name pool faulted
    if (text == L"?")
    {
        platform::Log(L"whole screen: the location name {:#x} could not be read; not taken as the title", name);
        return false;
    }
    platform::Log(L"whole screen: location {}", text);
    return text == kTitleName;
}

/// Whether the game state's current location is the title; the name is turned into text only when it changes.
bool AtTitle(std::uintptr_t gameState) noexcept
{
    std::uint64_t name = 0;
    if (!Read(gameState + kLocation, name))
    {
        return false;
    }
    if (!g_locationRead || name != g_location)
    {
        g_location     = name;
        g_locationRead = true;
        g_atTitle      = IsTitleName(name);
    }
    return g_atTitle;
}

/// Each change of the signals, logged once (the edges the single view follows).
void LogOnChange(const ScreenSignals& now) noexcept
{
    if (now.readable == g_logged.readable && now.gameState == g_logged.gameState && now.menuKind == g_logged.menuKind &&
        now.scriptMenu == g_logged.scriptMenu && now.jobBoard == g_logged.jobBoard && now.cutscene == g_logged.cutscene &&
        now.skippable == g_logged.skippable && now.skipping == g_logged.skipping && now.title == g_logged.title)
    {
        return;
    }
    g_logged = now;
    platform::Log(L"whole screen: {}game state {} (menu kind {}), script menu {}, job board {}, cutscene {}, skippable "
                  L"scene {}, skip {}, title {} -> {}",
                  now.readable ? L"" : L"(no game state) ", now.gameState, now.menuKind, now.scriptMenu, now.jobBoard,
                  now.cutscene, now.skippable, now.skipping, now.title, NameOf(ReasonOf(now)));
}

}  // namespace

bool InstallWholeScreen(const platform::GameImage& image) noexcept
{
    const auto getState = image.Rebase(kGetEndGameState);
    const auto root     = image.Rebase(kGameRoot, sizeof(std::uintptr_t));
    const auto manager  = image.Rebase(kCutsceneManager, sizeof(std::uintptr_t));
    const auto playing  = image.Rebase(kIsAnyPlaying);
    if (!getState || !root || !manager || !playing || !image.BytesMatch(kGetEndGameState, kGetEndGameStateBytes) ||
        !image.BytesMatch(kIsAnyPlaying, kIsAnyPlayingBytes))
    {
        platform::Log(L"whole screen: the game's state or cutscene code is not what the research read; the screen "
                      L"stays split in menus and cutscenes (the single-view command still works)");
        return false;
    }
    g_getEndGameState = reinterpret_cast<GetEndGameStateFn>(*getState);
    g_gameRoot        = *root;
    g_cutsceneManager = *manager;
    g_isAnyPlaying    = reinterpret_cast<IsAnyPlayingFn>(*playing);
    g_installed       = true;
    const auto slot    = image.Rebase(kJobBoardSlot, sizeof(std::uintptr_t));
    const auto isValid = image.Rebase(kWeakIsValid);
    if (slot && isValid && image.BytesMatch(kWeakIsValid, kWeakIsValidBytes))
    {
        g_jobBoardSlot = *slot;
        g_weakIsValid  = reinterpret_cast<WeakIsValidFn>(*isValid);
    }
    platform::Log(L"whole screen: menus, {}the map, skippable scenes, cutscenes and the title are read from the game's "
                  L"own state",
                  g_jobBoardSlot != 0 ? L"the job board, " : L"(NOT the job board: its check failed) ");
    return true;
}

bool GameStatePresent() noexcept
{
    void* state = nullptr;
    return g_installed && !g_stateCallFailed && platform::CallGuarded(g_getEndGameState, state) && state != nullptr;
}

ScreenSignals ReadScreenSignals() noexcept
{
    ScreenSignals signals;
    if (!g_installed)
    {
        return signals;
    }
    void* state = nullptr;
    if (!g_stateCallFailed && !platform::CallGuarded(g_getEndGameState, state))
    {
        g_stateCallFailed = true;  // not called again this run
        platform::Log(L"whole screen: the game-state getter raised an exception; menus no longer switch the view");
    }
    const auto gameState = reinterpret_cast<std::uintptr_t>(state);
    signals.readable = !g_stateCallFailed && gameState != 0 && Read(gameState + kState, signals.gameState);
    if (signals.readable)
    {
        Read(gameState + kMenuKind, signals.menuKind);
        signals.title             = AtTitle(gameState);
        signals.jobBoard          = JobBoardOpen();
        const std::uintptr_t root = Pointer(g_gameRoot);
        if (const std::uintptr_t stateManager = root != 0 ? Pointer(root + kStateManager) : 0)
        {
            signals.scriptMenu = AnyMenuId(stateManager);
            signals.skippable  = OnOff(stateManager, kSkippableSceneId);
        }
        if (const std::uintptr_t manager = Pointer(g_cutsceneManager))  // only once the game has made it
        {
            std::uint8_t skipping = 0;
            signals.skipping      = Read(manager + kSkipping, skipping) && skipping != 0;
            bool playing          = false;
            if (!g_cutsceneCallFailed && !platform::CallGuarded(g_isAnyPlaying, playing))
            {
                g_cutsceneCallFailed = true;  // not called again this run
                platform::Log(L"whole screen: the cutscene check raised an exception; cutscenes no longer switch the view");
            }
            signals.cutscene = !g_cutsceneCallFailed && playing;
        }
    }
    LogOnChange(signals);
    return signals;
}

}  // namespace lss::views
