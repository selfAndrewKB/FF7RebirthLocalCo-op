#include "menu/coop_slider.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/menu.h"
#include "menu/coop_menu_rules.h"
#include "menu/coop_page.h"
#include "menu/game_ui.h"
#include "platform/call_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/generation.h"
#include "players/unreal_access.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

namespace lss::menu::coop_slider {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;

using game_ui::At;
using platform::Read;
using platform::ReadPointer;
using RC::Unreal::UFunction;
using RC::Unreal::UObject;

using MarkerFn     = void (*)(void* page, void* gauge, const std::uint8_t* info);
using AppendIntFn  = void* (*)(void* out, std::int32_t value);
using ActionDownFn = std::uintptr_t (*)(void* page, void* a, void* b, void* event);

/// The reflected calls the slider makes, found at install.
struct Calls
{
    UFunction* setDesiredCount = nullptr;
    UFunction* setScrollOffset = nullptr;
    UFunction* getScrollOffset = nullptr;
    UFunction* setText         = nullptr;
};

// Set at install and on the first tick, then only read.
Calls                  g_calls;
std::uintptr_t         g_marker = 0, g_appendInt = 0, g_openSite = 0, g_stepSite = 0, g_rowSite = 0;
std::uintptr_t         g_keyLeft = 0, g_keyRight = 0;  // the d-pad keys' FKey globals (live addresses)
platform::CallRedirect g_open, g_step, g_row;
platform::VTableEntry  g_hold;
bool                   g_ready = false;

// Game thread.
bool  g_failSaid = false;  // a failed marker or label write logged for this open
float g_shift    = 0.0f;   // how far right this open moved the arrows (the marker follows)

/// The arrows' box (the left arrow, the strip and the right arrow) as the game placed it, read once per box: each open
/// sets it from there, so it never drifts. Every gauge is the same Blueprint, so a box at a reused address has the same
/// place.
struct ArrowsBox
{
    void* slot = nullptr;
    float left = 0.0f, top = 0.0f;
};
ArrowsBox g_arrows;

/// SetScrollOffset's parameters: the offset and the animate flag right after it, 5 bytes as the function reads them
/// (read live); the frame is the struct, padded to 8.
struct ScrollParams
{
    float offset  = 0.0f;
    bool  animate = false;
};
static_assert(offsetof(ScrollParams, animate) == 4);
constexpr std::int32_t kScrollParamsSize = 5;

bool Call(void* object, UFunction* function, void* params) noexcept
{
    return object != nullptr && function != nullptr &&
           players::unreal::GuardedProcessEvent(static_cast<UObject*>(object), function, params);
}

bool SetText(void* block, const wchar_t* text) noexcept
{
    // An FString parameter as ProcessEvent's frame holds it: a view over the mod's text (the call copies it; nothing
    // here is freed by the game).
    const auto          length = static_cast<std::int32_t>(std::wcslen(text)) + 1;
    platform::GameArray params{const_cast<wchar_t*>(text), length, length};
    return Call(block, g_calls.setText, &params);
}

void ClassTextRaw(void* object, wchar_t (&out)[64])
{
    const auto name = static_cast<UObject*>(object)->GetClassPrivate()->GetName();
    wcsncpy_s(out, name.c_str(), _TRUNCATE);
}

/// An object's class name (under a guard: a raw pointer read from the game), for a log line.
bool ClassText(void* object, wchar_t (&out)[64]) noexcept
{
    __try
    {
        ClassTextRaw(object, out);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The gauge's arrows and strip moved right by `shift` from where the game placed their box, through the box's canvas
/// slot (menu/coop_menu_rules.h LabelShift). False if the box or its slot is not the one the facts describe.
bool MoveArrows(void* gauge, float shift) noexcept
{
    void* const arrow     = ReadPointer(gauge, facts::kGaugeArrowLeft);
    void* const arrowSlot = arrow != nullptr ? players::actor_fields::WidgetSlot(arrow) : nullptr;
    void* const box       = arrowSlot != nullptr ? ReadPointer(arrowSlot, facts::kPanelSlotParent) : nullptr;
    void* const boxSlot   = box != nullptr ? players::actor_fields::WidgetSlot(box) : nullptr;
    if (!game_ui::IsCanvasSlot(boxSlot))
    {
        return false;
    }
    if (boxSlot != g_arrows.slot)
    {
        ArrowsBox read{boxSlot};
        if (!Read(At(boxSlot, facts::kCanvasSlotLeft), read.left) || !Read(At(boxSlot, facts::kCanvasSlotTop), read.top))
        {
            return false;
        }
        g_arrows = read;
        platform::LogDebug(L"Co-op slider: the arrows' box at ({:.1f}, {:.1f}), moved right by {:.1f} for the words",
                           read.left, read.top, shift);
    }
    return game_ui::SetSlotVector(boxSlot, game_ui::SlotVector::Position, g_arrows.left + shift, g_arrows.top);
}

/// The window, the marker and the label of the gauge the game has just updated: the mod's sliders only.
void Fit(void* page, void* gauge, const std::uint8_t* info, bool opened) noexcept
{
    std::uint8_t category = 0;
    std::int32_t min = 0, max = 0, value = 0;
    if (!coop_page::IsPage(page) || gauge == nullptr || gauge != ReadPointer(page, facts::kOptionsGauge) || info == nullptr ||
        !Read(info + facts::kOptionInfoCategory, category) || !Read(info + facts::kOptionInfoMin, min) ||
        !Read(info + facts::kOptionInfoMax, max) || !Read(At(page, facts::kOptionsEditValue), value))
    {
        return;
    }
    const auto option = rules::OptionOf(category);
    if (!option)
    {
        return;
    }
    const settings::Option& row   = *settings::kOptions[static_cast<std::size_t>(*option)];
    void* const             strip = ReadPointer(page, facts::kOptionsGaugeStrip);
    const int               cells = max - min, marker = value - min;
    if (!rules::IsSlider(row) || strip == nullptr || cells <= 0 || marker < 0 || marker > cells || value < 0 ||
        value >= static_cast<int>(row.wordLabels.size()))
    {
        return;
    }
    if (opened)
    {
        g_failSaid = false;
    }
    // The window: the stock 10 cells, the call the game's shop list makes (once per open); then scrolled to keep the
    // marker in view.
    if (opened)
    {
        std::int32_t desired = cells > rules::kSliderWindow ? rules::kSliderWindow : 0;
        Call(strip, g_calls.setDesiredCount, &desired);
    }
    float offset = 0.0f;
    Call(strip, g_calls.getScrollOffset, &offset);
    const int    was   = static_cast<int>(std::lround(offset / facts::kGaugePitch));
    const int    first = rules::FirstVisible(marker, was, cells, rules::kSliderWindow);
    ScrollParams to{static_cast<float>(first) * facts::kGaugePitch, false};
    Call(strip, g_calls.setScrollOffset, &to);
    // Room for the words: the arrows and the strip moved right once per open; the marker follows only if they moved.
    bool moved = true;
    if (opened)
    {
        const float shift = rules::LabelShift(row);
        moved             = MoveArrows(gauge, shift) || shift == 0.0f;  // set every open: a shift of 0 puts them back
        g_shift           = moved ? shift : 0.0f;
    }
    // The marker, by the setter the game placed it with (Square's canvas slot): its x within the window, its y as the
    // game left it.
    void* const head   = ReadPointer(gauge, facts::kGaugeHead);
    void* const slot   = head != nullptr ? players::actor_fields::WidgetSlot(head) : nullptr;
    const bool  canvas = game_ui::IsCanvasSlot(slot);
    float       y      = 0.0f;
    const float x      = static_cast<float>(marker - first) * facts::kGaugePitch + facts::kGaugeHeadX + g_shift;
    const bool  placed = canvas && Read(At(slot, facts::kCanvasSlotTop), y) &&
                        game_ui::SetSlotVector(slot, game_ui::SlotVector::Position, x, y);
    const wchar_t* const word    = row.wordLabels[static_cast<std::size_t>(value)];
    const bool           labeled = SetText(ReadPointer(gauge, facts::kGaugeText), word);
    const bool           failed  = !placed || !labeled || !moved;
    if (!placed && !g_failSaid)
    {
        wchar_t headClass[64] = L"?", slotClass[64] = L"?";
        if (head != nullptr)
        {
            ClassText(head, headClass);
        }
        if (slot != nullptr)
        {
            ClassText(slot, slotClass);
        }
        platform::LogDebug(L"Co-op slider: the marker: head {} ({}), its slot {} ({}), canvas {}", head, headClass, slot,
                           slotClass, canvas);
    }
    if (opened || (failed && !g_failSaid))
    {
        g_failSaid = g_failSaid || failed;
        platform::LogDebug(L"Co-op slider: {} {} at {} (cell {} of {}), its window from cell {}{}{}{}", row.key,
                           opened ? L"opened" : L"stepped", word, marker, cells, first,
                           placed ? L"" : L"; the marker not moved", labeled ? L"" : L"; the label not written",
                           moved ? L"" : L"; the arrows not moved (the word may touch them)");
    }
    else if (platform::Enabled(platform::Level::Trace))
    {
        platform::LogTrace(L"Co-op slider: {} at {} (cell {} of {}), its window from cell {}", row.key, word, marker, cells,
                           first);
    }
}

/// The game's gauge update at the press (every row kind; the open of a Range row).
void Opened(void* page, void* gauge, const std::uint8_t* info)
{
    reinterpret_cast<MarkerFn>(g_open.Original())(page, gauge, info);
    Fit(page, gauge, info, true);
}

/// The game's gauge update after each Left/Right step.
void Stepped(void* page, void* gauge, const std::uint8_t* info)
{
    reinterpret_cast<MarkerFn>(g_step.Original())(page, gauge, info);
    Fit(page, gauge, info, false);
}

/// A Range row's value text, appended into the empty string `out`: the value's word for the mod's sliders (the row's
/// value read just answered by the page), else the game's number.
void* RowValueText(void* out, std::int32_t value)
{
    const auto option = coop_page::TakeAnswered(value);
    if (option && out != nullptr)
    {
        const settings::Option& row = *settings::kOptions[static_cast<std::size_t>(*option)];
        if (rules::IsSlider(row) && value >= 0 && value < static_cast<std::int32_t>(row.wordLabels.size()) &&
            game_ui::AssignText(out, row.wordLabels[static_cast<std::size_t>(value)]))
        {
            return out;
        }
    }
    return reinterpret_cast<AppendIntFn>(g_row.Original())(out, value);
}

/// The repeat flag of a held d-pad Left/Right on the mod's page while one of its sliders is open; null otherwise.
std::uint8_t* HeldStep(void* page, void* event) noexcept
{
    std::uint8_t  state = 0, repeat = 0;
    std::uint64_t key = 0, left = 0, right = 0;
    if (!g_ready || event == nullptr || !coop_page::IsPage(page) || !Read(At(page, facts::kOptionsState), state) ||
        state != facts::kOptionsStateRangeEdit || !Read(At(event, facts::kActionEventRepeat), repeat) || repeat != 1 ||
        !Read(At(event, facts::kActionEventKey), key) || !Read(g_keyLeft, left) || !Read(g_keyRight, right) ||
        (key != left && key != right))
    {
        return nullptr;
    }
    return At(event, facts::kActionEventRepeat);
}

/// The page's action handler: a held Left/Right on the mod's open slider is passed as a press, so the game steps it at
/// the input's own repeat rate, as its handler does every press. The event is put back after.
std::uintptr_t ActionDown(void* page, void* a, void* b, void* event)
{
    const auto             original = reinterpret_cast<ActionDownFn>(g_hold.Original());
    std::uint8_t* const    repeat   = HeldStep(page, event);
    constexpr std::uint8_t kPress = 0, kRepeat = 1;
    if (repeat == nullptr || !platform::WriteGuarded(repeat, &kPress, sizeof kPress))
    {
        return original(page, a, b, event);
    }
    const std::uintptr_t reply = original(page, a, b, event);
    platform::WriteGuarded(repeat, &kRepeat, sizeof kRepeat);
    return reply;
}

template <typename T>
T* Find(const wchar_t* path)
{
    return RC::Unreal::UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
}

/// The reflected calls, found and checked on the first tick (the script packages loaded). False (logged) if one is
/// missing or shaped otherwise: each call's frame is exactly its parameters, so another shape would read past it.
bool LookUp() noexcept
try
{
    g_calls.setDesiredCount = Find<UFunction>(L"/Script/EndGame.EndVirtualScrollBox:SetDesiredCount");
    g_calls.setScrollOffset = Find<UFunction>(L"/Script/EndGame.EndVirtualScrollBox:SetScrollOffset");
    g_calls.getScrollOffset = Find<UFunction>(L"/Script/EndGame.EndVirtualScrollBox:GetScrollOffset");
    g_calls.setText         = Find<UFunction>(L"/Script/EndGame.EndTextBlock:SetText");
    const auto shaped = [](UFunction* function, std::int32_t bytes) {
        return function != nullptr && function->GetParmsSize() == bytes;
    };
    if (shaped(g_calls.setDesiredCount, 4) && shaped(g_calls.setScrollOffset, kScrollParamsSize) &&
        shaped(g_calls.getScrollOffset, 4) && shaped(g_calls.setText, sizeof(platform::GameArray)))
    {
        return true;
    }
    const auto size = [](UFunction* function) { return function != nullptr ? function->GetParmsSize() : -1; };
    platform::LogWarning(L"Co-op slider: a reflected call is not what the research read (parameter sizes: SetDesiredCount "
                         L"{}, SetScrollOffset {}, GetScrollOffset {}, SetText {}); the long rows stay off the Co-op page",
                         size(g_calls.setDesiredCount), size(g_calls.setScrollOffset), size(g_calls.getScrollOffset),
                         size(g_calls.setText));
    return false;
}
catch (...)
{
    return false;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f          = facts;
    const auto marker    = platform::FindFunction(image, f::kGaugeMarker);
    const auto appendInt = platform::FindFunction(image, f::kAppendInt);
    const auto openSite  = image.Rebase(f::kGaugeMarkerOpenCall, 5);
    const auto stepSite  = image.Rebase(f::kGaugeMarkerStepCall, 5);
    const auto rowSite   = image.Rebase(f::kRowValueTextCall, 5);
    if (!marker || !appendInt || !openSite || !stepSite || !rowSite)
    {
        platform::LogWarning(L"Co-op slider: an engine check failed (gauge {}, AppendInt {}, calls {}{}{}); the long rows "
                             L"stay off the Co-op page",
                             marker ? L"ok" : L"no", appendInt ? L"ok" : L"no", openSite ? L"o" : L"-",
                             stepSite ? L"s" : L"-", rowSite ? L"r" : L"-");
        return false;
    }
    g_marker    = *marker;
    g_appendInt = *appendInt;
    g_openSite  = *openSite;
    g_stepSite  = *stepSite;
    g_rowSite   = *rowSite;
    g_keyLeft   = image.Rebase(f::kKeyDPadLeft, 8).value_or(0);
    g_keyRight  = image.Rebase(f::kKeyDPadRight, 8).value_or(0);
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    // From the game thread (the first tick), outside every function holding the calls: 1.004's gauge calls cross a block.
    g_ready = g_marker != 0 && LookUp() &&
              g_open.InstallFromOwnThread(g_openSite, g_marker, reinterpret_cast<std::uintptr_t>(&Opened)) &&
              g_step.InstallFromOwnThread(g_stepSite, g_marker, reinterpret_cast<std::uintptr_t>(&Stepped)) &&
              g_row.InstallFromOwnThread(g_rowSite, g_appendInt, reinterpret_cast<std::uintptr_t>(&RowValueText));
    if (!g_ready)
    {
        Unplace();
        platform::LogWarning(L"Co-op slider: a call could not be redirected; the long rows stay off the Co-op page");
        return false;
    }
    // Holding Left/Right is its own: without it each press still steps once.
    const bool held = g_keyLeft != 0 && g_keyRight != 0 &&
                      platform::ReplaceSlot(image, facts::kOptionsActionDown, reinterpret_cast<std::uintptr_t>(&ActionDown),
                                            g_hold);
    platform::Log(L"Co-op slider: the long rows are the game's sliders, a window of {} cells scrolled with the value; {}",
                  rules::kSliderWindow,
                  held ? L"a held Left/Right steps on at the input's repeat rate"
                       : L"the page's action handler is not what the research read: one step per press");
    return true;
}

void Unplace() noexcept
{
    g_ready = false;
    g_hold.Restore();
    // A call that crosses a block goes back from the game thread only; from another (an unload) it stays, passing
    // through to the game's function for every page but the mod's (the DLL is pinned).
    if (players::generation::OnGameThread())
    {
        g_arrows = {};
        g_row.Restore();
        g_step.Restore();
        g_open.Restore();
    }
}

bool Ready() noexcept
{
    return g_ready;
}

}  // namespace lss::menu::coop_slider
