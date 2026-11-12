#include "views/whole_screen.h"

#include "facts/engine.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/unreal_access.h"
#include "views/screen_widgets.h"
#include "views/whole_screen_rules.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace lss::views {

namespace {

// research/RE-p2-single-view.md; this build.
//
// The game state through the game's own getter, which also checks its class (null otherwise): it
// reads g_AEndGameMode +0x288, takes no arguments, and changes nothing.
constexpr std::uint8_t   kGetEndGameStateBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                    0x70, 0x0A, 0x68, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kGetEndGameState{L"the EndGameState getter", 0x140A2E4B4, kGetEndGameStateBytes};
constexpr std::uint32_t  kState    = 0x278;  // uint8: the EndGameState state (+0x279 is its history)
constexpr std::uint32_t  kMenuKind = 0x3BC;  // uint8: EEndSystemPauseMenuKind

// The on/off table: its owner at [[kEndGameRoot]+0x170] (a native game object the game mode creates
// and frees), two int32 counts per id; an id is on while their sum is not zero.
using facts::kEndGameRoot;
using facts::kRootGameState;
constexpr std::uint32_t  kOnOffCountA  = 0x1A1238C;
constexpr std::uint32_t  kOnOffCountB  = 0x1A1263C;

// EndFieldOnOffTable: ShowMainMenu, ShowShopMenu, ShowMapJournal, ShowSkillTreeMenu,
// ShowMainMenuInPV, ShowMainMenu_CardGameDecks.
constexpr std::array<std::int32_t, 6> kMenuIds{147, 148, 149, 154, 155, 156};
// EndFieldOnOffTable ShowSkipPauseMenu: held by the dialogue ("Dialogue") and camera-planner ("PlannerEvent")
// scenes while they can be skipped.
constexpr std::int32_t kSkippableSceneId = 12;

// The job board: EndUISystem's slot +0x1E8 (a static) points to the board's 8-byte FWeakObjectPtr;
// the board's script command keeps running while the game's FWeakObjectPtr_IsValid says it is valid.
constexpr std::uintptr_t kJobBoardSlot          = 0x1493498C8;
constexpr std::uint8_t   kWeakIsValidBytes[]    = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45,
                                                   0x85, 0xC9, 0x74, 0x4F, 0x8B, 0x01, 0x85, 0xC0};
constexpr platform::FunctionFact kWeakIsValid{L"FWeakObjectPtr_IsValid", 0x1418EB3D4, kWeakIsValidBytes};

// The pause-menu family (research/RE-p2-menu-census.md): UEndMenuAPI::IsPauseMenuClosed, the game's own
// EndUI_NoBlockingWidgetAlive thunk (bool fn(), true = closed). It only reads; its EndUISystem__Get would build the UI
// system if it did not exist, so it is called only once the system's slot 27 (written by its constructor) is set.
constexpr std::uint8_t   kPauseMenuClosedBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kPauseMenuClosed{L"UEndMenuAPI::IsPauseMenuClosed", 0x140806908,
                                                  kPauseMenuClosedBytes};
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

// On/off 153 ShowColosseumMenu, held by a VR or colosseum session from Begin to End (research/RE-p2-minigames.md). For the
// log only (E4, E6): 92 SoundCallEventMiniGameMenuInOut (set by no native code: Blueprint or script data), and Chadley's
// manager [root+0x258]: state machine count +0x1D0, table +0x1D8 (32-byte records {i32 id; Enter; Tick; Exit}), current
// +0x1E0, requested +0x1E8.
constexpr std::int32_t  kColosseumMenuId  = 153;
constexpr std::int32_t  kMiniGameMenuId   = 92;
constexpr std::uint32_t kChadley          = 0x258;
constexpr std::uint32_t kChadleyCount     = 0x1D0;
constexpr std::uint32_t kChadleyTable     = 0x1D8;
constexpr std::uint32_t kChadleyCurrent   = 0x1E0;
constexpr std::uint32_t kChadleyRequested = 0x1E8;
constexpr std::uint32_t kChadleyRecord    = 32;

