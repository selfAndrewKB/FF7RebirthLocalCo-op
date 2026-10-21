#include "player_two/widget_owner.h"

#include "hud/widget_opacity.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/command_menu_two.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two::widget_owner {

namespace {

// Facts read from the database (RE-p2-party-list-views.md §8.1; this build).
constexpr std::uint8_t kCellTickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                           0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::SlotFact kCellTick{L"the party-list cell's NativeTick slot (102)", 0x146748958,
                                       {L"UEndMenuStatusPaneCell_NativeTick", 0x140F3D0CC, kCellTickBytes}};
constexpr std::uint8_t kGuideTickBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xAC,
                                            0x6E, 0x17, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::FunctionFact kGuideTick{L"EndUI__M1A8_Tick", 0x140F38078, kGuideTickBytes};
constexpr std::uintptr_t         kGuideTickCall = 0x140F37F48;  // in MasterHUD_Tick, with EndUISystem[53]
// The synergy pane (UEndCmdLinkupPane, one in each controller's command menu; RE-p2-synergy-pane.md): its
// NativeTick rebuilds its rows whenever the party leader's type (PM+0x150) differs from the character it
// shows (+0x3D0). Its owner is recorded at menu setup (+0x3C8, the controller).
constexpr std::uint8_t kPaneTickBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xBC,
                                           0x04, 0x08, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xD9};
constexpr platform::SlotFact kPaneTick{L"the synergy pane's NativeTick slot (102)", 0x146725260,
                                       {L"UEndCmdLinkupPane_NativeTick_RebuildOnPM150", 0x14102EA68, kPaneTickBytes}};
constexpr std::uint32_t      kPaneOwner = 0x3C8;
constexpr std::uint32_t      kPaneType  = 0x3D0;  // u8: the character whose synergy skills it lists

// A cell's owner: its party list (written for every cell by PartyList360_Bind), whose +0x20 is the command
// controller. Logged for player 2's cells: slot, look (0 field, 1 battle, 2 battle with the menu open),
// leader flag, character type and stacked position.
constexpr std::uint32_t kCellPartyList  = 0x7B0;
constexpr std::uint32_t kPartyListOwner = 0x20;
constexpr std::uint32_t kCellSlot       = 0x9E8;  // i32
constexpr std::uint32_t kCellLook       = 0x9EC;  // u8
constexpr std::uint32_t kCellLeader     = 0x9F8;  // u8
constexpr std::uint32_t kCellType       = 0x7B8;  // u8
constexpr std::uint32_t kCellPosition   = 0xA24;  // float x, y
// The controller: its own record of its leader's type (written from the leader type at the end of each of
// its ticks; for the game's controller it equals PM+0x150) and its battle HUD mode (0 out of battle).
constexpr std::uint32_t kStoredLeaderType = 0x30;
constexpr std::uint32_t kHudMode          = 0x540;
// The UI system's slots.
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kController  = 0xB8;   // EndUISystem[23]
constexpr std::uint32_t  kKeyGuide    = 0x1A8;  // EndUISystem[53]
// A key guide (logged): +0 active, +2 the option's cache, +3 shown, +0x14 mode, +4 its widget
// (FWeakObjectPtr); the widget's UWidget::Visibility (u8 at +0xC3).
constexpr std::uint32_t kGuideWidget = 0x04;
constexpr std::uint32_t kGuideMode   = 0x14;
constexpr std::uint32_t kVisibility  = 0xC3;
constexpr std::size_t   kLoggedCells = 4;

using CellTickFn  = void (*)(void* cell, const void* geometry, float deltaTime);
using GuideTickFn = void (*)(void* keyGuide);
using WeakGetFn   = void* (*)(const std::uint64_t* weak);

// Set once at install.
platform::VTableEntry  g_cellSlot;
platform::VTableEntry  g_paneSlot;
platform::CallRedirect g_guideCall;
WeakGetFn              g_weakGet     = nullptr;
std::uintptr_t         g_endUISystem = 0;

// Game thread.
bool          g_cellsOn  = false;
bool          g_guidesOn = false;
bool          g_panesOn  = false;
std::uint64_t g_cellTicks = 0, g_guideTicks = 0, g_paneTicks = 0, g_refusals = 0;

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

/// A line of `level`.
void Line(const std::wstring& text, platform::Level level = platform::Level::Info) noexcept
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"widget owner: {}", text);
    }
}

bool DebugOn() noexcept
{
    return platform::Enabled(platform::Level::Debug);
}

