#include "views/whole_screen.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/unreal_access.h"
#include "views/screen_widgets.h"

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

// The pause-menu family (RE-p2-menu-census.md E1, S-MENUEDGE): UEndMenuAPI::IsPauseMenuClosed, the game's own
// EndUI_NoBlockingWidgetAlive thunk (bool fn(), true = closed). It only reads; its EndUISystem__Get would build the UI
// system if it did not exist, so it is called only once the system's slot 27 (written by its constructor) is set.
constexpr std::uintptr_t kPauseMenuClosed        = 0x140806908;
constexpr std::uint8_t   kPauseMenuClosedBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8};
constexpr std::uintptr_t kUiSlot27               = 0x1493497B8;  // EndUISystem[27], the pause-menu widget's holder

// The script menus with their own EndUISystem slot (E2): each static points at a heap holder; the weak pointers and
// fields below are the ones the game's own "still open" tests read. FWeakObjectPtr::Get for the widget fields.
constexpr std::uintptr_t kUiSlot54 = 0x149349890;  // jukebox: weak +0
constexpr std::uintptr_t kUiSlot56 = 0x1493498A0;  // chocobo carriage: byte +0x20 = open (the widget is reused)
constexpr std::uintptr_t kUiSlot57 = 0x1493498A8;  // point warp: weak +0, open while the widget's i32 +0x3D8 != 0
constexpr std::uintptr_t kUiSlot68 = 0x149349900;  // Queen's Blood NPC deck: weak +0 (reused; z +0x2A8 != -1)
constexpr std::uintptr_t kUiSlot72 = 0x149349920;  // chocobo stable +0, race menu +8 (byte +0x3D8 5 = done), select +0x10
constexpr std::uintptr_t kUiSlot78 = 0x149349950;  // Queen's Blood card challenge: weak +0
constexpr std::uintptr_t kUiSlot82 = 0x149349970;  // 3D Brawler select +0, winner +8 (no game test; z +0x2A8 != -1)
constexpr std::uint32_t  kCarriageShown  = 0x20;
constexpr std::uint32_t  kWidgetState    = 0x3D8;
constexpr std::uint32_t  kWidgetZOrder   = 0x2A8;  // -1 once the game's RemoveFromParent took it off its overlay
constexpr std::uint8_t   kRaceMenuDone   = 5;
constexpr std::uintptr_t kWeakGet        = 0x141905998;
constexpr std::uint8_t   kWeakGetBytes[] = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45,
                                            0x85, 0xC9, 0x74, 0x4F, 0x8B, 0x01, 0x85, 0xC0};

// For the log only (E4, E6): on/off 153 ShowColosseumMenu and 92 SoundCallEventMiniGameMenuInOut (set by no native
// code: Blueprint or script data), and Chadley's manager [root+0x258]: state machine count +0x1D0, table +0x1D8 (32-byte
// records {i32 id; Enter; Tick; Exit}), current +0x1E0, requested +0x1E8.
constexpr std::int32_t  kColosseumMenuId  = 153;
constexpr std::int32_t  kMiniGameMenuId   = 92;
constexpr std::uint32_t kChadley          = 0x258;
constexpr std::uint32_t kChadleyCount     = 0x1D0;
constexpr std::uint32_t kChadleyTable     = 0x1D8;
constexpr std::uint32_t kChadleyCurrent   = 0x1E0;
constexpr std::uint32_t kChadleyRequested = 0x1E8;
constexpr std::uint32_t kChadleyRecord    = 32;