// Minigames (research/RE-p2-minigames.md, RE-p2-minigames-stage2.md; both builds, the same offsets): the
// field-exclusion mask (u32 in the on/off table's owner), the on/off ids 36 MiniGamePlaying, 139 CrashBoxMiniGamePlaying,
// 140 CactusMissionMiniGamePlaying.
constexpr std::uint32_t kExclusionMask  = 0x1A12288;
constexpr std::int32_t  kMiniGameId     = 36;
constexpr std::int32_t  kCrashBoxId     = 139;
constexpr std::int32_t  kCactusId       = 140;
// Id 36's setter (SM+0x1A128EC + 8*36: the FName of the last SetOn that passed one, cleared when 36 goes off); the field
// uses by name are in FieldUseName (whole_screen_reason.cpp).
constexpr std::uint32_t kId36Setter = 0x1A12A0C;
// The game's battle test, UEndBattleAPI_IsInBattle, as reads: the battle system's byte +0x300 is 0
// and the encounter state ([[game root]+0x1B8]+0x3D558) is 3 or 4. The game does not null-check the battle system; this
// does.
using facts::kBattleStateGlobal;
constexpr std::uint32_t  kBattleIdle     = 0x300;
using facts::kRootModeHolder;
constexpr std::uint32_t  kEncounterState = 0x3D558;
// The frog-guys manager (a pointer global to a plain C++ object, cleared in its destructor before it dies) and its
// EndStateMachine's current row (+0x18, into a static table in the exe; the state id is the row's first i32: 2 load ..
// 7 play .. 8 result .. 11 retry or finish, 0 done, 1 unused; research/RE-p2-minigames.md); the G-Bike manager actor's weak pointer (a
// static FWeakObjectPtr).
constexpr std::uintptr_t kFrogManager  = 0x14939BCC0;
constexpr std::uint32_t  kFrogStateRow = 0x18;
constexpr std::uintptr_t kGBikeManager = 0x1470C6CF0;
// The minigame-only maps (the game state's location): views/whole_screen_rules.h.

// The game state's current location (an 8-byte FName; +0x30C requested, +0x314 previous) and the
// title's, which the game compares against itself.
constexpr std::uint32_t  kLocation  = 0x304;  // the title's name: views/whole_screen_rules.h

// The cutscene manager (a pointer global; the game creates the manager on first use, so the check
// below is called only once it exists) and the game's own "a sequence of type 0 or 2 plays". The
// check takes no arguments, writes nothing, and walks the manager's list on the game thread.
constexpr std::uintptr_t kCutsceneManager     = 0x1490FEB30;
constexpr std::uint32_t  kSkipping            = 0xA60;  // uint8: a cutscene skip under way
constexpr std::uint8_t   kIsAnyPlayingBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0xDA,
                                                 0x65, 0x01, 0x48, 0x2B, 0xE0, 0xE8, 0x32, 0x00};
constexpr platform::FunctionFact kIsAnyPlaying{L"the cutscene manager's IsAnyPlaying", 0x140A514AC, kIsAnyPlayingBytes};

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
std::uint64_t g_id36Setter         = 0;      // the last id-36 setter read
bool          g_id36Field          = false;  // it is a field use

using platform::Read;  // the guarded reads: platform/guarded.h
using platform::ReadAt;

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
    const std::uintptr_t weak = g_boardCallFailed ? 0 : ReadAt<std::uintptr_t>(g_jobBoardSlot);
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
    if (g_pauseClosed == nullptr || g_pauseCallFailed || ReadAt<std::uintptr_t>(g_uiSlot27) == 0)
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

