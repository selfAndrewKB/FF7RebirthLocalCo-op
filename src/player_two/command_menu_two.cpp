#include "player_two/command_menu_two.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "hud/widget_opacity.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/focus_scope.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/party_member.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::player_two::command_menu_two {

namespace {

// Facts read from the database (RE-p2-command-menu.md §1.1, §10; this build).
constexpr std::uint8_t kMallocBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                         0x00, 0x00, 0x00, 0xE8, 0x48, 0xFB, 0x86, 0x01};
constexpr platform::FunctionFact kMalloc{L"FMemory_Malloc", 0x14083F3D8, kMallocBytes};
constexpr std::uint8_t kCtorBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48,
                                       0x89, 0x7C, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B,
                                       0xEC, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0xBB, 0x64, 0x6A, 0xFE};
constexpr platform::FunctionFact kCtor{L"FieldCommandCtrl__Ctor", 0x143A08A54, kCtorBytes};
constexpr std::uint8_t kSetupBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55,
                                        0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                        0x8B, 0xEC, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x1A, 0x3E, 0xB2, 0x00};
constexpr platform::FunctionFact kSetup{L"FieldCommandCtrl__SetupMenuWidgets", 0x14158B0F4, kSetupBytes};

constexpr std::size_t kControllerSize = 0x798;
// The key guide (EndUISystem[53]): the game builds its one with FMemory_Malloc(0x38) and KeyGuide53__Ctor
// (EndUISystem__Ctor); player 2's is built the same way (RE-p2-command-menu-views.md §10.4). Its widget
// (FWeakObjectPtr at +4) is created by the game's own ShowMode the first time player 2's controller drives it.
constexpr std::uint8_t kKeyGuideCtorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xE0,
                                               0x0F, 0x69, 0xFE, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::FunctionFact kKeyGuideCtor{L"KeyGuide53__Ctor", 0x143A1DF44, kKeyGuideCtorBytes};
constexpr std::size_t   kKeyGuideSize   = 0x38;
constexpr std::uint32_t kKeyGuideSlot   = 0x1A8;  // EndUISystem[53]
constexpr std::uint32_t kKeyGuideWidget = 0x04;
// The UI system's command controller slot (EndUISystem[23]: the game's controller).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kGameController = 0xB8;
constexpr std::uint32_t  kHudMode        = 0x540;  // u8: the battle HUD mode
// The controller's lifecycle (RE-p2-command-menu-lifecycle.md §2, §3.1): "setup done" (+0x777: Setup sets
// it, only ResetAll clears it; while 0 no menu state can be pushed) and "setup pending" (+0x775). The
// game resets and sets up its own controller again when it restarts the field (a retry); the UI
// system's +0x2D1 marks a Setup deferred behind a fade.
constexpr std::uint32_t kSetupDone     = 0x777;
constexpr std::uint32_t kSetupPending  = 0x775;
constexpr std::uint32_t kSetupDeferred = 0x2D1;  // in the UI system
constexpr std::uint32_t kSelecterSet   = 0x776;  // u8, set by Setup (logged)
constexpr std::uint32_t kSetupOther    = 0x778;  // u8, set by Setup (logged)
constexpr std::uint32_t kMenuLayer     = 0x783;  // u8 (logged)
constexpr int           kRepairGap     = 30;     // engine ticks between repair tries
// What the log reads of a controller: the members (leader, ally, shortcut target, ally pick), the
// menu widget (FWeakObjectPtr) and its back pointer to the controller, and the focus bytes (§11 of
// RE-p2-command-menu-input.md: battle input, switching layer, +0x777, menu focus layer, ready, HUD mode).
constexpr std::uint32_t kMembers    = 0x34;
constexpr std::uint32_t kMenuWidget = 0x4F0;
constexpr std::uint32_t kWidgetCtrl = 0x4E0;
constexpr std::array<std::uint32_t, 6> kBytes{0x25C, 0x258, 0x777, 0x783, 0x5E1, 0x540};
// UObjectBase: the class and the outer.
constexpr std::uint32_t kObjectClass = 0x10;
constexpr std::uint32_t kObjectOuter = 0x20;
constexpr int kSetupTries = 5;  // per world; a refused focus scope is tried again next tick
constexpr int kMaxLines   = 200;

using MallocFn = void* (*)(std::size_t size, std::uint32_t alignment);
using CtorFn   = void* (*)(void* controller);
using SetupFn  = std::uint8_t (*)(void* controller, void* world);
using KeyGuideCtorFn = void* (*)(void* keyGuide);
using WeakGetFn = void* (*)(const std::uint64_t* weak);