// Minigames (RE-p2-minigames.md §1, §7; S-MINIGAME, both builds, the same offsets): the field-exclusion mask (u32 in the
// on/off table's owner), the on/off ids 36 MiniGamePlaying, 139 CrashBoxMiniGamePlaying, 140 CactusMissionMiniGamePlaying,
// and id 23's setter (the FName of the last code that turned it on; SM+0x1A128EC + 8*id).
constexpr std::uint32_t kExclusionMask  = 0x1A12288;
constexpr std::int32_t  kMiniGameId     = 36;
constexpr std::int32_t  kCrashBoxId     = 139;
constexpr std::int32_t  kCactusId       = 140;
constexpr std::uint32_t kId23Setter     = 0x1A129A4;
constexpr const wchar_t* kFrogGuysSetter = L"FEndFrogGuysMiniGame";
// The game's battle test, UEndBattleAPI_IsInBattle (1.004 0x1408E242C), as reads: the battle system's byte +0x300 is 0
// and the encounter state ([[game root]+0x1B8]+0x3D558) is 3 or 4. The game does not null-check the battle system; this
// does.
constexpr std::uintptr_t kBattleSystem   = 0x14939BC58;
constexpr std::uint32_t  kBattleIdle     = 0x300;
constexpr std::uint32_t  kRootGameMode   = 0x1B8;
constexpr std::uint32_t  kEncounterState = 0x3D558;
// The frog-guys manager (pointer global) and its frog character's weak pointer (FrogGuysManager_GetWeakCharacter16C); the
// G-Bike manager actor's weak pointer (a static FWeakObjectPtr).
constexpr std::uintptr_t kFrogManager  = 0x14939BCC0;
constexpr std::uint32_t  kFrogCharacter = 0x16C;
constexpr std::uintptr_t kGBikeManager = 0x1470C6CF0;
// The minigame-only maps (the game state's location).
constexpr std::array<const wchar_t*, 4> kMinigameMaps{L"2351-CRACE", L"2352-SCOST", L"2353-GBIKE", L"7110-MOGHS"};

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
using WeakGetFn         = void* (*)(const void* weak);
using PauseClosedFn     = bool (*)();

// Set once at install.
GetEndGameStateFn g_getEndGameState = nullptr;
std::uintptr_t    g_gameRoot        = 0;  // the address holding the on/off table's root
std::uintptr_t    g_cutsceneManager = 0;  // the address holding the cutscene manager
IsAnyPlayingFn    g_isAnyPlaying    = nullptr;
std::uintptr_t    g_jobBoardSlot    = 0;  // the address holding the job board's weak pointer (0: not watched)
WeakIsValidFn     g_weakIsValid     = nullptr;
WeakGetFn         g_weakGet         = nullptr;  // null: the slot menus are not read
PauseClosedFn     g_pauseClosed     = nullptr;  // null: the pause-family widgets are not read
std::uintptr_t    g_uiSlot27        = 0;
std::array<std::uintptr_t, 7> g_uiSlots{};     // 54, 56, 57, 68, 72, 78, 82 (0: not read)
std::uintptr_t    g_battleSystem    = 0;  // the address holding the battle system (0: no battle test)
std::uintptr_t    g_frogManager     = 0;
std::uintptr_t    g_gbikeManager    = 0;
bool              g_screenWidgets   = false;
bool              g_installed       = false;

// Game thread.
ScreenSignals g_logged{};
bool          g_stateCallFailed    = false;
bool          g_cutsceneCallFailed = false;
bool          g_boardCallFailed    = false;
bool          g_pauseCallFailed    = false;
unsigned      g_slotFaults         = 0;  // guarded slot calls that faulted (a stale holder): that read says closed
std::uint64_t g_location           = 0;      // the last location FName read
bool          g_locationRead       = false;
bool          g_atTitle            = false;  // g_location is the title's
bool          g_atMinigameMap      = false;  // g_location is a minigame-only map
std::uint64_t g_id23Setter         = 0;      // the last id-23 setter read
bool          g_id23Frog           = false;  // it is the frog-guys minigame

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

/// The game's own "a pause-family widget is alive": IsPauseMenuClosed is false.
bool PauseWidgetAlive() noexcept
{
    if (g_pauseClosed == nullptr || g_pauseCallFailed || Pointer(g_uiSlot27) == 0)
    {
        return false;
    }
    bool closed = true;
    if (!platform::CallGuarded(g_pauseClosed, closed))
    {
        g_pauseCallFailed = true;  // not called again this run
        platform::Log(L"whole screen: IsPauseMenuClosed raised an exception; Blueprint-opened pause menus no longer switch "
                      L"the view");
        return false;
    }
    return !closed;
}