/// True at 1, 10, 100, ...: counters logged without a budget of their own.
bool PowerOfTen(std::uint64_t count) noexcept
{
    while (count >= 10 && count % 10 == 0)
    {
        count /= 10;
    }
    return count == 1;
}

/// A controller's view for its widget's tick: EndUISystem[23] answers the controller and, when given,
/// EndUISystem[53] its key guide; in battle the party leader's type answers the controller's own record
/// of its leader. The slots are put back when it ends.
class OwnerView
{
public:
    OwnerView(std::uintptr_t controller, std::uintptr_t keyGuide) noexcept
    {
        held_ = Enter(0, kController, controller) && (keyGuide == 0 || Enter(1, kKeyGuide, keyGuide));
        if (held_ && Field<std::uint8_t>(controller + kHudMode) != 0)
        {
            type_.emplace(Field<std::uint8_t>(controller + kStoredLeaderType));
            typeRefused_ = !type_->Held();
        }
    }
    ~OwnerView()
    {
        type_.reset();
        for (auto it = slots_.rbegin(); it != slots_.rend(); ++it)
        {
            if (it->address != 0)
            {
                platform::WriteGuarded(reinterpret_cast<void*>(it->address), &it->saved, sizeof(it->saved));
            }
        }
    }
    OwnerView(const OwnerView&)            = delete;
    OwnerView& operator=(const OwnerView&) = delete;

    bool Held() const noexcept { return held_; }
    /// In battle, the leader type could not answer the controller's (the tick still runs in its view).
    bool TypeRefused() const noexcept { return typeRefused_; }

private:
    bool Enter(std::size_t i, std::uint32_t slot, std::uintptr_t value) noexcept
    {
        const auto     address = g_endUISystem + slot;
        std::uintptr_t saved   = 0;
        if (!platform::CopyGuarded(&saved, reinterpret_cast<const void*>(address), sizeof(saved)) ||
            !platform::WriteGuarded(reinterpret_cast<void*>(address), &value, sizeof(value)))
        {
            return false;
        }
        slots_[i] = {address, saved};
        return true;
    }

    struct Saved
    {
        std::uintptr_t address = 0, saved = 0;
    };
    std::array<Saved, 2>                         slots_{};
    std::optional<leader_scope::LeaderTypeScope> type_;
    bool                                         held_        = false;
    bool                                         typeRefused_ = false;
};

void TypeRefused() noexcept
{
    static std::uint64_t count = 0;
    if (++count <= 5 || count % 3600 == 0)
    {
        Line(std::format(L"the leader type could not answer player 2's controller for a widget tick ({} time(s))", count));
    }
}

void Refused(const wchar_t* what) noexcept
{
    if (++g_refusals <= 5 || g_refusals % 3600 == 0)
    {
        Line(std::format(L"{} ticked in the shared view: its owner's view could not be entered ({} time(s))", what,
                         g_refusals));
    }
}

std::uintptr_t OwnerOf(const void* cell) noexcept
{
    const auto list = Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(cell) + kCellPartyList);
    return list != 0 ? Field<std::uintptr_t>(list + kPartyListOwner) : 0;
}

/// Player 2's cells on change (by slot): what the cell tick left in them, and the leader types it compared.
void WatchCell(std::uintptr_t cell, std::uintptr_t two) noexcept
{
    struct Seen
    {
        std::uint8_t look = 0xFF, leader = 0xFF, type = 0xFF, answered = 0xFF;
        bool operator==(const Seen&) const = default;
    };
    static std::array<Seen, kLoggedCells> seen{};
    if (!DebugOn())
    {
        return;
    }
    const auto slot = Field<std::int32_t>(cell + kCellSlot);
    if (slot < 0 || slot >= static_cast<std::int32_t>(kLoggedCells))
    {
        return;
    }
    const Seen now{Field<std::uint8_t>(cell + kCellLook), Field<std::uint8_t>(cell + kCellLeader),
                   Field<std::uint8_t>(cell + kCellType), Field<std::uint8_t>(two + kStoredLeaderType)};
    auto& before = seen[static_cast<std::size_t>(slot)];
    if (now == before)
    {
        return;
    }
    before = now;
    const auto one = Field<std::uintptr_t>(g_endUISystem + kController);
    Line(std::format(L"player 2's cell {:#x} (slot {}): type {} leader {} look {} at ({:.1f}, {:.1f}) | leader type "
                     L"answered {} (player 1's controller's {}) | P2 battle HUD {}",
                     cell, slot, now.type, now.leader, now.look, Field<float>(cell + kCellPosition),
                     Field<float>(cell + kCellPosition + 4), now.answered, Field<std::uint8_t>(one + kStoredLeaderType),
                     Field<std::uint8_t>(two + kHudMode)),
         platform::Level::Debug);
}