// Set once at install.
MallocFn  g_malloc  = nullptr;
CtorFn    g_ctor    = nullptr;
SetupFn   g_setup   = nullptr;
KeyGuideCtorFn g_keyGuideCtor = nullptr;
WeakGetFn g_weakGet = nullptr;
std::uintptr_t g_worldClass = 0;  // UWorld's class

// Game thread. The controller outlives the mod's stages, like the game's.
void*         g_controller = nullptr;
void*         g_keyGuide   = nullptr;  // player 2's key guide (built after the controller; kept like it)
bool          g_installed  = false;
bool          g_failed     = false;
std::uint64_t g_worldWeak  = 0;        // the world the controller was set up for (0: none)
void*         g_world      = nullptr;  // that world, and the one the tries count for
int           g_tries      = 0;
int           g_lines      = 0;

struct Snapshot
{
    std::array<std::int32_t, 4>          members{};
    std::array<std::uint8_t, kBytes.size()> bytes{};
    void*                                widget = nullptr;
    bool                                 widgetNamesUs = false;
    focus_scope::Summary                 context{};
    bool operator==(const Snapshot& other) const
    {
        return members == other.members && bytes == other.bytes && widget == other.widget &&
               widgetNamesUs == other.widgetNamesUs && context.pathCount == other.context.pathCount &&
               context.layerCount == other.context.layerCount;
    }
};
Snapshot g_logged{};
bool     g_loggedOnce = false;
int      g_hides      = 0;  // times the menu widget was found shown and hidden again
int      g_repairTries = 0;   // Setup repairs tried since player 2's "setup done" was last 1
unsigned g_repairTicks = 0;
std::array<std::uint8_t, 3> g_loggedLifecycle{0xFF, 0xFF, 0xFF};  // P1 setup done, deferred, P2 setup done
std::uintptr_t g_endUISystem = 0;
// A controller's widgets shown per half (RE-p2-command-menu-views.md §10.3, §10.5): the command
// menu, the field menu (the party list) and the wait-mode filter (the tactical-mode tint), each an
// FWeakObjectPtr in the controller. In player 2's pass, player 2's command and field menus take player
// 1's own opacity (their fades), the filter its full opacity (its own content decides).
struct Part
{
    std::uint32_t offset;
    bool          borrowOpacity;
};
constexpr std::array<Part, 3> kParts{{{kMenuWidget, true}, {0x4E8, true}, {0x62C, false}}};
// Render (game thread): player 1's widgets (FWeakObjectPtr, resolved again at each use: the game can
// rebuild them) and their own opacity while player 2's pass shows player 2's.
struct Saved
{
    std::uint64_t weak    = 0;
    float         opacity = 1.0F;
};
std::array<Saved, kParts.size() + 1> g_oneSaved{};  // the parts, then the key guide's widget
bool                             g_swapped = false;

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    if (address != 0)
    {
        platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(T));
    }
    return value;
}

void Line(const std::wstring& text) noexcept
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"command menu two: {}", text);
    }
}

/// The world player 1's controller is in (the first outer of class UWorld; guarded reads), or null.
void* WorldOf(void* controller) noexcept
{
    auto object = reinterpret_cast<std::uintptr_t>(controller);
    for (int depth = 0; object != 0 && g_worldClass != 0 && depth < 4; ++depth)
    {
        object = Field<std::uintptr_t>(object + kObjectOuter);
        if (object != 0 && Field<std::uintptr_t>(object + kObjectClass) == g_worldClass)
        {
            return reinterpret_cast<void*>(object);
        }
    }
    return nullptr;
}

/// How many command menu widgets exist (each controller's Setup creates one when it has none).
std::size_t CountMenus() noexcept
{
    try
    {
        std::vector<RC::Unreal::UObject*> menus;
        RC::Unreal::UObjectGlobals::FindAllOf(STR("Command_C"), menus);
        return menus.size();
    }
    catch (...)
    {
        return 0;
    }
}

enum class SetupResult
{
    Ran,
    Refused,
    Faulted
};