/// A guarded slot call faulted: that read gives "closed" (the other slots go on being read). Logged on the first fault
/// and every 1000th.
void SlotFault() noexcept
{
    if (g_slotFaults++ % 1000 == 0)
    {
        platform::Log(L"whole screen: a menu slot's weak-pointer call raised an exception ({} so far); that menu reads as "
                      L"closed",
                      g_slotFaults);
    }
}

/// The game's validity test of the FWeakObjectPtr at `weak`; false if none or it faults.
bool WeakValid(std::uintptr_t weak) noexcept
{
    bool valid = false;
    if (weak == 0 || g_weakIsValid == nullptr)
    {
        return false;
    }
    if (!platform::CallGuarded(g_weakIsValid, valid, reinterpret_cast<const void*>(weak)))
    {
        SlotFault();
        return false;
    }
    return valid;
}

/// The game's resolve of the FWeakObjectPtr at `weak`; 0 if none or it faults.
std::uintptr_t WeakObject(std::uintptr_t weak) noexcept
{
    void* object = nullptr;
    if (weak == 0 || g_weakGet == nullptr)
    {
        return 0;
    }
    if (!platform::CallGuarded(g_weakGet, object, reinterpret_cast<const void*>(weak)))
    {
        SlotFault();
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(object);
}

/// A reused widget still in the game's overlay (its z field not -1).
bool InOverlay(std::uintptr_t widget) noexcept
{
    std::int32_t z = -1;
    return widget != 0 && Read(widget + kWidgetZOrder, z) && z != -1;
}

std::uintptr_t Holder(std::size_t index) noexcept
{
    return g_uiSlots[index] != 0 ? Pointer(g_uiSlots[index]) : 0;
}

/// The slot menus open now (NativeMenu bits), each by the test the game itself uses for it (E2).
std::uint16_t NativeMenusOpen() noexcept
{
    std::uint16_t open = 0;
    if (WeakValid(Holder(0)))
    {
        open |= kJukebox;
    }
    if (const auto carriage = Holder(1))
    {
        std::uint8_t shown = 0;
        open |= Read(carriage + kCarriageShown, shown) && shown != 0 ? kCarriage : 0;
    }
    if (const auto warp = WeakObject(Holder(2)))
    {
        std::int32_t state = 0;
        open |= Read(warp + kWidgetState, state) && state != 0 ? kPointWarp : 0;
    }
    if (InOverlay(WeakObject(Holder(3))))
    {
        open |= kCardDeckNpc;
    }
    if (const auto chocobo = Holder(4))
    {
        open |= WeakValid(chocobo) ? kChocoboStable : 0;
        open |= WeakValid(chocobo + 0x10) ? kChocoboSelect : 0;
        if (const auto race = WeakObject(chocobo + 8))
        {
            std::uint8_t state = kRaceMenuDone;
            open |= Read(race + kWidgetState, state) && state != kRaceMenuDone ? kChocoboRace : 0;
        }
    }
    if (WeakValid(Holder(5)))
    {
        open |= kCardChallenge;
    }
    if (const auto brawler = Holder(6))
    {
        open |= InOverlay(WeakObject(brawler)) ? kBrawlerSelect : 0;
        open |= InOverlay(WeakObject(brawler + 8)) ? kBrawlerWinner : 0;
    }
    return open;
}

/// Chadley's manager state for the log: the requested record, else the current one, accepted only inside the
/// manager's own table (-1: none or unreadable).
std::int8_t ChadleyState(std::uintptr_t root) noexcept
{
    const std::uintptr_t manager = root != 0 ? Pointer(root + kChadley) : 0;
    if (manager == 0)
    {
        return -1;
    }
    const std::uintptr_t table  = Pointer(manager + kChadleyTable);
    std::uintptr_t       record = Pointer(manager + kChadleyRequested);
    record                      = record != 0 ? record : Pointer(manager + kChadleyCurrent);
    std::int32_t count = 0;
    std::int32_t id    = -1;
    if (table == 0 || record < table || !Read(manager + kChadleyCount, count) || count <= 0 || count > 8 ||
        record >= table + kChadleyRecord * static_cast<std::uint32_t>(count) || (record - table) % kChadleyRecord != 0 ||
        !Read(record, id) || id < 0 || id > 127)
    {
        return -1;
    }
    return static_cast<std::int8_t>(id);
}

/// The game's UEndBattleAPI::IsInBattle, as reads.
bool InBattle(std::uintptr_t root) noexcept
{
    const std::uintptr_t system = Pointer(g_battleSystem);
    std::uint8_t         idle   = 1;
    std::uint8_t         state  = 0;
    const std::uintptr_t mode   = root != 0 ? Pointer(root + kRootGameMode) : 0;
    return system != 0 && Read(system + kBattleIdle, idle) && idle == 0 && mode != 0 &&
           Read(mode + kEncounterState, state) && (state == 3 || state == 4);
}

/// Whether id 23's setter is the frog-guys minigame (the name turned into text only when it changes).
bool FrogGuysSetter(std::uintptr_t stateManager) noexcept
{
    std::uint64_t setter = 0;
    if (!Read(stateManager + kId23Setter, setter) || setter == 0)
    {
        return false;
    }
    if (setter != g_id23Setter)
    {
        g_id23Setter = setter;
        g_id23Frog   = players::unreal::NameText(setter) == kFrogGuysSetter;
    }
    return g_id23Frog;
}

/// The minigame inputs of the signals (RE-p2-minigames.md §7.6).
void ReadMinigame(ScreenSignals& signals, std::uintptr_t root, std::uintptr_t stateManager) noexcept
{
    signals.inBattle    = g_battleSystem == 0 || InBattle(root);  // no battle test: the minigame bit needs a minigame id
    signals.minigameMap = g_atMinigameMap;
    signals.gbike       = g_gbikeManager != 0 && WeakValid(g_gbikeManager);
    if (stateManager == 0)
    {
        return;
    }
    Read(stateManager + kExclusionMask, signals.exclusion);
    signals.minigameIds = OnOff(stateManager, kMiniGameId) || OnOff(stateManager, kCrashBoxId) ||
                          OnOff(stateManager, kCactusId);
    if ((signals.exclusion & kExclusionSecond) != 0 && !signals.minigameIds)  // the frog test matters only then
    {
        const std::uintptr_t frog = g_frogManager != 0 ? Pointer(g_frogManager) : 0;
        signals.frogGuys          = (frog != 0 && WeakValid(frog + kFrogCharacter)) || FrogGuysSetter(stateManager);
    }
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
        g_location      = name;
        g_locationRead  = true;
        g_atTitle       = IsTitleName(name);
        const std::wstring text = players::unreal::NameText(name);
        g_atMinigameMap = false;
        for (const wchar_t* map : kMinigameMaps)
        {
            g_atMinigameMap = g_atMinigameMap || text == map;
        }
    }
    return g_atTitle;
}

