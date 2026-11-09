#include "hud/lead_mark.h"

#include <Unreal/NameTypes.hpp>

#include "facts/hud.h"
#include "hud/minimap/column_area_icons.h"
#include "hud/minimap/minimap.h"
#include "hud/passes/menu_buffers.h"
#include "hud/widget_visibility.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "session/lead_gate.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::hud::lead_mark {

namespace {

using players::kSeatsBuilt;
using players::SeatName;

constexpr float         kLineLength   = 60;    // cm: the diamond above a standing point
constexpr float         kVisibleRange = 5000;  // cm: the diamond shows within 50 m of the view's character
constexpr std::uint32_t kCheckEvery   = 10;    // ticks
constexpr float         kMovedCm      = 1;     // a stop further than this from the placed one is a new stop

/// The game's FString as AddIcon copies it: empty.
struct EmptyText
{
    const wchar_t* data = nullptr;
    std::int32_t   num  = 0;
    std::int32_t   max  = 0;
};

/// MapIconParams (RE-lead-mark.md §2.1), 96 bytes: a plain objective mark at a world point.
struct alignas(8) IconParams
{
    float        lineLength   = kLineLength;    // +0
    float        unknown4     = 0.0f;           // +4
    float        area         = 0.0f;           // +8: above 0 an area icon (no diamond)
    float        visibleRange = kVisibleRange;  // +12
    std::uint8_t hidden       = 0;              // +16: the minimap and the compass skip it
    std::uint8_t diamonds     = 1;              // +17: the icon managers draw it
    std::uint8_t byte18       = 1;              // +18
    std::uint8_t minimap      = 1;              // +19: the minimap draws it
    std::uint8_t compass      = 0;              // +20: the compass draws it (off: no far marker either)
    std::uint8_t pad21[3]{};
    std::int32_t layer        = -1;             // +24: the map's bounds
    std::uint8_t mapName[8]{};                  // +28: an FName (unaligned in the block); 0 = the current map
    std::uint8_t rest[60]{};                    // +36..+95: padding, two empty arrays, the radii, no cross-map point
};
static_assert(sizeof(IconParams) == 96 && offsetof(IconParams, visibleRange) == 12 && offsetof(IconParams, hidden) == 16 &&
                  offsetof(IconParams, compass) == 20 && offsetof(IconParams, layer) == 24 && offsetof(IconParams, mapName) == 28,
              "the block as the game reads it");

using AddIconFn    = void (*)(std::uint64_t key, std::uint8_t kind, const float* position, const EmptyText* text, const IconParams* params);
using RemoveIconFn = void (*)(std::uint64_t key);

/// One seat's marker.
struct Seat
{
    std::uint64_t key       = 0;      // LSS_LeadMark_<seat>, made at the first use
    bool          keyFailed = false;  // the name could not be made: no marker for this seat (logged once)
    bool          placed    = false;
    float         at[3]     = {};
    std::uint32_t ticks     = 0;
};
std::array<Seat, kSeatsBuilt + 1> g_seats;  // by seat ([0] unused)
bool                              g_installed = false;
AddIconFn                         g_add       = nullptr;
RemoveIconFn                      g_remove    = nullptr;

/// The FName of `text`, made if new, packed as the game passes it (comparison index low, number high); a free function
/// so a fault is caught by the guard.
void MakeName(const wchar_t* text, std::uint64_t* out) noexcept
try
{
    *out = EngineNameValue(text, RC::Unreal::FNAME_Add);
}
catch (...)
{
    *out = 0;
}

bool KeyOf(int seat, Seat& s) noexcept
{
    if (s.key != 0 || s.keyFailed)
    {
        return s.key != 0;
    }
    const std::wstring text = std::format(L"LSS_LeadMark_{}", seat);
    if (!platform::CallGuarded(&MakeName, text.c_str(), &s.key) || s.key == 0)
    {
        s.key       = 0;
        s.keyFailed = true;
        platform::Log(L"lead mark: the name {} could not be made; {}'s kept walks get no marker", text, SeatName(seat));
        return false;
    }
    return true;
}

bool Add(const Seat& s, const float at[3]) noexcept
{
    const EmptyText  text;
    const IconParams params;
    return platform::CallGuarded(g_add, s.key, facts::kMapIconKindMainStory, at, &text, &params);
}

void Take(int seat, Seat& s, const wchar_t* why) noexcept
{
    const bool removed = platform::CallGuarded(g_remove, s.key);
    platform::Log(L"lead mark: {}'s marker removed ({}){}", SeatName(seat), why, removed ? L"" : L" (the game's remove raised)");
    s.placed = false;
}

/// The marker of `s` withheld from a view: its diamond in `column`'s icon manager (and player 1's, never its owner's),
/// its pin on the minimap as the holder last ran.
void Withhold(const Seat& s, int column) noexcept
{
    column_area_icons::HideIcon(column, s.key);
    if (column != 0)
    {
        column_area_icons::HideIcon(0, s.key);
    }
    minimap::HidePin(s.key);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto add    = platform::FindFunction(image, facts::kAddMapIcon);
    const auto remove = platform::FindFunction(image, facts::kRemoveMapIcon);
    if (!add || !remove || !(widget_visibility::Installed() || widget_visibility::Install(image)))
    {
        platform::Log(L"lead mark: an engine check failed; a walk the level needs goes to the character's AI under either "
                      L"lead-holder setting");
        return false;
    }
    g_add       = reinterpret_cast<AddIconFn>(*add);
    g_remove    = reinterpret_cast<RemoveIconFn>(*remove);
    g_installed = true;
    session::lead_gate::SetMarking(true);
    platform::Log(L"lead mark: a walk player 2 or 3 keeps (lead-holder keeps) is marked in that player's view (the game's "
                  L"main-objective diamond and minimap pin)");
    return true;
}

void Remove() noexcept
{
    if (!g_installed)
    {
        return;
    }
    session::lead_gate::SetMarking(false);
    const bool gameThread = players::generation::OnGameThread();
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[static_cast<std::size_t>(seat)];
        if (s.placed && gameThread)
        {
            Take(seat, s, L"the mod unloads");
        }
        s.placed = false;  // off the game thread: left to the next field end, which empties the store
    }
    g_installed = false;
}