/// Setup on player 2's controller in player 2's focus context and, when player 2's member is known, with
/// the party leader answering player 2's member (Setup stores the leader's type in the controller).
SetupResult RunSetup(void* world, std::uint8_t& result, const wchar_t*& refusal) noexcept
{
    void*      member     = generation::Member();
    const auto memberType = member != nullptr ? party_member::PlayerTypeOf(member) : std::nullopt;
    const auto memberWeak = member != nullptr ? leader_scope::WeakOf(member) : 0;
    focus_scope::FocusScope scope(g_controller);
    if (!scope.Entered())
    {
        refusal = scope.Refusal();
        return SetupResult::Refused;
    }
    std::optional<leader_scope::LeaderScope>     leader;
    std::optional<leader_scope::LeaderTypeScope> type;
    if (memberType && memberWeak != 0)
    {
        leader.emplace(memberWeak);
        type.emplace(*memberType);
    }
    if (!platform::CallGuarded(g_setup, result, g_controller, world))
    {
        scope.Abandon();
        return SetupResult::Faulted;
    }
    return SetupResult::Ran;
}

/// Player 2's controller follows the game's lifecycle: when the game has set its own up again (after a
/// retry resets it) and player 2's is not set up, Setup runs on player 2's (as the game's restart runs
/// Setup on its own). Also logs the "setup done" window on change.
void RepairSetup(void* world) noexcept
{
    const auto one = Field<std::uintptr_t>(g_endUISystem + kGameController);
    const auto two = reinterpret_cast<std::uintptr_t>(g_controller);
    const std::array<std::uint8_t, 3> lifecycle{Field<std::uint8_t>(one + kSetupDone),
                                                Field<std::uint8_t>(g_endUISystem + kSetupDeferred),
                                                Field<std::uint8_t>(two + kSetupDone)};
    if (lifecycle != g_loggedLifecycle)
    {
        g_loggedLifecycle = lifecycle;
        Line(std::format(L"setup done: player 1 {} (a setup deferred: {}), player 2 {}", lifecycle[0], lifecycle[1],
                         lifecycle[2]));
    }
    if (lifecycle[2] != 0)
    {
        g_repairTries = 0;
        return;
    }
    if (one == 0 || lifecycle[0] == 0 || Field<std::uint8_t>(one + kSetupPending) != 0 || g_repairTries >= kSetupTries ||
        g_repairTicks++ % kRepairGap != 0)
    {
        return;
    }
    ++g_repairTries;
    std::uint8_t   set     = 0;
    const wchar_t* refusal = nullptr;
    switch (RunSetup(world, set, refusal))
    {
        case SetupResult::Refused:
            Line(std::format(L"setup repair waits: {} (try {} of {})", refusal, g_repairTries, kSetupTries));
            break;
        case SetupResult::Faulted:
            g_failed = true;
            Line(L"FAILED: the setup repair faulted; player 2's menu stays off");
            break;
        case SetupResult::Ran:
            Line(std::format(L"setup repaired (returned {}): 777={} 776={} 778={} 783={}", set,
                             Field<std::uint8_t>(two + kSetupDone), Field<std::uint8_t>(two + kSelecterSet),
                             Field<std::uint8_t>(two + kSetupOther), Field<std::uint8_t>(two + kMenuLayer)));
            break;
    }
}

/// Allocates and constructs the controller, as EndUISystem__Ctor does for the game's.
bool Build() noexcept
{
    void* memory = nullptr;
    if (!platform::CallGuarded(g_malloc, memory, kControllerSize, std::uint32_t{0}) || memory == nullptr)
    {
        return false;
    }
    std::memset(memory, 0, kControllerSize);
    void* built = nullptr;
    if (!platform::CallGuarded(g_ctor, built, memory))
    {
        return false;  // the memory is left: the ctor may have registered it before faulting
    }
    g_controller = memory;
    Line(std::format(L"built player 2's controller at {:#x}", reinterpret_cast<std::uintptr_t>(memory)));
    // Player 2's key guide, as EndUISystem__Ctor builds the game's. Optional: without it player 2's controller
    // leaves the shared one alone (player_two/command_menu_tick skips its key-guide writes).
    void* guide = nullptr;
    if (platform::CallGuarded(g_malloc, guide, kKeyGuideSize, std::uint32_t{0}) && guide != nullptr)
    {
        std::memset(guide, 0, kKeyGuideSize);
        void* built2 = nullptr;
        if (platform::CallGuarded(g_keyGuideCtor, built2, guide))
        {
            g_keyGuide = guide;
            focus_scope::SetKeyGuide(guide);
        }
    }
    Line(std::format(L"player 2's key guide {}", g_keyGuide != nullptr ? L"built" : L"NOT built (the shared one stays)"));
    return true;
}

