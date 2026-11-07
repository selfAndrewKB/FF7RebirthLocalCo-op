#include "hud/view_prompt.h"

#include "facts/engine.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/unreal_access.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace lss::hud::view_prompt {

namespace {

using RC::Unreal::UFunction;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;
namespace unreal         = players::unreal;

using PostRenderFn = void (*)(void* hud);

constexpr int kPlayers = players::kMaxPlayers;

// The look: a dark translucent box in the lower middle of the view, white lines, one in a warm highlight. Sizes are
// for a 1080-pixel-high view and scale with the view's height.
constexpr float kBox[4]       = {0.0f, 0.0f, 0.0f, 0.65f};
constexpr float kText[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr float kHighlight[4] = {1.0f, 0.85f, 0.35f, 1.0f};
constexpr float kScaleAt1080  = 3.2f;   // the font's scale in a 1080-pixel-high view (2.2 reads small from a couch)
constexpr float kMinScale     = 1.6f;
constexpr float kLineAt1      = 20.0f;  // a line's height at scale 1, before the text size is read
constexpr float kPadAt1       = 8.0f;
constexpr float kBoxWidth     = 0.70f;  // of the view's width, centred
constexpr float kBoxTop       = 0.60f;  // of the view's height, raised when a taller box would pass kBoxBottom
constexpr float kBoxBottom    = 0.95f;

/// A reflected HUD function and its parameters' offsets, found by name (a different layout is refused, never written).
struct DrawRectCall
{
    UFunction*   function = nullptr;
    std::int32_t color = -1, x = -1, y = -1, w = -1, h = -1;
};
struct DrawTextCall
{
    UFunction*   function = nullptr;
    std::int32_t text = -1, color = -1, x = -1, y = -1, font = -1, scale = -1;
};
struct TextSizeCall
{
    UFunction*   function = nullptr;
    std::int32_t text = -1, width = -1, height = -1, font = -1, scale = -1;
};

// Set at install, then only read.
platform::VTableEntry g_entry;
PostRenderFn          g_postRender = nullptr;
std::uintptr_t        g_engine     = 0;  // &GEngine
DrawRectCall          g_rect;
DrawTextCall          g_text;
TextSizeCall          g_size;
bool                  g_installed = false;

// Game thread.
std::array<Prompt, kPlayers> g_prompts{};
std::array<bool, kPlayers>   g_shown{};
std::array<bool, kPlayers>   g_logged{};  // the first draw in that view logged
float                        g_lineAt1      = kLineAt1;
bool                         g_lineMeasured = false;  // g_lineAt1 read from the font (once)
std::uint64_t                g_draws = 0, g_noFont = 0, g_noCanvas = 0;

std::int32_t Offset(UFunction* function, const wchar_t* name, std::int32_t size) noexcept
{
    const auto offset = unreal::ParamOffset(function, name, size);
    return offset ? *offset : -1;
}

UFunction* Find(const wchar_t* path) noexcept
{
    try
    {
        return UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path);
    }
    catch (...)
    {
        return nullptr;
    }
}

/// The reflected `HUD:DrawRect`, `HUD:DrawText` and `HUD:GetTextSize` and their parameters, by name: once, at the first
/// prompt shown (reflection is up by then). False without DrawText.
bool g_lookedUp = false;
bool LookUp() noexcept
{
    if (std::exchange(g_lookedUp, true))
    {
        return g_text.function != nullptr;
    }
    constexpr auto kString = static_cast<std::int32_t>(sizeof(RC::Unreal::FString));
    if (UFunction* f = Find(STR("/Script/Engine.HUD:DrawRect")); f != nullptr)
    {
        g_rect = {f, Offset(f, STR("RectColor"), 16), Offset(f, STR("ScreenX"), 4), Offset(f, STR("ScreenY"), 4),
                  Offset(f, STR("ScreenW"), 4), Offset(f, STR("ScreenH"), 4)};
        if (g_rect.color < 0 || g_rect.x < 0 || g_rect.y < 0 || g_rect.w < 0 || g_rect.h < 0 || f->GetParmsSize() > 64)
        {
            g_rect = {};
        }
    }
    if (UFunction* f = Find(STR("/Script/Engine.HUD:DrawText")); f != nullptr)
    {
        g_text = {f, Offset(f, STR("Text"), kString), Offset(f, STR("TextColor"), 16), Offset(f, STR("ScreenX"), 4),
                  Offset(f, STR("ScreenY"), 4), Offset(f, STR("Font"), 8), Offset(f, STR("Scale"), 4)};
        if (g_text.text < 0 || g_text.color < 0 || g_text.x < 0 || g_text.y < 0 || g_text.font < 0 || g_text.scale < 0)
        {
            g_text = {};
        }
    }
    if (UFunction* f = Find(STR("/Script/Engine.HUD:GetTextSize")); f != nullptr)
    {
        g_size = {f, Offset(f, STR("Text"), kString), Offset(f, STR("OutWidth"), 4), Offset(f, STR("OutHeight"), 4),
                  Offset(f, STR("Font"), 8), Offset(f, STR("Scale"), 4)};
        if (g_size.text < 0 || g_size.width < 0 || g_size.height < 0 || g_size.font < 0 || g_size.scale < 0)
        {
            g_size = {};
        }
    }
    if (g_text.function == nullptr)
    {
        platform::LogWarning(L"view prompt: the HUD's DrawText was not found as expected; no prompt can be drawn");
    }
    else if (g_rect.function == nullptr)
    {
        platform::Log(L"view prompt: the HUD's DrawRect was not found as expected; prompts are drawn without their box");
    }
    return g_text.function != nullptr;
}

void DrawRect(void* hud, float x, float y, float w, float h) noexcept
{
    if (g_rect.function == nullptr)
    {
        return;
    }
    alignas(16) std::uint8_t frame[64]{};
    std::memcpy(frame + g_rect.color, kBox, sizeof(kBox));
    std::memcpy(frame + g_rect.x, &x, 4);
    std::memcpy(frame + g_rect.y, &y, 4);
    std::memcpy(frame + g_rect.w, &w, 4);
    std::memcpy(frame + g_rect.h, &h, 4);
    unreal::GuardedProcessEvent(static_cast<UObject*>(hud), g_rect.function, frame);
}

/// One line at (x, y) in `color` and `scale`. With `measure`, its height in pixels from the text size (0 if it could
/// not be read); else 0.
float DrawLine(void* hud, const std::wstring& line, float x, float y, const void* font, const float* color, float scale,
               bool measure) noexcept
{
    unreal::CallWithString(
        static_cast<UObject*>(hud), g_text.function, g_text.text, line.c_str(),
        [&](std::uint8_t* frame) {
            std::memcpy(frame + g_text.color, color, 16);
            std::memcpy(frame + g_text.x, &x, 4);
            std::memcpy(frame + g_text.y, &y, 4);
            std::memcpy(frame + g_text.font, &font, 8);
            std::memcpy(frame + g_text.scale, &scale, 4);
        },
        [](const std::uint8_t*) {});
    float height = 0.0f;
    if (measure && g_size.function != nullptr)
    {
        unreal::CallWithString(
            static_cast<UObject*>(hud), g_size.function, g_size.text, line.c_str(),
            [&](std::uint8_t* frame) {
                std::memcpy(frame + g_size.font, &font, 8);
                std::memcpy(frame + g_size.scale, &scale, 4);
            },
            [&](const std::uint8_t* frame) { std::memcpy(&height, frame + g_size.height, 4); });
    }
    return height > 0.0f && height < 4096.0f ? height : 0.0f;
}

/// The HUD's player: a seat's by its controller, else player 1's (a mirror's HUD too: see the header).
int SeatOf(const void* controller) noexcept
{
    const int seat = controller != nullptr ? players::generation::SeatOfController(controller) : -1;
    return seat >= 1 ? seat : 0;
}

/// After the game's own HUD pass of one player's view: that player's prompt, if any.
void Draw(void* hud) noexcept
{
    void* const canvas = platform::ReadPointer(hud, facts::kHudCanvas);
    const int   seat   = SeatOf(platform::ReadPointer(hud, facts::kHudOwner));
    if (!g_shown[static_cast<std::size_t>(seat)])
    {
        return;
    }
    float clipX = 0, clipY = 0;
    if (canvas == nullptr || !platform::Read(canvas, facts::kCanvasClipX, clipX) ||
        !platform::Read(canvas, facts::kCanvasClipY, clipY) || !(clipX > 1.0f) || !(clipY > 1.0f))
    {
        ++g_noCanvas;
        return;
    }
    void* const engine = platform::ReadPointerAt(g_engine);
    void* const font   = engine != nullptr ? platform::ReadPointer(engine, facts::kEngineMediumFont) : nullptr;
    if (font == nullptr)
    {
        ++g_noFont;
        return;
    }
    const Prompt& prompt = g_prompts[static_cast<std::size_t>(seat)];
    int           lines  = 0;
    for (const std::wstring& line : prompt.lines)
    {
        lines += line.empty() ? 0 : 1;
    }
    const float scale = std::max(kMinScale, clipY / 1080.0f * kScaleAt1080);
    const float pad   = kPadAt1 * scale;
    const float lineH = g_lineAt1 * scale;
    const float boxX  = clipX * (1.0f - kBoxWidth) / 2.0f;
    const float boxH  = lineH * static_cast<float>(lines) + 2.0f * pad;
    const float boxY  = std::min(clipY * kBoxTop, clipY * kBoxBottom - boxH);
    DrawRect(hud, boxX, boxY, clipX * kBoxWidth, boxH);
    float y = boxY + pad;
    for (int i = 0; i < kLines; ++i)
    {
        const std::wstring& line = prompt.lines[static_cast<std::size_t>(i)];
        if (line.empty())
        {
            continue;
        }
        // The font's real line height is read once (it scales with the text): the box uses it from then on.
        const bool  measure = !g_lineMeasured;
        const float height  = DrawLine(hud, line, boxX + 2.0f * pad, y, font, i == prompt.highlight ? kHighlight : kText,
                                       scale, measure);
        if (height > 0.0f)
        {
            g_lineAt1      = height / scale;
            g_lineMeasured = true;
        }
        y += height > 0.0f ? height : lineH;
    }
    ++g_draws;
    if (!g_logged[static_cast<std::size_t>(seat)])
    {
        g_logged[static_cast<std::size_t>(seat)] = true;
        platform::Log(L"view prompt: drawn in {}'s view ({:.0f} x {:.0f} pixels, text scale {:.2f})", players::SeatName(seat),
                      clipX, clipY, scale);
    }
}

/// The HUD's PostRender (the AHUD vtable's slot): the game's, then the prompt.
void PostRenderFor(void* hud) noexcept
{
    g_postRender(hud);
    try
    {
        bool any = false;
        for (const bool shown : g_shown)
        {
            any = any || shown;
        }
        if (any && hud != nullptr)
        {
            Draw(hud);
        }
    }
    catch (...)
    {
    }
}

#if LSS_DEV
std::wstring Reading()
{
    std::wstring shown;
    for (int seat = 0; seat < kPlayers; ++seat)
    {
        if (g_shown[static_cast<std::size_t>(seat)])
        {
            shown += std::format(L"{}{}", shown.empty() ? L"" : L", ", players::SeatName(seat));
        }
    }
    return std::format(L"{}; prompts in: {}; drawn {}, no font {}, no canvas {}; box {}, text size {}",
                       g_installed ? L"on" : L"not installed", shown.empty() ? L"none" : shown, g_draws, g_noFont,
                       g_noCanvas, g_rect.function != nullptr ? L"the HUD's DrawRect" : L"none (DrawRect not found)",
                       g_size.function != nullptr ? L"read" : L"estimated (GetTextSize not found)");
}
#endif

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto postRender = platform::FindFunction(image, facts::kHudPostRender);
    const auto engine     = image.Rebase(facts::kGEngine, sizeof(void*));
    if (!postRender || !engine)
    {
        platform::Log(L"view prompt: an engine check failed; no prompt can be drawn in a player's view");
        return false;
    }
    g_postRender = reinterpret_cast<PostRenderFn>(*postRender);
    g_engine     = *engine;
    if (!platform::ReplaceSlot(image, facts::kHudPostRenderSlot, reinterpret_cast<std::uintptr_t>(&PostRenderFor), g_entry))
    {
        platform::Log(L"view prompt: the HUD pass could not be taken; no prompt can be drawn in a player's view");
        return false;
    }
    g_installed = true;
    platform::Log(L"view prompt: each player's HUD pass can draw a prompt in that player's view");
    return true;
}

