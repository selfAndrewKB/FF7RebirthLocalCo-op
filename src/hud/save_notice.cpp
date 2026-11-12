#include "hud/save_notice.h"

#include "facts/engine.h"
#include "facts/hud.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/first_player_scope.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/weak_object.h"
#include "views/whole_screen.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace lss::hud::save_notice {

namespace {

using ShowFn     = void* (*)(void* world, std::uint8_t kind);
using HideFn     = std::uint8_t (*)(std::uint8_t nextKind);
using SetTextFn  = void (*)(void* block, void* text);
using AllocateFn = void* (*)(std::size_t size, std::uint32_t alignment);

/// The engine's string as the setter takes it: the buffer (the setter frees it), the count with the terminator, the
/// capacity.
struct EngineString
{
    wchar_t*     data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};

constexpr std::uint8_t kCheckLook = 0;  // the show's kind: the check's text and look, no text swap later
constexpr std::uint8_t kNoNext    = 0;  // the hide's next kind: nothing after

ShowFn         g_show     = nullptr;
HideFn         g_hide     = nullptr;
SetTextFn      g_setText  = nullptr;
AllocateFn     g_allocate = nullptr;
std::uintptr_t g_system   = 0;  // the UI system object's address

// Game thread. Two states apart: a line waiting for its draw, and the mod's text being on the game's notice; a new
// line while the old one is up keeps the second, so the old one is never taken for the game's own notice.
std::wstring g_text;
int          g_seconds   = 0;      // the line's time, from its first draw
bool         g_pending   = false;  // a line waits for its first draw
std::int64_t g_waitUntil = 0;      // the wait's cap: the game's own notice holding the box past it drops the line
std::int64_t g_until     = 0;      // the drawn line is due until then (0: none)
bool         g_up        = false;  // the mod's text is on the game's notice now

std::int64_t NowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The notice's manager (EndUISystem[35]: the UI system is the global itself, its slots pointers), or null.
const void* Manager() noexcept
{
    return reinterpret_cast<const void*>(ReadAt<std::uintptr_t>(g_system + facts::kUISaveIcon));
}

bool Shown(const void* manager) noexcept
{
    return ReadField<std::uint8_t>(manager, facts::kSaveIconShown) != 0;
}

/// The game's hide; harmless when the notice is down already.
void Hide() noexcept
{
    std::uint8_t hidden = 0;
    (void)platform::CallGuarded(g_hide, hidden, kNoNext);
    g_up = false;
}

/// The game's show with the check's look (skipped while the mod's own line is up: only the text changes), then the
/// mod's text on the widget's block. False (said) when a step failed; the notice is hidden then, so neither a box of
/// the game's after its show nor the mod's old line stays up.
bool Draw() noexcept
{
    // The world through the game state (an actor the title screen has too), else through player 1's controller.
    void* world = const_cast<void*>(players::WorldOf(views::GameStateNow()));
    world       = world != nullptr ? world : const_cast<void*>(players::WorldOf(players::generation::PlayerOneController()));
    if (world == nullptr)
    {
        platform::Log(L"save notice: no world through the game state or player 1's controller; the line is dropped");
        Hide();  // the old line, if up, with it
        return false;
    }
    const bool over  = g_up && Shown(Manager());  // the new line over the mod's own
    void*      shown = nullptr;
    if (!over && !platform::CallGuarded(g_show, shown, world, kCheckLook))
    {
        platform::Log(L"save notice: the game's show faulted; the line is dropped");
        Hide();
        return false;
    }
    const void* manager = Manager();
    void*       widget  = players::weak_object::ObjectAt(manager);
    void*       block   = ReadPointer(widget, facts::kSaveIconText);
    if (block == nullptr || !Shown(manager))
    {
        platform::Log(L"save notice: the game's notice did not come up (shown {}, widget {}, text block {}); the line is dropped",
                      Shown(manager) ? L"yes" : L"no", widget != nullptr ? L"yes" : L"none", block != nullptr ? L"yes" : L"none");
        Hide();
        return false;
    }
    EngineString text;
    text.num  = static_cast<std::int32_t>(g_text.size()) + 1;
    text.max  = text.num;
    text.data = static_cast<wchar_t*>(g_allocate(static_cast<std::size_t>(text.num) * sizeof(wchar_t), 0));
    if (text.data == nullptr)
    {
        platform::Log(L"save notice: no memory for the line; it is dropped");
        Hide();
        return false;
    }
    std::memcpy(text.data, g_text.c_str(), static_cast<std::size_t>(text.num) * sizeof(wchar_t));
    if (!platform::CallGuarded(g_setText, block, static_cast<void*>(&text)))
    {
        platform::Log(L"save notice: the text setter faulted; the line is dropped");
        Hide();
        return false;
    }
    g_up = true;
    if (g_pending)
    {
        g_pending = false;
        g_until   = NowMs() + static_cast<std::int64_t>(g_seconds) * 1000;
        platform::Log(L"save notice: \"{}\" on the game's save-data notice", g_text);
    }
    else
    {
        platform::LogDebug(L"save notice: \"{}\" shown again (the game's own check hid the box early)", g_text);
    }
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto show     = platform::FindFunction(image, facts::kShowSaveIcon);
    const auto hide     = platform::FindFunction(image, facts::kHideSaveIcon);
    const auto setText  = platform::FindFunction(image, facts::kTextBlockSetText);
    const auto allocate = platform::FindFunction(image, facts::kMalloc);
    const auto system   = image.Rebase(facts::kEndUISystem, facts::kUISaveIcon + sizeof(void*));
    g_show     = show ? reinterpret_cast<ShowFn>(*show) : nullptr;
    g_hide     = hide ? reinterpret_cast<HideFn>(*hide) : nullptr;
    g_setText  = setText ? reinterpret_cast<SetTextFn>(*setText) : nullptr;
    g_allocate = allocate ? reinterpret_cast<AllocateFn>(*allocate) : nullptr;
    g_system   = system ? *system : 0;
    const bool ready = g_show != nullptr && g_hide != nullptr && g_setText != nullptr && g_allocate != nullptr && g_system != 0;
    platform::Log(L"save notice: a line of the mod's on the game's save-data notice {}",
                  ready ? L"ready" : L"UNAVAILABLE (a check failed; nothing is shown)");
    if (!ready)
    {
        g_show = nullptr;
    }
    return ready;
}

void Remove() noexcept
{
    if (g_up && g_hide != nullptr)
    {
        if (players::generation::OnGameThread())
        {
            Hide();
        }
        else
        {
            platform::LogDebug(L"save notice: the line is up at the mod's removal off the game thread; the game hides it itself");
        }
    }
    g_up        = false;
    g_pending   = false;
    g_waitUntil = 0;
    g_until     = 0;
    g_show      = nullptr;
}

void Show(std::wstring text, int seconds) noexcept
{
    if (g_show == nullptr)
    {
        return;
    }
    try
    {
        g_text = std::move(text);
    }
    catch (...)
    {
        return;
    }
    g_seconds   = seconds;
    g_pending   = true;
    g_waitUntil = NowMs() + static_cast<std::int64_t>(seconds) * 1000;
    g_until     = 0;  // the new line's own time runs from its draw; the old line, if up, is written over
    Tick();
}

void Tick() noexcept
{
    if (g_show == nullptr || (!g_pending && g_until == 0) || !players::generation::OnGameThread())
    {
        return;
    }
    const std::int64_t now   = NowMs();
    const bool         shown = Shown(Manager());
    if (g_until != 0 && now >= g_until)
    {
        g_until = 0;
        if (g_up)
        {
            Hide();
            platform::LogDebug(L"save notice: the line's time is up; the game's notice hidden");
        }
        return;
    }
    if (g_pending && !g_up && shown)
    {
        // The game's own notice is up (its save check): the line waits, up to its own time.
        if (now >= g_waitUntil)
        {
            g_pending = false;
            platform::LogDebug(L"save notice: the game's own notice held the box past the line's time; the line is dropped");
        }
        return;
    }
    if (!g_pending && g_up && shown)
    {
        return;  // the line is up
    }
    // The first draw, a new line over the old, or the line again after the game's check hid it early.
    if (!Draw())
    {
        g_pending = false;  // a step failed (said): no retry every frame
        g_until   = 0;
    }
}

}  // namespace lss::hud::save_notice