void Tick() noexcept
try
{
    if (!g_installed)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        Seat& s = g_seats[static_cast<std::size_t>(seat)];
        if (++s.ticks % kCheckEvery != 0)
        {
            continue;
        }
        void* member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        float at[3]  = {};
        if (member == nullptr || !session::lead_gate::KeptMark(member, at))
        {
            if (s.placed)
            {
                Take(seat, s, member == nullptr ? L"the player let go of the character"
                                                : L"the walk is no longer the player's to keep");
            }
            continue;
        }
        if (!KeyOf(seat, s))
        {
            continue;
        }
        const bool moved = s.placed && (std::fabs(s.at[0] - at[0]) > kMovedCm || std::fabs(s.at[1] - at[1]) > kMovedCm ||
                                        std::fabs(s.at[2] - at[2]) > kMovedCm);
        const bool added = Add(s, at);  // every check: the game updates a key it holds in place, and adds it after a reset
        if (added && (!s.placed || moved))
        {
            platform::Log(L"lead mark: {}'s marker {} at ({:.1f}, {:.1f}, {:.1f}) m, shown in {}'s view only", SeatName(seat),
                          s.placed ? L"moved (the level's next stop)" : L"placed", at[0] / 100, at[1] / 100, at[2] / 100,
                          SeatName(seat));
        }
        else if (!added && !s.placed)
        {
            platform::Log(L"lead mark: {}'s marker could not be added (the game's AddIcon raised)", SeatName(seat));
        }
        s.placed = added || s.placed;
        s.at[0]  = at[0];
        s.at[1]  = at[1];
        s.at[2]  = at[2];
    }
}
catch (...)
{
    platform::LogWarning(L"lead mark: the tick raised; the markers stay as they were");
}

void BeforeColumnPaint(int column, int shown) noexcept
{
    if (!g_installed)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[static_cast<std::size_t>(seat)];
        if (s.placed && seat != shown)
        {
            Withhold(s, column);
        }
    }
}

void BeforePlayerOnePaint() noexcept
{
    if (!g_installed)
    {
        return;
    }
    for (int seat = 1; seat <= kSeatsBuilt; ++seat)
    {
        const Seat& s = g_seats[static_cast<std::size_t>(seat)];
        if (s.placed)
        {
            Withhold(s, 0);
        }
    }
}

}  // namespace lss::hud::lead_mark
