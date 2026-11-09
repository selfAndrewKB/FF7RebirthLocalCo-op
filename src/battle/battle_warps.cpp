#include "battle/battle_warps.h"

#include "battle/battle_warps_rules.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/dev_switch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/unreal_access.h"

#include "platform/command_registry.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

namespace lss::battle::battle_warps {

namespace {

using battle_warps_rules::Mode;

// The words of the setting's row in the table, in the enum's order.
constexpr auto& kModeWords = platform::settings::kBattleWarpsWords;
static_assert(kModeWords.size() == 3 && std::wstring_view(kModeWords[static_cast<std::size_t>(Mode::PlayersStay)]) == L"players-stay" &&
                  std::wstring_view(kModeWords[static_cast<std::size_t>(Mode::Off)]) == L"off" &&
                  std::wstring_view(kModeWords[static_cast<std::size_t>(Mode::Game)]) == L"game",
              "the table's words name the modes in the enum's order");

// Game thread (the console and the page set it there; the redirects read it there).
Mode g_mode = Mode::PlayersStay;

// Facts read from the database (RE-p2-battle-gather-warp.md §S-BTLWARP2; this build).
// The exclusion set's test, bool(InlineActorArray16* set, AActor** element): a pure linear search with 16 callers, so
// only its loop-1 call in PartyBattleAIMgr_QueueBattleInTeleports is redirected (rdx = &the battle member).
constexpr std::uint8_t kContainsBytes[] = {0x48, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC1, 0x48, 0x85,
                                           0xC0, 0x4C, 0x0F, 0x45, 0xC0, 0x48, 0x63, 0x81, 0x88, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kContains{L"InlineActorArray16_Contains", 0x140A6C818, kContainsBytes};
constexpr std::uintptr_t         kBattleInCall = 0x141251364;
// The battle end's per-member placement, void(ctx{&mgr, mgr}*, character, FVector* location, bool extra); its call
// in PartyBattleAIMgr_QueueBattleFinishTeleports (non-leader members, rdx = the member).
constexpr std::uint8_t kPlaceBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x4C, 0x89,
                                        0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x41, 0x54, 0x41};
constexpr platform::FunctionFact kPlace{L"PartyBattleAIMgr_BattleEnd_PlaceMember", 0x1421DCD50, kPlaceBytes};
constexpr std::uintptr_t         kBattleFinishCall = 0x141324E04;

constexpr std::uint32_t kControllerPawn = 0x258;  // AController::Pawn

using ContainsFn = bool (*)(void* set, void* const* element);
using PlaceFn    = void (*)(void* context, void* character, void* location, bool extra);

// Set once at install.
platform::CallRedirect g_battleInCall;
platform::CallRedirect g_battleFinishCall;
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t contains = 0, place = 0, inSite = 0, endSite = 0;  // 0: not found (logged by the finder)
};
Sites                   g_sites;
platform::DeferredSites g_placement;  // 1.005: the sites placed from the game thread (platform/deferred_sites.h)

/// The seat whose member `character` is, while that seat plays; else 0.
int SeatPlaying(const void* character) noexcept
{
    const int seat = character != nullptr ? players::generation::SeatOfMember(character) : 0;
    return players::SeatBuilt(seat) && players::generation::Active(seat) ? seat : 0;
}

/// This battle's gather, as its start's exclusion test saw it (game thread): reached at all (the battle's data allowed
/// one), and each member asked about with the data's own answer.
struct Gather
{
    bool                                       reached = false;
    std::array<std::pair<const void*, bool>, 8> members{};  // {character, kept out by the data}
    std::size_t                                count = 0;
};
Gather g_gather;

void NoteGathered(const void* character, bool byData) noexcept
{
    g_gather.reached = true;
    for (std::size_t i = 0; i < g_gather.count; ++i)
    {
        if (g_gather.members[i].first == character)
        {
            g_gather.members[i].second = byData;
            return;
        }
    }
    if (g_gather.count < g_gather.members.size())
    {
        g_gather.members[g_gather.count++] = {character, byData};
    }
}

/// Whether the setting keeps `character` where it is (battle/battle_warps_rules.h); its seat in `seat` (0: none). Only
/// on the game thread: any other thread passes through.
bool KeepsInPlace(const void* character, int& seat) noexcept
{
    seat = 0;
    if (character == nullptr || g_mode == Mode::Game || !players::generation::OnGameThread())
    {
        return false;
    }
    seat = SeatPlaying(character);
    return battle_warps_rules::Stays(g_mode, seat);
}

/// Whose character it is, for the log lines.
std::wstring Whose(int seat)
{
    return seat != 0 ? std::wstring(players::SeatName(seat)) + L"'s member" : std::wstring(L"a member no player controls");
}

/// A seat's member's distance from player 1's character in metres, or -1 if it cannot be read.
float DistanceFromPlayerOne(void* member) noexcept
{
    void* pawn = nullptr;
    if (const auto controller = reinterpret_cast<std::uintptr_t>(players::generation::PlayerOneController()); controller != 0)
    {
        platform::CopyGuarded(&pawn, reinterpret_cast<const void*>(controller + kControllerPawn), sizeof(pawn));
    }
    float own[3]{}, one[3]{};
    if (pawn == nullptr || !players::unreal::ActorLocation(member, own) || !players::unreal::ActorLocation(pawn, one))
    {
        return -1.0f;
    }
    const float dx = own[0] - one[0], dy = own[1] - one[1], dz = own[2] - one[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;
}

/// One kept member's line (Info). Allocates only for the line.
void LogKept(const wchar_t* when, int seat, void* character) noexcept
{
    try
    {
        platform::Log(L"battle warps: battle {} leaves {} where it is ({:.1f} m from player 1's character; -1: unknown)",
                      when, Whose(seat), DistanceFromPlayerOne(character));
    }
    catch (...)
    {
    }
}

/// One member left to the game's own decision (Debug, game thread only). Allocates only for the line.
void LogLeft(const wchar_t* when, int seat) noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || !players::generation::OnGameThread())
    {
        return;
    }
    try
    {
        platform::LogDebug(L"battle warps: battle {} leaves {} to the game's own warp", when, Whose(seat));
    }
    catch (...)
    {
    }
}