void Remove() noexcept
{
    g_shown = {};
    g_entry.Restore();
    g_installed = false;
}

void Show(int seat, const Prompt& prompt) noexcept
try
{
    if (seat < 0 || seat >= kPlayers || !g_installed || !LookUp())
    {
        return;
    }
    g_prompts[static_cast<std::size_t>(seat)] = prompt;
    g_shown[static_cast<std::size_t>(seat)]   = true;
}
catch (...)
{
}

void ClearAll() noexcept
{
    g_shown = {};
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"views.prompt",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the prompts shown in the players' views (the player windows') and how often drawn",
                       .get   = [] {
                           try
                           {
                               return Value{Reading()};
                           }
                           catch (...)
                           {
                               return Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
    control::Register({.name  = L"view-prompt",
                       .shape = control::Shape::Text,
                       .help  = L"for the checks: `view-prompt <player> <text>` shows the text in that player's view "
                                L"(1-3), `view-prompt off` clears every prompt",
                       .set   = [](const Value& value) {
                           const auto& text = std::get<std::wstring>(value);
                           if (text == L"off")
                           {
                               ClearAll();
                               return Outcome{true, L"no prompt"};
                           }
                           if (text.size() < 3 || text[0] < L'1' || text[0] > L'0' + kPlayers || text[1] != L' ')
                           {
                               return Outcome{false, L"a player 1-3, a space and the text; or off"};
                           }
                           Prompt p;
                           p.lines[0]  = text.substr(2);
                           p.highlight = 0;
                           Show(text[0] - L'1', p);
                           return Outcome{true, L"shown"};
                       },
                       .get = [] { return Value{std::wstring(g_installed ? L"on" : L"not installed")}; }});
}

#endif

}  // namespace lss::hud::view_prompt