/// A guarded weak-pointer call faulted (a menu slot's, or the G-Bike manager's): that read gives "closed"
/// (the others go on being read). Logged on the first fault and every 1000th.
void SlotFault() noexcept
{
    if (g_slotFaults++ % 1000 == 0)
    {
        platform::Log(L"whole screen: a weak-pointer call (a menu slot, or the G-Bike manager) raised an "
                      L"exception ({} so far); that read says closed",
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
    return g_uiSlots[index] != 0 ? ReadAt<std::uintptr_t>(g_uiSlots[index]) : 0;
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
    const std::uintptr_t manager = root != 0 ? ReadAt<std::uintptr_t>(root + kChadley) : 0;
    if (manager == 0)
    {
        return -1;
    }
    const std::uintptr_t table  = ReadAt<std::uintptr_t>(manager + kChadleyTable);
    std::uintptr_t       record = ReadAt<std::uintptr_t>(manager + kChadleyRequested);
    record                      = record != 0 ? record : ReadAt<std::uintptr_t>(manager + kChadleyCurrent);
    std::int32_t count = 0;
    std::int32_t id    = -1;
    if (table == 0 || record < table || !Read(manager + kChadleyCount, count) ||
        !ChadleyRecordValid(table, record, count, kChadleyRecord) || !Read(record, id) || !ChadleyIdValid(id))
    {
        return -1;
    }
    return static_cast<std::int8_t>(id);
}

/// The game's UEndBattleAPI::IsInBattle, as reads.
bool InBattle(std::uintptr_t root) noexcept
{
    const std::uintptr_t system = ReadAt<std::uintptr_t>(g_battleSystem);
    std::uint8_t         idle   = 1;
    std::uint8_t         state  = 0;
    const std::uintptr_t mode   = root != 0 ? ReadAt<std::uintptr_t>(root + kRootModeHolder) : 0;
    return system != 0 && Read(system + kBattleIdle, idle) && idle == 0 && mode != 0 &&
           Read(mode + kEncounterState, state) && EncounterInBattle(state);
}

/// Whether id 36's setter is a field use (the name turned into text only when it changes).
bool FieldMinigameSetter(std::uintptr_t stateManager) noexcept
{
    std::uint64_t setter = 0;
    if (!Read(stateManager + kId36Setter, setter) || setter == 0)
    {
        return false;
    }
    if (setter != g_id36Setter)
    {
        const std::wstring text = players::unreal::NameText(setter);
        if (text == L"?")
        {
            return false;  // the name pool faulted: not cached, read again next tick
        }
        g_id36Setter = setter;
        g_id36Field  = FieldUseName(text);
        platform::LogDebug(L"whole screen: on/off 36 set by {}{}", text,
                           g_id36Field ? L" (a field use: stays split)" : L" (a minigame)");
    }
    return g_id36Field;
}

/// Whether the frog-guys manager is in a game state (2..11); its global stays set in state 0 until the command dies.
bool FrogGuysPlaying() noexcept
{
    const std::uintptr_t frog  = g_frogManager != 0 ? ReadAt<std::uintptr_t>(g_frogManager) : 0;
    const std::uintptr_t row   = frog != 0 ? ReadAt<std::uintptr_t>(frog + kFrogStateRow) : 0;
    std::int32_t         state = 0;
    return row != 0 && Read(row, state) && state >= 2 && state <= 11;
}

/// The minigame inputs of the signals (research/RE-p2-minigames.md).
void ReadMinigame(ScreenSignals& signals, std::uintptr_t root, std::uintptr_t stateManager) noexcept
{
    signals.inBattle    = g_battleSystem == 0 || InBattle(root);  // no battle test: the minigame bit is not read
    signals.minigameMap = g_atMinigameMap;
    signals.gbike       = g_gbikeManager != 0 && WeakValid(g_gbikeManager);
    signals.frogGuys    = FrogGuysPlaying();
    if (stateManager == 0)
    {
        return;
    }
    Read(stateManager + kExclusionMask, signals.exclusion);
    signals.minigamePlaying = OnOff(stateManager, kMiniGameId);
    signals.fieldMinigame   = signals.minigamePlaying && FieldMinigameSetter(stateManager);
    signals.minigameIds     = OnOff(stateManager, kCrashBoxId) || OnOff(stateManager, kCactusId);
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
    return ClassifyLocation(text).title;
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
        g_atMinigameMap = ClassifyLocation(players::unreal::NameText(name)).minigameMap;
    }
    return g_atTitle;
}

}  // namespace

bool AtTitleNow() noexcept
{
    void* state = nullptr;
    if (!g_installed || g_stateCallFailed || !platform::CallGuarded(g_getEndGameState, state) || state == nullptr)
    {
        return false;
    }
    return AtTitle(reinterpret_cast<std::uintptr_t>(state));
}

namespace {

/// Each change of the signals, logged once (the edges the single view follows).
void LogOnChange(const ScreenSignals& now) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    if (now == g_logged)
    {
        return;
    }
    g_logged = now;
    platform::LogDebug(L"whole screen: {}game state {} (menu kind {}), script menu {}, job board {}, pause widget {}, slot "
                       L"menus {:#x}, full-screen widget {}, cutscene {}, skippable scene {}, skip {}, title {}; on/off 153 "
                       L"{}, 92 {}, Chadley state {}; exclusion {:#x}, minigame playing {} (field use {}), 139/140 {}, "
                       L"frog {}, in battle {}, G-Bike {}, minigame map {} -> {}",
                       now.readable ? L"" : L"(no game state) ", now.gameState, now.menuKind, now.scriptMenu,
                       now.jobBoard, now.pauseWidget, now.nativeMenus, now.screenWidget, now.cutscene, now.skippable,
                       now.skipping, now.title, now.colosseumFlag, now.miniGameMenu, now.chadleyState, now.exclusion,
                       now.minigamePlaying, now.fieldMinigame, now.minigameIds, now.frogGuys, now.inBattle, now.gbike,
                       now.minigameMap, NameOf(ReasonOf(now)));
}

}  // namespace

