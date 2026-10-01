#include "player_two/menu_focus.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::menu_focus {

namespace {

// Facts read from the database (RE-p2-command-menu-input.md §1.2, §2, §10; this build).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kInput       = 0x10;  // EndUISystem[2]: Square's input object
constexpr std::uint32_t  kController  = 0xB8;  // EndUISystem[23]: the command controller
// The input object: the focus path (FWeakWidgetPath: TArray<TWeakPtr<SWidget>> first) and the focus
// layers (TArray of 40 bytes: weak layer widget, weak saved focus at +0x10, int32 priority at +0x20).
constexpr std::uint32_t kPath       = 0x08;
constexpr std::uint32_t kLayers     = 0x40;
constexpr std::uint32_t kLayerSize  = 40;
constexpr std::uint32_t kLayerSaved = 0x10;
constexpr std::uint32_t kLayerPrio  = 0x20;
constexpr std::uint32_t kFocusCause = 0x78;
// SWidget::TypeOfWidget (an FName) and the user widget of SObjectWidget (and Square's SEndObjectWidget);
// UObject's class and name.
constexpr std::uint32_t kWidgetType = 0x220;
constexpr std::uint32_t kUserWidget = 0x3A8;
constexpr std::uint32_t kClass      = 0x10;
constexpr std::uint32_t kName       = 0x18;
// The controller's focus bytes: battle input (+0x25C), member selecter layer (+0x258), HUD input
// suspended (+0x777), menu focus layer (+0x783), ready (+0x5E1), battle HUD mode (+0x540).
constexpr std::array<std::uint32_t, 6> kControllerBytes{0x25C, 0x258, 0x777, 0x783, 0x5E1, 0x540};

constexpr std::int32_t kMaxPath   = 16;
constexpr std::int32_t kMaxLayers = 8;
constexpr int          kMaxLines  = 400;

std::uintptr_t g_endUISystem = 0;

struct Layer
{
    std::uintptr_t widget = 0, saved = 0;
    std::int32_t   priority = 0;
    bool operator==(const Layer&) const = default;
};

/// What the log shows, as raw values: only live widgets are kept (a dead one reads 0).
struct Snapshot
{
    std::int32_t                              pathCount = 0, layerCount = 0;
    std::array<std::uintptr_t, kMaxPath>      path{};
    std::array<Layer, kMaxLayers>             layers{};
    std::uint8_t                              cause = 0;
    std::array<std::uint8_t, kControllerBytes.size()> controller{};
    bool operator==(const Snapshot&) const = default;
};

// Game thread.
Snapshot g_logged{};
bool     g_loggedOnce = false;
int      g_lines      = 0;

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

/// The object a TWeakPtr at `address` names, or 0 when it has expired (its shared count is 0).
std::uintptr_t LiveWeak(std::uintptr_t address) noexcept
{
    const auto object     = Field<std::uintptr_t>(address);
    const auto controller = Field<std::uintptr_t>(address + 8);
    return object != 0 && controller != 0 && Field<std::int32_t>(controller + 8) > 0 ? object : 0;
}

/// The FName at `address` (a name read from a freed object can index past the name pool; both the read
/// and the lookup are guarded).
std::wstring NameText(std::uintptr_t address) noexcept
{
    std::uint64_t name = 0;
    if (address == 0 || !platform::CopyGuarded(&name, reinterpret_cast<const void*>(address), sizeof(name)))
    {
        return L"?";
    }
    return players::unreal::NameText(name);
}

/// A widget's Slate type, a UMG host's user-widget class in brackets (a raw pointer: its name is read
/// guarded), and its low address bits.
std::wstring Describe(std::uintptr_t widget)
{
    if (widget == 0)
    {
        return L"-";
    }
    std::wstring text = NameText(widget + kWidgetType);
    if (text == L"SObjectWidget" || text == L"SEndObjectWidget")  // UMG's host, and Square's subclass of it
    {
        const auto user = Field<std::uintptr_t>(widget + kUserWidget);
        const auto cls  = Field<std::uintptr_t>(user + kClass);
        text += L"<" + (cls != 0 ? NameText(cls + kName) : std::wstring(L"?")) + L">";
    }
    return std::format(L"{}@{:04X}", text, widget & 0xFFFF);
}

Snapshot Read() noexcept
{
    Snapshot   now;
    const auto input = Field<std::uintptr_t>(g_endUISystem + kInput);
    if (input != 0)
    {
        const auto path   = Field<std::uintptr_t>(input + kPath);
        now.pathCount     = Field<std::int32_t>(input + kPath + 8);
        for (std::int32_t i = 0; path != 0 && i < std::min(now.pathCount, kMaxPath); ++i)
        {
            now.path[i] = LiveWeak(path + static_cast<std::uintptr_t>(i) * 16);
        }
        const auto layers = Field<std::uintptr_t>(input + kLayers);
        now.layerCount    = Field<std::int32_t>(input + kLayers + 8);
        for (std::int32_t i = 0; layers != 0 && i < std::min(now.layerCount, kMaxLayers); ++i)
        {
            const auto layer = layers + static_cast<std::uintptr_t>(i) * kLayerSize;
            now.layers[i]    = {LiveWeak(layer), LiveWeak(layer + kLayerSaved), Field<std::int32_t>(layer + kLayerPrio)};
        }
        now.cause = Field<std::uint8_t>(input + kFocusCause);
    }
    const auto controller = Field<std::uintptr_t>(g_endUISystem + kController);
    for (std::size_t i = 0; controller != 0 && i < kControllerBytes.size(); ++i)
    {
        now.controller[i] = Field<std::uint8_t>(controller + kControllerBytes[i]);
    }
    return now;
}

std::wstring Format(const Snapshot& now)
{
    std::wstring path;
    for (std::int32_t i = 0; i < std::min(now.pathCount, kMaxPath); ++i)
    {
        path += (i == 0 ? L"" : L" > ") + Describe(now.path[i]);
    }
    std::wstring layers;
    for (std::int32_t i = 0; i < std::min(now.layerCount, kMaxLayers); ++i)
    {
        const auto& layer = now.layers[i];
        layers += std::format(L"{}[{} prio {} saved {}]", i == 0 ? L"" : L" ", Describe(layer.widget), layer.priority,
                              Describe(layer.saved));
    }
    const auto& c = now.controller;
    return std::format(L"menu focus: path {} ({}); layers {} {}; cause {}; ctrl 25C={} 258={} 777={} 783={} 5E1={} 540={}",
                       now.pathCount, path, now.layerCount, layers, now.cause, c[0], c[1], c[2], c[3], c[4], c[5]);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(kEndUISystem, kController + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    g_loggedOnce      = false;
    platform::Log(L"menu focus: {}", system ? L"player 1's UI focus logged on change (read-only)"
                                            : L"UNAVAILABLE (the UI system is outside the image)");
    return system.has_value();
}

void Remove() noexcept
{
    g_endUISystem = 0;
}

void Watch() noexcept
{
    if (g_endUISystem == 0 || !generation::OnGameThread() || g_lines >= kMaxLines)
    {
        return;
    }
    const Snapshot now = Read();
    if (g_loggedOnce && now == g_logged)
    {
        return;
    }
    g_logged     = now;
    g_loggedOnce = true;
    try
    {
        platform::Log(L"{}", Format(now));
    }
    catch (...)
    {
    }
    if (++g_lines == kMaxLines)
    {
        platform::Log(L"menu focus: {} lines logged; the log stops here", kMaxLines);
    }
}

}  // namespace lss::player_two::menu_focus
