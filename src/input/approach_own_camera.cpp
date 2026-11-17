#include "input/approach_own_camera.h"

#include "facts/controller.h"
#include "input/pad_sticks.h"
#include "platform/angles.h"
#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/actor_fields.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace lss::input::approach_own_camera {

namespace seat_ladder = scopes::seat_ladder;

namespace {

// 1.004 addresses; 1.005 through replaced-approach-two.json (every site and callee read from both exes).
constexpr std::uintptr_t kDirCheck  = 0x1415DD02C;  // FieldAction_ApproachDirCheck_MergedStick
constexpr std::uintptr_t kBeamCheck = 0x14344983C;  // SplineMove_ApproachCheck_MergedStickPrimaryBasis

struct Site
{
    const wchar_t* name;
    std::uintptr_t call;
    bool           late = false;  // the call crosses a 16-byte line in a build: installed from the game thread (Tick)
};
// The notifies' calls of the shared check (research/RE-p2-ladder-approach-L2.md). The last four cross an aligned 16-byte
// line in at least one build; every notify runs only on the game thread (research/RE-p2-spider-oneshot-notify-threads.md),
// so those
// are swapped from the game thread outside any character's tick, the way the HUD's crossing calls are.
constexpr Site kDirSites[] = {
    {L"ladder", 0x1415DCC77},         {L"crawl", 0x1415DC89A},         {L"squeeze", 0x1415DDAC7},
    {L"squeeze", 0x1426F0862},        {L"squeeze", 0x1426F08A3},       {L"climbing wall", 0x1415DF469},
    {L"pole slide", 0x143377B71},     {L"hook rope", 0x1433B295A},     {L"lifting rope", 0x1433B2E77},
    {L"one-shot move", 0x1426F09D5, true}, {L"monkey bars", 0x1433779BC, true}, {L"zip line", 0x14337814D, true},
    {L"spider wall", 0x14340A0FB, true},
};
constexpr std::size_t kDirCount = std::size(kDirSites);
constexpr Site        kBeamSite{L"balance beam", 0x1426F0BF7};

// bool Check(character, endTransform, outClosestPoint, forward, back, radius, slack, lift, angleDegrees): pure but for the
// write to the closest point.
using DirCheckFn = bool (*)(void* character, const void* endTransform, void* outPoint, float forward, float back,
                            float radius, float slack, float lift, float angle);
// bool Check(character, ...): pointer-sized arguments only (research/RE-p2-ladder-approach-L2.md).
using BeamCheckFn = bool (*)(void* character, void* a2, void* a3, void* a4, void* a5, void* a6);

platform::CallRedirect g_dirCalls[kDirCount];
platform::CallRedirect g_beamCall;

// The late sites' live addresses, found at Install; swapped once by Tick on the game thread.
struct Pending
{
    std::uintptr_t site   = 0;
    std::uintptr_t target = 0;
};
Pending g_pending[kDirCount];
bool    g_lateTried = false;

constexpr int kOther = -1;  // a character of no player's

/// Whose character the check is for: a built seat's member (while its player is in), 0 player 1's pawn (while a seat's
/// player is in), or kOther.
int WhoIs(const void* character) noexcept
{
    if (character == nullptr || !players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        return kOther;
    }
    const int seat = players::generation::SeatOfMember(character);
    if (players::SeatBuilt(seat) && players::generation::Active(seat))
    {
        return seat;
    }
    void* one = players::generation::ControllerOf(0);
    return one != nullptr && character == players::actor_fields::Pawn(one) ? 0 : kOther;
}

std::int32_t PadOf(int who) noexcept
{
    return pad_sticks::ControllerIdOf(players::generation::ControllerOf(who));
}

/// The yaw (degrees) of the end transform's forward axis, the direction the game compares the stick with.
float ForwardYaw(const void* transform) noexcept
{
    return QuatFieldYaw(transform, 0);  // the transform's rotation leads it
}

// Game thread: the last answer logged per player, by seat (0 player 1's; Debug on change).
struct Seen
{
    const wchar_t* site    = nullptr;
    bool           passed  = false;
    int            before  = -1;     // what the check answered in player 1's camera basis (a seat, Debug; -1 not run)
    bool           scoped  = false;
    bool           any     = false;
};
Seen g_seen[players::kMaxPlayers];

void Note(int who, const wchar_t* site, bool passed, int before, bool scoped, float limit, float yaw) noexcept
{
    Seen& seen = g_seen[who];
    platform::LogTrace(L"approach: player {}'s {} check: {} (limit {:.0f} deg, the end faces {:.0f} deg)", who + 1,
                       site, passed ? L"pass" : L"fail", limit, yaw);
    if (seen.any && seen.site == site && seen.passed == passed && seen.before == before && seen.scoped == scoped)
    {
        return;
    }
    seen = {site, passed, before, scoped, true};
    if (who != 0)
    {
        platform::LogDebug(L"approach: {}'s {} check {} in {}'s camera basis{}{}; limit {:.0f} deg, the "
                           L"end faces {:.0f} deg",
                           players::SeatName(who), site, passed ? L"passes" : L"fails", players::SeatName(who),
                           before < 0 ? L"" : before != 0 ? L" (player 1's would pass)" : L" (player 1's would fail)",
                           scoped ? L"" : L" — a scope was REFUSED, the game's basis was used", limit, yaw);
        return;
    }
    platform::LogDebug(L"approach: player 1's {} check {} with pad 1's stick{}", site, passed ? L"passes" : L"fails",
                       scoped ? L"" : L" (the stick scope was refused: the merged stick)");
}

/// The check's scopes: the player's pad's sticks and, for a seat, the primary controller (the camera manager getter
/// answers the seat's, the lock is theirs). The check runs with whatever opened; Scoped says whether all of it did.
unsigned ScopesOf(int who) noexcept
{
    return who != 0 ? seat_ladder::Sticks | seat_ladder::Primary : seat_ladder::Sticks;
}

seat_ladder::Args ArgsOf(int who) noexcept
{
    seat_ladder::Args args;
    if (who != 0)
    {
        args.seat = who;  // player 1's check opens Sticks alone, which reads no seat
    }
    args.padId = PadOf(who);
    return args;
}

bool Scoped(int who, const seat_ladder::SeatScopes& scopes) noexcept
{
    return scopes.Holds<seat_ladder::SticksPart>() && (who == 0 || scopes.Holds<seat_ladder::PrimaryPart>());
}

template <std::size_t Index>
bool DirCheck(void* character, const void* endTransform, void* outPoint, float forward, float back, float radius,
              float slack, float lift, float angle) noexcept
{
    const auto original = reinterpret_cast<DirCheckFn>(g_dirCalls[Index].Original());
    const int  who      = WhoIs(character);
    if (who == kOther)
    {
        return original(character, endTransform, outPoint, forward, back, radius, slack, lift, angle);
    }
    const bool logs   = platform::Enabled(platform::Level::Debug);
    // What the game would answer without the scopes (only written to the closest point, which the scoped call below
    // writes again).
    const int before = logs && who != 0
                           ? original(character, endTransform, outPoint, forward, back, radius, slack, lift, angle)
                           : -1;
    bool passed = false;
    bool scoped = false;
    {
        const seat_ladder::SeatScopes scopes(ScopesOf(who), ScopesOf(who), ArgsOf(who));
        scoped = Scoped(who, scopes);
        passed = original(character, endTransform, outPoint, forward, back, radius, slack, lift, angle);
    }
    if (logs)
    {
        Note(who, kDirSites[Index].name, passed, before, scoped, angle, ForwardYaw(endTransform));
    }
    return passed;
}

bool BeamCheck(void* character, void* a2, void* a3, void* a4, void* a5, void* a6) noexcept
{
    const auto original = reinterpret_cast<BeamCheckFn>(g_beamCall.Original());
    const int  who      = WhoIs(character);
    if (who == kOther)
    {
        return original(character, a2, a3, a4, a5, a6);
    }
    bool passed = false;
    bool scoped = false;
    {
        const seat_ladder::SeatScopes scopes(ScopesOf(who), ScopesOf(who), ArgsOf(who));
        scoped = Scoped(who, scopes);
        passed = original(character, a2, a3, a4, a5, a6);
    }
    if (platform::Enabled(platform::Level::Debug))
    {
        Note(who, kBeamSite.name, passed, -1, scoped, 60.0f, 0.0f);  // its own 60-degree test; not run twice
    }
    return passed;
}

template <std::size_t... Index>
constexpr auto MakeDirChecks(std::index_sequence<Index...>) noexcept
{
    return std::array<DirCheckFn, sizeof...(Index)>{&DirCheck<Index>...};
}
constexpr auto kDirChecks = MakeDirChecks(std::make_index_sequence<kDirCount>{});

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    std::wstring missing;
    std::size_t  placed = 0, late = 0;
    for (std::size_t i = 0; i < kDirCount; ++i)
    {
        if (kDirSites[i].late)
        {
            const auto at = image.Rebase(kDirSites[i].call, 5);
            const auto to = image.Rebase(kDirCheck);
            if (at && to)
            {
                g_pending[i] = {*at, *to};
                ++late;
                continue;
            }
        }
        else if (g_dirCalls[i].InstallSource(image, kDirSites[i].call, kDirCheck,
                                               reinterpret_cast<std::uintptr_t>(kDirChecks[i])))
        {
            ++placed;
            continue;
        }
        try
        {
            missing += std::format(L" {} (0x{:X})", kDirSites[i].name, kDirSites[i].call);
        }
        catch (...)
        {
        }
    }
    g_lateTried = false;
    const bool beam = g_beamCall.InstallSource(image, kBeamSite.call, kBeamCheck,
                                                 reinterpret_cast<std::uintptr_t>(&BeamCheck));
    platform::Log(L"approach: {} walk-up checks (ladder, crawl, squeeze, climbing wall, pole slide, hook rope, lifting "
                  L"rope) read player 2's or 3's stick in that player's camera basis, {} more (one-shot move, monkey bars, zip "
                  L"line, spider wall) from the game thread's first tick{}{}; balance beam {}",
                  placed, late, missing.empty() ? L"" : L"; NOT placed:", missing, beam ? L"yes" : L"NO");
}