Snapshot Read() noexcept
{
    Snapshot   now;
    const auto ctrl = reinterpret_cast<std::uintptr_t>(g_controller);
    for (std::size_t i = 0; i < now.members.size(); ++i)
    {
        now.members[i] = Field<std::int32_t>(ctrl + kMembers + i * 4);
    }
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = Field<std::uint8_t>(ctrl + kBytes[i]);
    }
    const auto weak = Field<std::uint64_t>(ctrl + kMenuWidget);
    if (weak != 0)
    {
        platform::CallGuarded(g_weakGet, now.widget, &weak);
    }
    now.widgetNamesUs = now.widget != nullptr &&
                        Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(now.widget) + kWidgetCtrl) == ctrl;
    now.context = focus_scope::PlayerTwoContext();
    return now;
}

void Watch() noexcept
{
    const Snapshot now = Read();
    if (g_loggedOnce && now == g_logged)
    {
        return;
    }
    g_logged     = now;
    g_loggedOnce = true;
    const auto& b = now.bytes;
    Line(std::format(L"members {} {} {} {} | menu {} (names this controller: {}) | 25C={} 258={} 777={} 783={} 5E1={} "
                     L"540={} | player 2's focus: path {}, layers {}",
                     now.members[0], now.members[1], now.members[2], now.members[3], NameOf(now.widget),
                     YesNo(now.widgetNamesUs), b[0], b[1], b[2], b[3], b[4], b[5], now.context.pathCount,
                     now.context.layerCount));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto malloc = platform::FindFunction(image, kMalloc);
    const auto ctor   = platform::FindFunction(image, kCtor);
    const auto setup  = platform::FindFunction(image, kSetup);
    const auto get    = platform::FindFunction(image, facts::kWeakObjectGet);
    const auto guide  = platform::FindFunction(image, kKeyGuideCtor);
    g_keyGuideCtor    = guide ? reinterpret_cast<KeyGuideCtorFn>(*guide) : nullptr;
    try
    {
        g_worldClass = reinterpret_cast<std::uintptr_t>(
            RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, STR("/Script/Engine.World")));
    }
    catch (...)
    {
        g_worldClass = 0;
    }
    const auto system = image.Rebase(kEndUISystem, kKeyGuideSlot + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    g_installed       = malloc && ctor && setup && get && g_worldClass != 0 && system && focus_scope::Install(image);
    if (g_installed)
    {
        g_malloc  = reinterpret_cast<MallocFn>(*malloc);
        g_ctor    = reinterpret_cast<CtorFn>(*ctor);
        g_setup   = reinterpret_cast<SetupFn>(*setup);
        g_weakGet = reinterpret_cast<WeakGetFn>(*get);
    }
    g_worldWeak = 0;
    g_world     = nullptr;
    g_tries     = 0;
    platform::Log(L"command menu two: {}", g_installed ? L"player 2's command controller is built once player 2 plays (not "
                                                         L"ticked yet; logged)"
                                                       : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Remove() noexcept
{
    g_installed = false;  // the controller stays: the game's GC list refers to it
}

void* Controller() noexcept
{
    return g_controller;
}

void* KeyGuide() noexcept
{
    return g_keyGuide;
}

namespace {

/// The object a FWeakObjectPtr names, or null.
void* Resolve(std::uint64_t weak) noexcept
{
    void* object = nullptr;
    return weak != 0 && g_weakGet != nullptr && platform::CallGuarded(g_weakGet, object, &weak) ? object : nullptr;
}

/// A controller's widget (its FWeakObjectPtr at `offset`), or null.
void* WidgetOf(std::uintptr_t controller, std::uint32_t offset) noexcept
{
    return controller != 0 ? Resolve(Field<std::uint64_t>(controller + offset)) : nullptr;
}

void SetOpacity(void* widget, float opacity) noexcept
{
    if (widget != nullptr && hud::widget_opacity::Get(widget) != opacity)
    {
        hud::widget_opacity::Set(widget, opacity);
    }
}

}  // namespace

void ShowOnly(int player) noexcept
{
    if (g_controller == nullptr || g_endUISystem == 0 || !hud::widget_opacity::Installed() || !generation::OnGameThread())
    {
        return;
    }
    const auto two = reinterpret_cast<std::uintptr_t>(g_controller);
    const auto one = Field<std::uintptr_t>(g_endUISystem + kGameController);
    if (player != 1 || Field<std::uint8_t>(two + kHudMode) == 0 || one == 0)
    {
        return Restore();
    }
    for (std::size_t i = 0; i <= kParts.size(); ++i)
    {
        // The last entry is the key guide: the game's (EndUISystem[53]) and player 2's, each widget at +4.
        const bool guide     = i == kParts.size();
        const auto oneOwner  = guide ? Field<std::uintptr_t>(g_endUISystem + kKeyGuideSlot) : one;
        const auto twoOwner  = guide ? reinterpret_cast<std::uintptr_t>(g_keyGuide) : two;
        const auto offset    = guide ? kKeyGuideWidget : kParts[i].offset;
        const bool borrow    = guide || kParts[i].borrowOpacity;
        void*      oneWidget = WidgetOf(oneOwner, offset);
        void*      twoWidget = WidgetOf(twoOwner, offset);
        if (oneWidget == nullptr || twoWidget == nullptr)
        {
            continue;
        }
        if (!g_swapped)
        {
            g_oneSaved[i] = {Field<std::uint64_t>(oneOwner + offset), hud::widget_opacity::Get(oneWidget)};
        }
        SetOpacity(twoWidget, borrow ? g_oneSaved[i].opacity : 1.0F);
        SetOpacity(oneWidget, 0.0F);
    }
    g_swapped = true;
}


void Restore() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    if (!hud::widget_opacity::Installed())
    {
        return;
    }
    if (g_swapped)
    {
        for (auto& saved : g_oneSaved)
        {
            SetOpacity(Resolve(saved.weak), saved.opacity);  // null (skipped) if the game rebuilt it
            saved = {};
        }
        g_swapped = false;
    }
    for (const Part& part : kParts)
    {
        SetOpacity(WidgetOf(reinterpret_cast<std::uintptr_t>(g_controller), part.offset), 0.0F);
    }
    SetOpacity(WidgetOf(reinterpret_cast<std::uintptr_t>(g_keyGuide), kKeyGuideWidget), 0.0F);
}

void KeepHidden() noexcept
{
    if (g_controller == nullptr || g_weakGet == nullptr || !hud::widget_opacity::Installed() ||
        !generation::OnGameThread())
    {
        return;
    }
    for (const Part& part : std::array<Part, kParts.size() + 1>{kParts[0], kParts[1], kParts[2], Part{kKeyGuideWidget, true}})
    {
        const bool guide  = part.offset == kKeyGuideWidget;
        void*      widget = WidgetOf(reinterpret_cast<std::uintptr_t>(guide ? g_keyGuide : g_controller), part.offset);
        if (widget == nullptr || hud::widget_opacity::Get(widget) == 0.0F)
        {
            continue;
        }
        const bool hidden = hud::widget_opacity::Set(widget, 0.0F);
        if (++g_hides <= 5 || g_hides % 100 == 0)
        {
            Line(std::format(L"widget +{:#x} {} (render opacity 0; {} time(s))", part.offset,
                             hidden ? L"hidden" : L"could NOT be hidden", g_hides));
        }
    }
}

void Tick() noexcept
{
    if (!g_installed || g_failed || !generation::OnGameThread() || !generation::Active())
    {
        return;
    }
    void* world = WorldOf(generation::PlayerOneController());
    if (world == nullptr)
    {
        return;
    }
    if (world != g_world)
    {
        g_world     = world;
        g_worldWeak = 0;
        g_tries     = 0;
    }
    if (g_worldWeak != 0 && leader_scope::StillNames(g_worldWeak, world))
    {
        if (g_controller != nullptr)
        {
            RepairSetup(world);
            Watch();
        }
        return;
    }
    if (g_tries >= kSetupTries)
    {
        return;
    }
    if (g_controller == nullptr && !Build())
    {
        g_failed = true;
        Line(L"FAILED to build player 2's controller; player 2's menu stays off");
        return;
    }
    ++g_tries;
    std::uint8_t   set     = 0;
    const wchar_t* refusal = nullptr;
    const auto     setup   = RunSetup(world, set, refusal);
    if (setup == SetupResult::Refused)
    {
        Line(std::format(L"setup waits: {} (try {} of {})", refusal, g_tries, kSetupTries));
        return;
    }
    if (setup == SetupResult::Faulted)
    {
        g_failed = true;
        Line(L"FAILED: the setup faulted; player 2's menu stays off");
        return;
    }
    g_worldWeak  = leader_scope::WeakOf(world);
    if (g_worldWeak == 0)
    {
        g_failed = true;  // without it a new world could not be told apart from this one
        Line(L"FAILED: the world could not be named; player 2's menu stays off");
        return;
    }
    g_loggedOnce = false;
    // Once per world (an object walk is slow): a second Setup must not leave extra menus behind.
    Line(std::format(L"set up for world '{}' (returned {}); command menu widgets now: {}", NameOf(world), set,
                     CountMenus()));
    Watch();
}

}  // namespace lss::player_two::command_menu_two
