#include "battle/summon_seats.h"

#include "battle/command_menu.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/seats.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::battle::summon_seats {

namespace {

using ReaderFn = std::uint8_t (*)(void* controller);

// The two readers of the controller's own summon gauge widget: bool(controller), the widget's "gauge shown" and
// "summon on the field" bytes. Their first instruction is a 2-byte push, so their calls are redirected, not their entry.
constexpr std::uint8_t kGaugeShownBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                             0x70, 0x99, 0x41, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kGaugeShown{L"FieldCommandCtrl_IsSummonGaugeShown_W744_3EC", 0x140C955B4,
                                             kGaugeShownBytes};
constexpr std::uint8_t kOnFieldBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                          0xA8, 0x99, 0x41, 0x01, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kOnField{L"FieldCommandCtrl_IsSummonOnField_W744_3ED", 0x140C9557C, kOnFieldBytes};

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
};
constexpr std::array kSites{
    Site{0x140988321, Reader::GaugeShown},  // the summon-ready test
    Site{0x140D2342A, Reader::GaugeShown},  // the Summon row's check
    Site{0x140D23212, Reader::OnField},     // the row count
    Site{0x141089CB1, Reader::OnField},     // the top rows' cell setup
    Site{0x14165CA93, Reader::OnField},     // the row builder: the summon's commands, else Summon
    Site{0x1418EFD06, Reader::OnField},     // the summon icon's state
};

ReaderFn                                          g_shown = nullptr, g_onField = nullptr;
std::array<platform::CallRedirect, kSites.size()> g_calls;

// Game thread: each seat's last answer per reader, for the log (0 not yet asked, 1 no, 2 yes, 3 player 1's not asked).
using Said = std::array<int, players::kMaxPlayers>;
Said g_shownSaid{};
Said g_onFieldSaid{};

/// The reader's answer for `controller`: a seat's asks with player 1's controller, the holder of the party gauge's state.
std::uint8_t Answer(ReaderFn original, void* controller, Said& said, const wchar_t* what) noexcept
{
    const int seat = command_menu::SeatOfController(controller);
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

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto shown   = platform::FindFunction(image, kGaugeShown);
    const auto onField = platform::FindFunction(image, kOnField);
    bool       ok      = shown.has_value() && onField.has_value();
    if (ok)
    {
        g_shown   = reinterpret_cast<ReaderFn>(*shown);
        g_onField = reinterpret_cast<ReaderFn>(*onField);
    }
    for (std::size_t i = 0; ok && i < kSites.size(); ++i)
    {
        const bool          isShown = kSites[i].reader == Reader::GaugeShown;
        const std::uintptr_t target = isShown ? *shown : *onField;
        const auto replacement = reinterpret_cast<std::uintptr_t>(isShown ? &GaugeShown : &OnField);
        ok                     = g_calls[i].Install(image, kSites[i].at, target, replacement);
    }
    if (!ok)
    {
        Remove();
        platform::LogWarning(L"summon seats: a reader or one of its calls is not as the research read it; only player 1 "
                             L"can summon");
        return false;
    }
    platform::Log(L"summon seats: installed ({} calls); players 2 and 3's menus offer Summon with the party's gauge, as "
                  L"player 1's",
                  kSites.size());
    return true;
}

void Remove() noexcept
{
    for (platform::CallRedirect& call : g_calls)
    {
        if (call.IsInstalled())
        {
            (void)call.Restore();
        }
    }
    g_shownSaid   = {};
    g_onFieldSaid = {};
}

}  // namespace lss::battle::summon_seats
