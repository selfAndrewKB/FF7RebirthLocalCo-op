#include "battle/summon_seats.h"

#include "battle/command_menu.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/seats.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::battle::summon_seats {

namespace {

using ReaderFn = std::uint8_t (*)(void* controller);

// The two readers of the controller's own summon gauge widget: bool(controller), the widget's "gauge shown" and
// "summon on the field" bytes. Their first instruction is a 2-byte push, so their calls are redirected, not their entry.
constexpr std::uint8_t kGaugeShownBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                             0xA8, 0x2A, 0x66, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kGaugeShown{L"FieldCommandCtrl_IsSummonGaugeShown_W744_3EC", 0x140B090CC,
                                             kGaugeShownBytes};
constexpr std::uint8_t kOnFieldBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                          0xE0, 0x2A, 0x66, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kOnField{L"FieldCommandCtrl_IsSummonOnField_W744_3ED", 0x140B09094, kOnFieldBytes};

enum class Reader : std::uint8_t
{
    GaugeShown,
    OnField,
};

/// A call of a reader that decides what a command menu offers. The battle log's two calls (it reads the UI system's
/// controller slot, to move the log above the gauge) are left as they are.
struct Site
{
    std::uintptr_t at;
    Reader         reader;
    const wchar_t* what;
};
constexpr std::array kSites{
    Site{0x1417A27A1, Reader::GaugeShown, L"the summon-ready test"},
    Site{0x1411D64E5, Reader::GaugeShown, L"the Summon row's check"},
    Site{0x1411D65BE, Reader::OnField, L"the row count"},
    Site{0x1411B1FB1, Reader::OnField, L"the top rows' cell setup"},
    Site{0x141760C73, Reader::OnField, L"the row builder"},  // the summon's commands, else Summon
    Site{0x14088DBCF, Reader::OnField, L"the summon icon's state"},
};

ReaderFn                                          g_shown = nullptr, g_onField = nullptr;
std::array<platform::CallRedirect, kSites.size()> g_calls;
// Off, every call passes straight to the reader: a call that crosses a block stays placed after Remove (only the thread
// that runs it may put it back, while not at it, and unloading is not that thread).
std::atomic<bool> g_on{false};

// Game thread: each seat's last answer per reader, for the log (0 not yet asked, 1 no, 2 yes, 3 player 1's not asked).
using Said = std::array<int, players::kMaxPlayers>;
Said g_shownSaid{};
Said g_onFieldSaid{};

/// The reader's answer for `controller`: a seat's asks with player 1's controller, the holder of the party gauge's state.
std::uint8_t Answer(ReaderFn original, void* controller, Said& said, const wchar_t* what) noexcept
{
    const int seat = g_on.load(std::memory_order_acquire) ? command_menu::SeatOfController(controller) : 0;
    if (seat < 1)
    {
        return original(controller);
    }
    // Inside the seat's own scope the UI system's slot names the seat's controller: player 1's is the one read outside.
    void* const  playerOne = command_menu::PlayerOneCommandController();
    std::uint8_t answer    = 0;
    const bool   asked     = playerOne != nullptr && platform::CallGuarded(original, answer, playerOne);
    int&         last      = said[static_cast<std::size_t>(seat)];
    const int    now       = !asked ? 3 : answer != 0 ? 2 : 1;
    if (last != now)
    {
        last = now;
        platform::LogDebug(L"summon seats: {}'s menu asks whether {}: {}", players::SeatName(seat), what,
                           !asked ? L"no (player 1's controller could not be asked)"
                           : answer != 0 ? L"yes (player 1's answer)"
                                         : L"no (player 1's answer)");
    }
    return asked ? answer : 0;
}

std::uint8_t GaugeShown(void* controller) noexcept
{
    return Answer(g_shown, controller, g_shownSaid, L"the summon gauge shows");
}

std::uint8_t OnField(void* controller) noexcept
{
    return Answer(g_onField, controller, g_onFieldSaid, L"a summon is on the field");
}

/// Puts the calls back: every one while nothing can run them yet (a failed install), else only those inside one block.
/// Returns how many stay placed (crossing ones left, or changed after us).
std::size_t RestoreCalls(bool beforeAnyRun) noexcept
{
    std::size_t left = 0;
    for (std::size_t i = 0; i < g_calls.size(); ++i)
    {
        platform::CallRedirect& call = g_calls[i];
        if (!call.IsInstalled())
        {
            continue;
        }
        if (!beforeAnyRun && call.Crosses())
        {
            ++left;
        }
        else if (!call.Restore())
        {
            ++left;
            platform::Log(L"summon seats: {}'s call was changed after us; left in place (it passes through)",
                          kSites[i].what);
        }
    }
    return left;
}

/// A site's five bytes as text, for a refusal's log line.
std::wstring SiteBytes(std::uintptr_t live) noexcept
{
    std::array<std::uint8_t, 5> bytes{};
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(live), bytes.size());
    try
    {
        return std::format(L"{:02X} {:02X} {:02X} {:02X} {:02X}", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
    }
    catch (...)
    {
        return L"?";
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto shown   = platform::FindFunction(image, kGaugeShown);
    const auto onField = platform::FindFunction(image, kOnField);
    if (!shown || !onField)
    {
        platform::LogWarning(L"summon seats: {} is not as the research read it; only player 1 can summon",
                             !shown ? kGaugeShown.what : kOnField.what);
        return false;
    }
    g_shown   = reinterpret_cast<ReaderFn>(*shown);
    g_onField = reinterpret_cast<ReaderFn>(*onField);
    std::size_t crossing = 0;
    for (std::size_t i = 0; i < kSites.size(); ++i)
    {
        const bool           isShown     = kSites[i].reader == Reader::GaugeShown;
        const std::uintptr_t target      = isShown ? *shown : *onField;
        const auto           replacement = reinterpret_cast<std::uintptr_t>(isShown ? &GaugeShown : &OnField);
        const auto           live        = image.Rebase(kSites[i].at, 5);
        // on_unreal_init: no battle and no HUD yet, so no thread can be at a call that crosses a block
        // (InstallFromOwnThread's rule; one that fits its block is placed as by Install).
        if (!live)
        {
            RestoreCalls(true);
            platform::LogWarning(L"summon seats: {}'s call is outside the game image; only player 1 can summon",
                                 kSites[i].what);
            return false;
        }
        if (!g_calls[i].InstallFromOwnThread(*live, target, replacement))
        {
            RestoreCalls(true);
            platform::LogWarning(L"summon seats: {}'s call could not be placed (its bytes {}, expected a call of {}; or no "
                                 L"room for its jump near the game's code); only player 1 can summon",
                                 kSites[i].what, SiteBytes(*live), isShown ? kGaugeShown.what : kOnField.what);
            return false;
        }
        crossing += g_calls[i].Crosses() ? 1 : 0;
    }
    g_on.store(true, std::memory_order_release);
    platform::Log(L"summon seats: installed ({} calls, {} across a code block); players 2 and 3's menus offer Summon with "
                  L"the party's gauge, as player 1's",
                  kSites.size(), crossing);
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);
    const std::size_t left = RestoreCalls(false);
    platform::Log(L"summon seats: removed; {} call(s) left in place, passing through (a call across a code block is put "
                  L"back only by the thread that runs it)",
                  left);
}

}  // namespace lss::battle::summon_seats