/// Replaces the battle-in queue's exclusion test: a member the setting keeps reads as excluded, so the game skips it
/// as it skips a vehicle rider (no warp entry, no fade, no AI pause; the next member takes its slot).
bool BattleInExcluded(void* set, void* const* element) noexcept
{
    // The game's own answer first (a pure set test): whether this battle's data keeps the member out of its gather.
    const bool byData = reinterpret_cast<ContainsFn>(g_battleInCall.Original())(set, element);
    if (players::generation::OnGameThread() && element != nullptr)
    {
        NoteGathered(*element, byData);
    }
    int seat = 0;
    if (element != nullptr && KeepsInPlace(*element, seat))
    {
        LogKept(L"start", seat, *element);
        return true;
    }
    LogLeft(L"start", seat);
    return byData;
}

/// Replaces the battle-finish queue's placement call: none for a member the setting keeps (as the placement's own
/// early-outs: no entry, no AI pause).
void BattleFinishPlace(void* context, void* character, void* location, bool extra) noexcept
{
    int seat = 0;
    if (KeepsInPlace(character, seat))
    {
        LogKept(L"end", seat, character);
        return;
    }
    LogLeft(L"end", seat);
    reinterpret_cast<PlaceFn>(g_battleFinishCall.Original())(context, character, location, extra);
}

const wchar_t* ModeWord(Mode mode) noexcept
{
    return kModeWords[static_cast<std::size_t>(mode)];
}

/// The mode named by `word`, or empty.
std::optional<Mode> ModeOf(std::wstring_view word) noexcept
{
    for (std::size_t i = 0; i < kModeWords.size(); ++i)
    {
        if (word == kModeWords[i])
        {
            return static_cast<Mode>(i);
        }
    }
    return std::nullopt;
}

