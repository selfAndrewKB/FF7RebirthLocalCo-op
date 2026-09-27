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

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
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
using WeakGetFn = void* (*)(const std::uint64_t* weak);

// Set once at install.
MallocFn  g_malloc  = nullptr;
CtorFn    g_ctor    = nullptr;
SetupFn   g_setup   = nullptr;
WeakGetFn g_weakGet = nullptr;
std::uintptr_t g_worldClass = 0;  // UWorld's class

// Game thread. The controller outlives the mod's stages, like the game's.
void*         g_controller = nullptr;
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
    try
    {
        g_worldClass = reinterpret_cast<std::uintptr_t>(
            RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, STR("/Script/Engine.World")));
    }
    catch (...)
    {
        g_worldClass = 0;
    }
    g_installed       = malloc && ctor && setup && get && g_worldClass != 0 && focus_scope::Install(image);
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

void KeepHidden() noexcept
{
    if (g_controller == nullptr || g_weakGet == nullptr || !hud::widget_opacity::Installed() ||
        !generation::OnGameThread())
    {
        return;
    }
    const auto weak   = Field<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g_controller) + kMenuWidget);
    void*      widget = nullptr;
    if (weak == 0 || !platform::CallGuarded(g_weakGet, widget, &weak) || widget == nullptr ||
        hud::widget_opacity::Get(widget) == 0.0F)
    {
        return;
    }
    const bool hidden = hud::widget_opacity::Set(widget, 0.0F);
    if (++g_hides <= 5 || g_hides % 100 == 0)
    {
        Line(std::format(L"menu widget {} (render opacity 0; {} time(s))", hidden ? L"hidden" : L"could NOT be hidden",
                         g_hides));
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
    std::uint8_t set = 0;
    bool         ran = false;
    {
        focus_scope::FocusScope scope(g_controller);
        if (!scope.Entered())
        {
            Line(std::format(L"setup waits: {} (try {} of {})", scope.Refusal(), g_tries, kSetupTries));
            return;
        }
        ran = platform::CallGuarded(g_setup, set, g_controller, world);
        if (!ran)
        {
            scope.Abandon();
        }
    }
    if (!ran)
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