/// Each change of the signals, logged once (the edges the single view follows).
void LogOnChange(const ScreenSignals& now) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    if (now.readable == g_logged.readable && now.gameState == g_logged.gameState && now.menuKind == g_logged.menuKind &&
        now.scriptMenu == g_logged.scriptMenu && now.jobBoard == g_logged.jobBoard && now.cutscene == g_logged.cutscene &&
        now.skippable == g_logged.skippable && now.skipping == g_logged.skipping && now.title == g_logged.title &&
        now.pauseWidget == g_logged.pauseWidget && now.nativeMenus == g_logged.nativeMenus &&
        now.screenWidget == g_logged.screenWidget && now.colosseumFlag == g_logged.colosseumFlag &&
        now.miniGameMenu == g_logged.miniGameMenu && now.chadleyState == g_logged.chadleyState &&
        now.exclusion == g_logged.exclusion && now.minigameIds == g_logged.minigameIds &&
        now.frogGuys == g_logged.frogGuys && now.inBattle == g_logged.inBattle && now.gbike == g_logged.gbike &&
        now.minigameMap == g_logged.minigameMap)
    {
        return;
    }
    g_logged = now;
    platform::LogDebug(L"whole screen: {}game state {} (menu kind {}), script menu {}, job board {}, pause widget {}, slot "
                       L"menus {:#x}, full-screen widget {}, cutscene {}, skippable scene {}, skip {}, title {}; on/off 153 "
                       L"{}, 92 {}, Chadley state {}; exclusion {:#x}, minigame ids {}, frog {}, in battle {}, G-Bike {}, "
                       L"minigame map {} -> {}",
                       now.readable ? L"" : L"(no game state) ", now.gameState, now.menuKind, now.scriptMenu,
                       now.jobBoard, now.pauseWidget, now.nativeMenus, now.screenWidget, now.cutscene, now.skippable,
                       now.skipping, now.title, now.colosseumFlag, now.miniGameMenu, now.chadleyState, now.exclusion,
                       now.minigameIds, now.frogGuys, now.inBattle, now.gbike, now.minigameMap, NameOf(ReasonOf(now)));
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
    const auto pauseClosed = image.Rebase(kPauseMenuClosed);
    const auto slot27      = image.Rebase(kUiSlot27, sizeof(std::uintptr_t));
    if (pauseClosed && slot27 && image.BytesMatch(kPauseMenuClosed, kPauseMenuClosedBytes))
    {
        g_pauseClosed = reinterpret_cast<PauseClosedFn>(*pauseClosed);
        g_uiSlot27    = *slot27;
    }
    const auto weakGet = image.Rebase(kWeakGet);
    if (g_weakIsValid != nullptr && weakGet && image.BytesMatch(kWeakGet, kWeakGetBytes))
    {
        g_weakGet = reinterpret_cast<WeakGetFn>(*weakGet);
        const std::array<std::uintptr_t, 7> statics{kUiSlot54, kUiSlot56, kUiSlot57, kUiSlot68,
                                                    kUiSlot72, kUiSlot78, kUiSlot82};
        for (std::size_t i = 0; i < statics.size(); ++i)
        {
            const auto live = image.Rebase(statics[i], sizeof(std::uintptr_t));
            g_uiSlots[i]    = live ? *live : 0;
        }
    }
    g_screenWidgets = screen_widgets::Install(image);
    const auto battleSystem = image.Rebase(kBattleSystem, sizeof(std::uintptr_t));
    const auto frogManager  = image.Rebase(kFrogManager, sizeof(std::uintptr_t));
    const auto gbikeManager = image.Rebase(kGBikeManager, sizeof(std::uint64_t));
    g_battleSystem          = battleSystem ? *battleSystem : 0;
    g_frogManager           = frogManager ? *frogManager : 0;
    g_gbikeManager          = gbikeManager ? *gbikeManager : 0;
    platform::Log(L"whole screen: minigames on their own stage by the game's field-exclusion mask{}{}{}",
                  g_battleSystem != 0 ? L"" : L" (NO battle test: the minigame bit is read only with a minigame id)",
                  g_frogManager != 0 ? L"" : L" (no frog-guys manager)", g_gbikeManager != 0 ? L"" : L" (no G-Bike)");
    platform::Log(L"whole screen: menus, {}the map, skippable scenes, cutscenes and the title are read from the game's "
                  L"own state; {}; {}",
                  g_jobBoardSlot != 0 ? L"the job board, " : L"(NOT the job board: its check failed) ",
                  g_pauseClosed != nullptr ? L"Blueprint-opened pause menus by IsPauseMenuClosed"
                                           : L"NOT the Blueprint-opened pause menus (IsPauseMenuClosed check failed)",
                  g_weakGet != nullptr ? L"the slot menus (jukebox, carriage, point warp, chocobo, Queen's Blood, 3D "
                                         L"Brawler) by the game's own tests"
                                       : L"NOT the slot menus (a weak-pointer check failed)");
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
            signals.scriptMenu    = AnyMenuId(stateManager);
            signals.skippable     = OnOff(stateManager, kSkippableSceneId);
            signals.colosseumFlag = OnOff(stateManager, kColosseumMenuId);
            signals.miniGameMenu  = OnOff(stateManager, kMiniGameMenuId);
        }
        signals.chadleyState = ChadleyState(root);
        ReadMinigame(signals, root, root != 0 ? Pointer(root + kStateManager) : 0);
        signals.pauseWidget  = PauseWidgetAlive();
        signals.nativeMenus  = NativeMenusOpen();
        signals.screenWidget = g_screenWidgets && screen_widgets::FullScreenOpen();
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