/// Replaces the party-list cell's NativeTick: a cell of player 2's controller ticks in that controller's view.
void CellTick(void* cell, const void* geometry, float deltaTime) noexcept
{
    const auto original = reinterpret_cast<CellTickFn>(g_cellSlot.Original());
    const auto two      = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    if (!g_cellsOn || two == 0 || !generation::OnGameThread() || !generation::Active() || OwnerOf(cell) != two)
    {
        return original(cell, geometry, deltaTime);
    }
    std::optional<OwnerView> view(std::in_place, two, 0);
    if (!view->Held())
    {
        view.reset();
        Refused(L"a cell of player 2's party list");
        return original(cell, geometry, deltaTime);
    }
    if (view->TypeRefused())
    {
        TypeRefused();
    }
    const bool ran = platform::CallGuarded(original, cell, geometry, deltaTime);
    view.reset();
    if (!ran)
    {
        g_cellsOn = false;
        Line(L"FAILED: a cell of player 2's party list faulted in its owner's view; the cells tick in the shared view "
             L"from now on");
        return;
    }
    if (PowerOfTen(++g_cellTicks) && DebugOn())
    {
        Line(std::format(L"{} tick(s) of player 2's party-list cells in player 2's view", g_cellTicks),
             platform::Level::Debug);
    }
    WatchCell(reinterpret_cast<std::uintptr_t>(cell), two);
}

/// Replaces the synergy pane's NativeTick: player 2's pane ticks in player 2's view, so its leader-type
/// check answers player 2's member and the pane keeps player 2's synergy skills.
void PaneTick(void* pane, const void* geometry, float deltaTime) noexcept
{
    const auto original = reinterpret_cast<CellTickFn>(g_paneSlot.Original());
    const auto two      = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    const auto at       = reinterpret_cast<std::uintptr_t>(pane);
    if (!g_panesOn || two == 0 || !generation::OnGameThread() || !generation::Active() ||
        Field<std::uintptr_t>(at + kPaneOwner) != two)
    {
        return original(pane, geometry, deltaTime);
    }
    std::optional<OwnerView> view(std::in_place, two, 0);
    if (!view->Held())
    {
        view.reset();
        Refused(L"player 2's synergy pane");
        return original(pane, geometry, deltaTime);
    }
    if (view->TypeRefused())
    {
        TypeRefused();
    }
    const bool ran = platform::CallGuarded(original, pane, geometry, deltaTime);
    view.reset();
    if (!ran)
    {
        g_panesOn = false;
        Line(L"FAILED: player 2's synergy pane faulted in its owner's view; it ticks in the shared view from now on");
        return;
    }
    static std::uint8_t shown = 0xFF;
    const auto          type  = Field<std::uint8_t>(at + kPaneType);
    if ((PowerOfTen(++g_paneTicks) || type != shown) && DebugOn())
    {
        shown = type;
        Line(std::format(L"player 2's synergy pane ticked in player 2's view ({} time(s)); it lists type {} (player 2's "
                         L"controller's leader type {})",
                         g_paneTicks, type, Field<std::uint8_t>(two + kStoredLeaderType)),
             platform::Level::Debug);
    }
}

void* Resolve(std::uint64_t weak) noexcept
{
    void* object = nullptr;
    return weak != 0 && g_weakGet != nullptr && platform::CallGuarded(g_weakGet, object, &weak) ? object : nullptr;
}

/// Both key guides on change: active, the option's cache, shown, mode, and their widget's visibility and
/// render opacity.
void WatchGuides(std::uintptr_t one, std::uintptr_t two) noexcept
{
    using State = std::array<std::uint8_t, 6>;
    static std::array<State, 2> seen{};
    static std::array<float, 2> seenOpacity{-1.0F, -1.0F};
    if (!DebugOn())
    {
        return;
    }
    std::array<State, 2> now{};
    std::array<float, 2> opacity{};
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto guide  = i == 0 ? one : two;
        void*      widget = Resolve(Field<std::uint64_t>(guide + kGuideWidget));
        const auto at     = reinterpret_cast<std::uintptr_t>(widget);
        now[i]     = {Field<std::uint8_t>(guide), Field<std::uint8_t>(guide + 2), Field<std::uint8_t>(guide + 3),
                      Field<std::uint8_t>(guide + kGuideMode), static_cast<std::uint8_t>(widget != nullptr),
                      widget != nullptr ? Field<std::uint8_t>(at + kVisibility) : std::uint8_t{0xFF}};
        opacity[i] = widget != nullptr ? hud::widget_opacity::Get(widget) : -1.0F;
    }
    if (now == seen && opacity == seenOpacity)
    {
        return;
    }
    seen        = now;
    seenOpacity = opacity;
    const auto side = [&](std::size_t i) {
        return std::format(L"active {} option {} shown {} mode {} widget {} visibility {} opacity {:.2f}", now[i][0],
                           now[i][1], now[i][2], now[i][3], now[i][4], now[i][5], opacity[i]);
    };
    Line(std::format(L"key guides: P1 [{}] | P2 [{}]", side(0), side(1)), platform::Level::Debug);
}