bool InstallWholeScreen(const platform::GameImage& image) noexcept
{
    const auto getState = platform::FindFunction(image, kGetEndGameState);
    const auto root     = image.Rebase(kEndGameRoot, sizeof(std::uintptr_t));
    const auto manager  = image.Rebase(kCutsceneManager, sizeof(std::uintptr_t));
    const auto playing  = platform::FindFunction(image, kIsAnyPlaying);
    if (!getState || !root || !manager || !playing)
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
    const auto isValid = platform::FindFunction(image, kWeakIsValid);
    if (slot && isValid)
    {
        g_jobBoardSlot = *slot;
        g_weakIsValid  = reinterpret_cast<WeakIsValidFn>(*isValid);
    }
    const auto pauseClosed = platform::FindFunction(image, kPauseMenuClosed);
    const auto slot27      = image.Rebase(kUiSlot27, sizeof(std::uintptr_t));
    if (pauseClosed && slot27)
    {
        g_pauseClosed = reinterpret_cast<PauseClosedFn>(*pauseClosed);
        g_uiSlot27    = *slot27;
    }
    const auto weakGet = platform::FindFunction(image, facts::kWeakObjectGet);
    if (g_weakIsValid != nullptr && weakGet)
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
    const auto battleSystem = image.Rebase(kBattleStateGlobal, sizeof(std::uintptr_t));
    const auto frogManager  = image.Rebase(kFrogManager, sizeof(std::uintptr_t));
    const auto gbikeManager = image.Rebase(kGBikeManager, sizeof(std::uint64_t));
    // The global's address is proven by IsInBattle's bytes (its RIP-relative read of it), found at startup.
    g_battleSystem          = battleSystem && facts::InBattleKnown() ? *battleSystem : 0;
    g_frogManager           = frogManager ? *frogManager : 0;
    g_gbikeManager          = gbikeManager ? *gbikeManager : 0;
    platform::Log(L"whole screen: minigames by the game's on/off 36 MiniGamePlaying (the field uses stay split), and by "
                  L"its field-exclusion mask before a game's 36{}{}{}",
                  g_battleSystem != 0 ? L"" : L" (NO battle test: the minigame bit is not read)",
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

bool WholeScreenInstalled() noexcept
{
    return g_installed;
}

bool WholeScreenShown() noexcept
{
    return g_installed && ReasonOf(ReadScreenSignals()) != WholeScreenReason::None;
}

bool GameStatePresent() noexcept
{
    void* state = nullptr;
    return g_installed && !g_stateCallFailed && platform::CallGuarded(g_getEndGameState, state) && state != nullptr;
}

const wchar_t* LocalPlayerRefused() noexcept
{
    static constexpr wchar_t kPauseMenu[] = L"the pause menu owns the screen (the engine refuses a local player there)";
    void*                    state        = nullptr;
    if (!g_installed || g_stateCallFailed || !platform::CallGuarded(g_getEndGameState, state) || state == nullptr)
    {
        return nullptr;
    }
    std::uint8_t current = 0;
    return Read(reinterpret_cast<std::uintptr_t>(state) + kState, current) && current == kStatePauseMenu ? kPauseMenu
                                                                                                         : nullptr;
}

bool MenuOwnsInput() noexcept
{
    void* state = nullptr;
    if (!g_installed || g_stateCallFailed)
    {
        return false;
    }
    if (!platform::CallGuarded(g_getEndGameState, state))
    {
        g_stateCallFailed = true;  // not called again this run (ReadScreenSignals logs the same fault the same way)
        platform::Log(L"whole screen: the game-state getter raised an exception; menus no longer switch the view");
        return false;
    }
    if (state == nullptr)
    {
        return false;
    }
    std::uint8_t current = 0;
    return Read(reinterpret_cast<std::uintptr_t>(state) + kState, current) && MenuOwnsInput(current, PauseWidgetAlive());
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
        const std::uintptr_t root = ReadAt<std::uintptr_t>(g_gameRoot);
        if (const std::uintptr_t stateManager = root != 0 ? ReadAt<std::uintptr_t>(root + kRootGameState) : 0)
        {
            signals.scriptMenu    = AnyMenuId(stateManager);
            signals.skippable     = OnOff(stateManager, kSkippableSceneId);
            signals.colosseumFlag = OnOff(stateManager, kColosseumMenuId);
            signals.miniGameMenu  = OnOff(stateManager, kMiniGameMenuId);
        }
        signals.chadleyState = ChadleyState(root);
        ReadMinigame(signals, root, root != 0 ? ReadAt<std::uintptr_t>(root + kRootGameState) : 0);
        signals.pauseWidget  = PauseWidgetAlive();
        signals.nativeMenus  = NativeMenusOpen();
        signals.screenWidget = g_screenWidgets && screen_widgets::FullScreenOpen();
        if (const std::uintptr_t manager = ReadAt<std::uintptr_t>(g_cutsceneManager))  // only once the game has made it
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

void RemoveWholeScreen() noexcept
{
    screen_widgets::Remove();  // the reads above patch nothing
}

}  // namespace lss::views