void Tick() noexcept
{
    if (g_lateTried || !players::generation::OnGameThread())
    {
        return;
    }
    g_lateTried = true;  // here the game thread is outside every character's tick, as InstallFromOwnThread requires
    std::size_t placed = 0, wanted = 0;
    for (std::size_t i = 0; i < kDirCount; ++i)
    {
        if (g_pending[i].site == 0)
        {
            continue;
        }
        ++wanted;
        if (g_dirCalls[i].InstallFromOwnThread(g_pending[i].site, g_pending[i].target,
                                               reinterpret_cast<std::uintptr_t>(kDirChecks[i])))
        {
            ++placed;
        }
        else
        {
            platform::Log(L"approach: the {} check's call could not be redirected; it keeps the game's basis",
                          kDirSites[i].name);
        }
    }
    if (wanted != 0)
    {
        platform::Log(L"approach: {} of {} game-thread walk-up checks placed (one-shot move, monkey bars, zip line, "
                      L"spider wall)", placed, wanted);
    }
}

void Remove() noexcept
{
    const bool gameThread = players::generation::OnGameThread();
    for (std::size_t i = 0; i < kDirCount; ++i)
    {
        platform::CallRedirect& redirect = g_dirCalls[i];
        if (!redirect.IsInstalled() || (kDirSites[i].late && !gameThread))
        {
            continue;  // a crossing call is restored only from the game thread; left in place it passes through
        }
        if (!redirect.Restore())
        {
            platform::Log(L"approach: a call was changed after us; left in place (it passes through)");
        }
    }
    for (Pending& pending : g_pending)
    {
        pending = {};
    }
    if (g_beamCall.IsInstalled() && !g_beamCall.Restore())
    {
        platform::Log(L"approach: the beam's call was changed after us; left in place (it passes through)");
    }
    for (Seen& seen : g_seen)
    {
        seen = {};
    }
}

}  // namespace lss::input::approach_own_camera
