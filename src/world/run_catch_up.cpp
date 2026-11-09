#include "world/run_catch_up.h"

#include "facts/party.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "session/lead_gate.h"
#include "world/catch_up_move.h"
#include "world/party_warp.h"
#include "world/story_box_rules.h"
#include "world/story_boxes.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace lss::world::run_catch_up {

namespace {

namespace rules = story_box_rules;

using LeaderFn = void* (*)();

constexpr float kPi = 3.14159265358979f;

// Set at install, then only read.
bool     g_installed = false;
LeaderFn g_leader    = nullptr;

std::atomic<bool> g_on{true};  // the console's switch
// Game thread.
std::uint64_t g_movedMember = 0, g_movedBox = 0;  // the last walk a character was brought back for (one move per walk)
std::uint32_t g_moved = 0, g_canRun = 0, g_onePast = 0, g_noPoint = 0, g_busy = 0, g_notHeld = 0;
std::wstring  g_last = L"none yet";

std::wstring Reading()
{
    return std::format(L"{}; brought back {}; left (its walk passes through its box) {}, player 1 past the box too {}, no navmesh "
                       L"point {}, in a field action {}, no held enter for the box {}; last: {}",
                       !g_installed ? L"not installed" : g_on.load() ? L"on" : L"off", g_moved, g_canRun, g_onePast, g_noPoint,
                       g_busy, g_notHeld, g_last);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto leader = platform::FindFunction(image, facts::kResolvePartyLeader);
    if (!leader || !catch_up_move::Installed())
    {
        platform::Log(L"run catch-up: an engine check failed; a character taken past its own story box stays where it is when "
                      L"its walk goes to its AI");
        return false;
    }
    g_leader    = reinterpret_cast<LeaderFn>(*leader);
    g_installed = true;
#if LSS_DEV
    control::Register({.name  = L"run-catch-up",
                       .shape = control::Shape::OnOff,
                       .help  = L"a character player 2 or 3 took past its own story box is brought beside player 1's character "
                                L"when the level's walk through that box goes to its AI (on at start); off: it walks from where "
                                L"it stands",
                       .set   = [](const control::Value& value) {
                           g_on.store(std::get<bool>(value));
                           platform::Log(L"run catch-up: {}", g_on.load() ? L"on" : L"off");
                           return control::Outcome{};
                       },
                       .get = [] { return control::Value{g_on.load()}; }});
    control::Register({.name  = L"world.run-catch-up",
                       .shape = control::Shape::None,
                       .help  = L"(read only) characters brought back for their walk into their own story box, and why not",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
#endif
    platform::Log(L"run catch-up: a character player 2 or 3 took past its own story box is brought beside player 1's character "
                  L"when the level's walk through it goes to its AI");
    return true;
}

bool BeforeGuide(int seat, void* member) noexcept
try
{
    if (!g_installed || !g_on.load() || member == nullptr)
    {
        return false;
    }
    float         to[3] = {};
    std::uint64_t box   = 0;
    if (!session::lead_gate::OwnBoxRun(member, to, box))
    {
        if (players::weak_object::StillNames(g_movedMember, member))
        {
            g_movedMember = g_movedBox = 0;  // the walk it was brought back for ended without the hand
        }
        return false;  // a guide for another reason: as before
    }
    const std::wstring who   = std::format(L"{}'s {}", players::SeatName(seat), players::NameOf(member));
    const std::wstring where = std::format(L"its walk through {}", players::unreal::NameText(box));
    rules::Box         shape{};
    if (!story_boxes::HeldShape(member, box, shape))
    {
        ++g_notHeld;
        platform::LogDebug(L"run catch-up: {}: {}: it never entered the box (no held enter); not moved", who, where);
        return false;
    }
    float at[3] = {};
    if (!players::unreal::ActorLocation(member, at))
    {
        return false;
    }
    // The hand follows this answer: the held enters the walk passes through from here wait for its own body. Marked
    // only now, with its AI about to take the character, never while its player still walks it.
    if (g_movedBox == box && players::weak_object::StillNames(g_movedMember, member))
    {
        g_movedMember = g_movedBox = 0;  // brought back once for this walk: the hand goes on
        if (story_boxes::AwaitRun(member, at, to) == 0)
        {
            platform::Log(L"run catch-up: {}: {}: from where it stands after the move its walk misses the box; the held enter "
                          L"fires at the hand",
                          who, where);
        }
        return false;
    }
    if (rules::RunCrosses(shape, at, to))
    {
        ++g_canRun;
        platform::LogDebug(L"run catch-up: {}: {} passes through the box from where it stands; not moved", who, where);
        story_boxes::AwaitRun(member, at, to);
        return false;
    }
    void* one      = nullptr;
    float oneAt[3] = {};
    if (!platform::CallGuarded(g_leader, one) || one == nullptr || one == member || !players::unreal::ActorLocation(one, oneAt))
    {
        return false;
    }
    if (!rules::RunCrosses(shape, oneAt, to))
    {
        ++g_onePast;
        g_last = std::format(L"{}: {}: player 1's character is past the box too; not moved", who, where);
        platform::Log(L"run catch-up: {}", g_last);
        return false;
    }
    if (party_warp::InFieldAction(member))
    {
        ++g_busy;
        platform::LogDebug(L"run catch-up: {}: in a field action; not moved", who);
        return false;
    }
    // Toward the walk's point from player 1's character, then 35 degrees to either side, then behind it: the first
    // navmesh point the walk passes the box from and no other player's character stands on.
    const float base = std::atan2(to[1] - oneAt[1], to[0] - oneAt[0]);
    if (!std::isfinite(base))
    {
        return false;
    }
    constexpr std::array<float, 4>          kTurns{0.0f, 35.0f, -35.0f, 180.0f};
    constexpr std::array<const wchar_t*, 4> kSides{L"toward the walk's point", L"to one side of the walk", L"to the other side",
                                                   L"behind it"};
    float          point[3] = {};
    const wchar_t* side     = nullptr;
    for (std::size_t i = 0; i < kTurns.size() && side == nullptr; ++i)
    {
        float candidate[3] = {};
        if (catch_up_move::PointAround(member, oneAt, base + kTurns[i] * kPi / 180.0f, catch_up_move::kFollowerCm, candidate) &&
            rules::RunCrosses(shape, candidate, to) && !catch_up_move::SpotTaken(candidate, member))
        {
            point[0] = candidate[0];
            point[1] = candidate[1];
            point[2] = candidate[2];
            side     = kSides[i];
        }
    }
    const float away = std::hypot(at[0] - oneAt[0], at[1] - oneAt[1]) / 100.0f;
    if (side == nullptr)
    {
        ++g_noPoint;
        g_last = std::format(L"{}: {}: {:.1f} m from player 1's character, no navmesh point beside it that the walk passes the box "
                             L"from; not moved",
                             who, where, away);
        platform::Log(L"run catch-up: {}", g_last);
        return false;
    }
    const std::uint64_t weak = players::weak_object::WeakOf(member);
    if (weak == 0)
    {
        return false;  // a move it could not be told apart from the next: the hand goes on as before
    }
    if (!catch_up_move::Move(member, point, std::atan2(to[1] - point[1], to[0] - point[0]) * 180.0f / kPi))
    {
        platform::LogWarning(L"run catch-up: the move of {} raised; it may stand where it was", who);
        return false;
    }
    g_movedMember = weak;
    g_movedBox    = box;
    ++g_moved;
    g_last = std::format(L"{}: {} could not pass through the box from {:.1f} m away from player 1's character: brought 1.5 m from "
                         L"player 1's character, {}; its AI takes it at the next check, its held-back story enters waiting for "
                         L"its own walk",
                         who, where, away, side);
    platform::Log(L"run catch-up: {}", g_last);
    return true;
}
catch (...)
{
    platform::Log(L"run catch-up: an exception; not moved");
    return false;
}

}  // namespace lss::world::run_catch_up
