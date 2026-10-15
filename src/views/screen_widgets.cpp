#include "views/screen_widgets.h"

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>

#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace lss::views::screen_widgets {

namespace {

// RE-p2-menu-census.md E5 (S-MENUEDGE, both builds): UEndUserWidget__AddToRootOverlay_ZOrderPlus10(widget rcx, z edx),
// returning a byte; it stores z at +0x2A8. First instruction mov [rsp+18h], rbx (5 bytes, position-independent, inside
// one 16-byte block in both builds; 1.004's block also holds SetZOrder's tail jump to this entry).
constexpr std::uintptr_t kAdd           = 0x140E68B98;
constexpr std::uint8_t   kAddFirst[]    = {0x48, 0x89, 0x5C, 0x24, 0x18};
constexpr std::uint32_t  kZOrder        = 0x2A8;  // i32; -1 once RemoveFromParent took it off the overlay
constexpr std::uint32_t  kVisibility    = 0xC3;   // UWidget::Visibility (ESlateVisibility; stock UE 4.26 UMG)
constexpr std::uint8_t   kCollapsed     = 1;
constexpr std::uint8_t   kHidden        = 2;
constexpr std::size_t    kMaxFollowed   = 8;

// The full-screen Blueprint menus, by class name: Chadley's VR battle list and the colosseum list (ColosseumMenuWidget,
// VRMenuWidget; their Blueprint classes carry these names), and photo mode (UEndCameraModeMenu, CameraModeWidget).
constexpr std::array<std::wstring_view, 3> kFullScreen{L"VRMenu", L"ColosseumMenu", L"CameraMode"};

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
std::unordered_set<std::wstring>   g_seen;

std::wstring ClassName(void* widget) noexcept
{
    try
    {
        auto* object = static_cast<RC::Unreal::UObject*>(widget);
        auto* type   = object->GetClassPrivate();
        return type != nullptr ? std::wstring(type->GetName()) : std::wstring(L"?");
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

template <class T>
bool Read(const void* widget, std::uint32_t offset, T& out) noexcept
{
    return platform::CopyGuarded(&out, static_cast<const std::byte*>(widget) + offset, sizeof(T));
}

/// Alive (the game's weak resolve), in the overlay and not Collapsed or Hidden.
bool OnScreen(const Followed& followed) noexcept
{
    if (!player_two::leader_scope::StillNames(followed.weak, followed.widget))
    {
        return false;
    }
    std::int32_t z          = -1;
    std::uint8_t visibility = kCollapsed;
    return Read(followed.widget, kZOrder, z) && z != -1 && Read(followed.widget, kVisibility, visibility) &&
           visibility != kCollapsed && visibility != kHidden;
}

void Follow(void* widget, const std::wstring& name, std::int32_t z) noexcept
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
        const std::wstring name = ClassName(widget);
        if (g_seen.insert(name).second)
        {
            platform::LogDebug(L"screen widgets: first add of {} (z {})", name, z);
        }
        else
        {
            platform::LogTrace(L"screen widgets: {} added (z {})", name, z);
        }
        if (FullScreenName(name))
        {
            Follow(widget, name, z);
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
    const auto add       = image.Rebase(kAdd);
    const bool installed = add && image.BytesMatch(kAdd, kAddFirst) &&
                           g_add.Install(*add, image.EntryInstruction(kAdd, kAddFirst), reinterpret_cast<std::uintptr_t>(&Add));
    platform::Log(L"screen widgets: {}",
                  installed ? L"the game's widget adds are logged; the VR / colosseum lists and photo mode give the single view"
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
        if (OnScreen(followed))
        {
            open = true;
            continue;
        }
        platform::Log(L"screen widgets: {} off screen", followed.name);
        followed = {};
    }
    return open;
}

}  // namespace lss::views::screen_widgets