/// Replaces MasterHUD_Tick's key-guide tick: the game's key guide, then player 2's in player 2's view.
void GuideTick(void* keyGuide) noexcept
{
    const auto original = reinterpret_cast<GuideTickFn>(g_guideCall.Original());
    original(keyGuide);
    const auto two   = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    const auto guide = reinterpret_cast<std::uintptr_t>(command_menu_two::KeyGuide());
    const auto one   = reinterpret_cast<std::uintptr_t>(keyGuide);
    if (!g_guidesOn || two == 0 || guide == 0 || one == guide || !generation::OnGameThread() || !generation::Active())
    {
        return;
    }
    {
        const OwnerView view(two, guide);
        if (!view.Held())
        {
            return Refused(L"player 2's key guide");
        }
        if (view.TypeRefused())
        {
            TypeRefused();
        }
        if (!platform::CallGuarded(original, reinterpret_cast<void*>(guide)))
        {
            g_guidesOn = false;
            Line(L"FAILED: player 2's key guide faulted in its tick; it is not ticked from now on");
            return;
        }
    }
    if (PowerOfTen(++g_guideTicks) && DebugOn())
    {
        Line(std::format(L"{} tick(s) of player 2's key guide", g_guideTicks), platform::Level::Debug);
    }
    WatchGuides(one, guide);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system  = image.Rebase(kEndUISystem, kKeyGuide + sizeof(void*));
    const auto weakGet = platform::FindFunction(image, facts::kWeakObjectGet);
    const auto guide   = platform::FindFunction(image, kGuideTick);
    const auto site    = image.Rebase(kGuideTickCall, 5);
    if (!system)
    {
        platform::Log(L"widget owner: UNAVAILABLE (a check failed): player 2's widgets tick in the shared view");
        return false;
    }
    g_endUISystem = *system;
    g_weakGet     = weakGet ? reinterpret_cast<WeakGetFn>(*weakGet) : nullptr;  // the key-guide log only
    g_cellsOn     = platform::ReplaceSlot(image, kCellTick, reinterpret_cast<std::uintptr_t>(&CellTick), g_cellSlot);
    g_guidesOn    = guide && site && g_guideCall.Install(*site, *guide, reinterpret_cast<std::uintptr_t>(&GuideTick));
    g_panesOn     = platform::ReplaceSlot(image, kPaneTick, reinterpret_cast<std::uintptr_t>(&PaneTick), g_paneSlot);
    platform::Log(L"widget owner: player 2's party-list cells {}; player 2's key guide {}; player 2's synergy pane {}",
                  g_cellsOn ? L"tick in player 2's view" : L"UNAVAILABLE (a check failed; they tick in the shared view)",
                  g_guidesOn ? L"is ticked in player 2's view" : L"UNAVAILABLE (a check failed; it is not ticked)",
                  g_panesOn ? L"ticks in player 2's view" : L"UNAVAILABLE (a check failed; it ticks in the shared view)");
    return g_cellsOn || g_guidesOn || g_panesOn;
}

void Remove() noexcept
{
    g_cellsOn  = false;
    g_guidesOn = false;
    g_panesOn  = false;
    if (g_cellSlot.Original() != 0 && !g_cellSlot.Restore())
    {
        platform::Log(L"widget owner: the cell tick slot was changed after us; left in place (it passes through)");
    }
    if (g_paneSlot.Original() != 0 && !g_paneSlot.Restore())
    {
        platform::Log(L"widget owner: the synergy pane's tick slot was changed after us; left in place (it passes through)");
    }
    if (g_guideCall.IsInstalled() && !g_guideCall.Restore())
    {
        platform::Log(L"widget owner: the key-guide call was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::player_two::widget_owner
