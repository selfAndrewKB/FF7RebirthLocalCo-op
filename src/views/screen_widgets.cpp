#include "views/screen_widgets.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lss::views::screen_widgets {

namespace {

// RE-p2-menu-census.md E5 (S-MENUEDGE, both builds): UEndUserWidget__AddToRootOverlay_ZOrderPlus10(widget rcx, z edx),
// returning a byte; it stores z at +0x2A8. First instruction mov [rsp+18h], rbx (5 bytes, position-independent, inside
// one 16-byte block in both builds; 1.004's block also holds SetZOrder's tail jump to this entry).
constexpr std::uint8_t   kAddFirst[]    = {0x48, 0x89, 0x5C, 0x24, 0x18};
constexpr platform::FunctionFact kAdd{L"UEndUserWidget__AddToRootOverlay_ZOrderPlus10", 0x140E68B98, kAddFirst};
constexpr std::uint32_t  kZOrder        = 0x2A8;  // i32; -1 once RemoveFromParent took it off the overlay
constexpr std::uint32_t  kVisibility    = 0xC3;   // UWidget::Visibility (ESlateVisibility; the UE4SS header dump, UMG.hpp)
constexpr std::uint8_t   kCollapsed     = 1;
constexpr std::uint8_t   kHidden        = 2;
constexpr std::size_t    kMaxFollowed   = 8;

// The full-screen Blueprint menus, by class name: photo mode (UEndCameraModeMenu, CameraModeWidget). The VR and colosseum
// menus are read from their session's own state instead (views/whole_screen.h): their classes (VR_Top_C, VR_Result_C...)
// do not carry the menu settings' slot names.
constexpr std::array<std::wstring_view, 1> kFullScreen{L"CameraMode"};

using AddFn = char (*)(void* widget, std::int32_t z);

// Set once at install.
platform::EntryRedirect g_add;

// Game thread.
struct Followed
{
    void*         widget = nullptr;
    std::uint64_t weak   = 0;
    std::wstring  name;
};
std::array<Followed, kMaxFollowed> g_followed{};
// Each widget class seen, with whether it is a full-screen menu: the name is built once per class.
struct ClassInfo
{
    std::wstring name;
    bool         fullScreen = false;
};
std::unordered_map<const void*, ClassInfo> g_classes;

const void* ClassOf(void* widget) noexcept
{
    try
    {
        return static_cast<RC::Unreal::UObject*>(widget)->GetClassPrivate();
    }
    catch (...)
    {
        return nullptr;
    }
}

std::wstring NameOfClass(const void* type)
{
    if (type == nullptr)
    {
        return L"?";
    }
    try
    {
        return std::wstring(static_cast<const RC::Unreal::UClass*>(type)->GetName());
    }
    catch (...)
    {
        return L"?";
    }
}

bool FullScreenName(std::wstring_view name) noexcept
{
    for (const std::wstring_view part : kFullScreen)
    {
        if (name.find(part) != std::wstring_view::npos)
        {
            return true;
        }
    }
    return false;
}

using platform::Read;  // the guarded reads: platform/guarded.h

enum class Presence
{
    Gone,    // destroyed or taken off the overlay: no longer followed
    Hidden,  // still in the overlay, Collapsed or Hidden: followed, not counted (it may be shown again without an add)
    Shown,
};

Presence PresenceOf(const Followed& followed) noexcept
{
    std::int32_t z = -1;
    if (!player_two::leader_scope::StillNames(followed.weak, followed.widget) || !Read(followed.widget, kZOrder, z) ||
        z == -1)
    {
        return Presence::Gone;
    }
    std::uint8_t visibility = kCollapsed;
    return Read(followed.widget, kVisibility, visibility) && visibility != kCollapsed && visibility != kHidden
               ? Presence::Shown
               : Presence::Hidden;
}

void Follow(void* widget, const std::wstring& name, std::int32_t z)
{
    for (const Followed& followed : g_followed)
    {
        if (followed.widget == widget)
        {
            return;  // re-added (a new z): already followed
        }
    }
    const std::uint64_t weak = player_two::leader_scope::WeakOf(widget);
    for (Followed& followed : g_followed)
    {
        if (followed.widget == nullptr)
        {
            followed = {widget, weak, name};
            platform::Log(L"screen widgets: {} on screen (z {}): a full-screen menu, single view", name, z);
            return;
        }
    }
    platform::Log(L"screen widgets: {} on screen but {} full-screen menus are already followed; not followed", name,
                  kMaxFollowed);
}

char Add(void* widget, std::int32_t z) noexcept
{
    const char result = reinterpret_cast<AddFn>(g_add.Original())(widget, z);
    if (widget == nullptr || !player_two::generation::OnGameThread())
    {
        return result;
    }
    try
    {
        const void* type = ClassOf(widget);
        auto        it   = g_classes.find(type);
        if (it == g_classes.end())
        {
            ClassInfo info{NameOfClass(type)};
            info.fullScreen = FullScreenName(info.name);
            it              = g_classes.emplace(type, std::move(info)).first;
            platform::LogDebug(L"screen widgets: first add of {} (z {}){}", it->second.name, z,
                               it->second.fullScreen ? L": a full-screen menu" : L"");
        }
        else
        {
            platform::LogTrace(L"screen widgets: {} added (z {})", it->second.name, z);
        }
        if (it->second.fullScreen)
        {
            Follow(widget, it->second.name, z);
        }
    }
    catch (...)
    {
    }
    return result;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto add       = platform::FindFunction(image, kAdd);
    const bool installed = add &&
                           g_add.Install(*add, image.EntryInstruction(kAdd.address, kAdd.entryBytes),
                                         reinterpret_cast<std::uintptr_t>(&Add));
    platform::Log(L"screen widgets: {}",
                  installed ? L"the game's widget adds are logged; photo mode gives the single view"
                            : L"the widget add is not what the research read; Blueprint full-screen menus stay split");
    return installed;
}

bool FullScreenOpen() noexcept
{
    bool open = false;
    for (Followed& followed : g_followed)
    {
        if (followed.widget == nullptr)
        {
            continue;
        }
        const Presence presence = PresenceOf(followed);
        if (presence != Presence::Gone)
        {
            open = open || presence == Presence::Shown;
            continue;
        }
        platform::Log(L"screen widgets: {} off screen", followed.name);
        followed = {};
    }
    return open;
}

}  // namespace lss::views::screen_widgets