/// The setting's one setter (the file at install, the console, the Co-op page). Allocates only for the line.
void SetMode(Mode mode, const wchar_t* why) noexcept
{
    g_mode = mode;
    try
    {
        switch (mode)
        {
            case Mode::Game:
                platform::Log(L"battle warps: the game warps the whole party at a battle's start and end ({})", why);
                break;
            case Mode::PlayersStay:
                platform::Log(L"battle warps: every player's own character stays where it is at a battle's start and "
                              L"end; the game warps the members no player controls ({})",
                              why);
                break;
            case Mode::Off:
                platform::Log(L"battle warps: no party member is warped at a battle's start or end ({})", why);
                break;
        }
    }
    catch (...)
    {
    }
}

/// The redirects, from what Install found.
bool PlaceSites(const platform::GameImage&) noexcept
{
    const Sites& s     = g_sites;
    const bool   start = s.contains != 0 && s.inSite != 0 &&
                         g_placement.Place(g_battleInCall, s.inSite, s.contains,
                                           reinterpret_cast<std::uintptr_t>(&BattleInExcluded));
    const bool end = s.place != 0 && s.endSite != 0 &&
                     g_placement.Place(g_battleFinishCall, s.endSite, s.place,
                                       reinterpret_cast<std::uintptr_t>(&BattleFinishPlace));
    platform::Log(L"battle warps: battle start {}; battle end {}",
                  start ? L"follows the setting" : L"UNAVAILABLE (a check failed): the game's warps",
                  end ? L"follows the setting" : L"UNAVAILABLE (a check failed): the game's warps");
    return start || end;
}

}  // namespace

bool GatheredFor(const void* character) noexcept
{
    if (!g_gather.reached || character == nullptr)
    {
        return false;
    }
    for (std::size_t i = 0; i < g_gather.count; ++i)
    {
        if (g_gather.members[i].first == character)
        {
            return !g_gather.members[i].second;
        }
    }
    return false;
}

void ForgetGather() noexcept
{
    g_gather = Gather{};
}

bool Install(const platform::GameImage& image) noexcept
{
    // The file's word is one of the table's (platform/settings.h); set before the redirects are placed.
    const Mode fromFile =
        ModeOf(platform::settings::Of(platform::settings::kBattleWarps).word).value_or(Mode::PlayersStay);
    try
    {
        SetMode(fromFile, platform::settings::Why(platform::settings::kBattleWarps).c_str());
    }
    catch (...)
    {
        SetMode(fromFile, L"the settings file");  // out of memory for the reason's text
    }
    try
    {
        control::RegisterOption(
            platform::settings::kBattleWarps,
            [](const std::wstring& word) {
                const auto mode = ModeOf(word);
                if (mode)
                {
                    SetMode(*mode, L"the console");
                }
                return control::Outcome{mode.has_value(), mode ? ModeWord(*mode) : L"not a battle-warps word"};
            },
            [] { return std::wstring(ModeWord(g_mode)); });
    }
    catch (...)
    {
        platform::Log(L"battle warps: the option could not be registered (out of memory?); the page cannot set it");
    }
    const auto contains = platform::FindFunction(image, kContains);
    const auto place    = platform::FindFunction(image, kPlace);
    const auto inSite   = image.Rebase(kBattleInCall, 5);
    const auto endSite  = image.Rebase(kBattleFinishCall, 5);
    g_sites             = {contains.value_or(0), place.value_or(0), inSite.value_or(0), endSite.value_or(0)};
    // The battle-in call crosses a 16-byte block in 1.005: the sites are placed from the game thread, between its
    // ticks (platform/deferred_sites.h; battle setup runs in the game mode's tick). The placement reads only the
    // addresses found here.
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"battle warps: {}",
                      placed ? L"its calls are placed from the game thread (1.005: one crosses a 16-byte block)"
                             : L"UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    for (auto* call : {&g_battleFinishCall, &g_battleInCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"battle warps: a battle warp call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::battle::battle_warps
