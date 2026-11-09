#include "battle/arena_warp.h"

#include "battle/battle_warps.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "rides/own_ride.h"
#include "world/catch_up_move.h"
#include "world/party_warp.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::battle::arena_warp {

namespace {

using CharacterTestFn = bool (*)(const void* character);
using LeaderFn        = void* (*)();

constexpr float kPi = 3.14159265358979f;
// The gather's own placement in a fight: around the field leader at 300 then 100 uu, 70 then 50 degrees to a side.
constexpr std::array<float, 4> kTurnsDegrees{70.0f, -70.0f, 50.0f, -50.0f};
constexpr std::array<float, 2> kDistancesCm{300.0f, 100.0f};
constexpr std::uint32_t        kCheckAfterTicks = 30;  // the inside test again, about half a second after a warp

// Set at install, then only read.
bool                  g_installed = false;
CharacterTestFn       g_alive     = nullptr;  // the battle switch's member test: live, not dead, not petrified
CharacterTestFn       g_inside    = nullptr;  // inside any battle area
LeaderFn              g_leader    = nullptr;
const std::uintptr_t* g_root      = nullptr;

// Game thread.
std::array<const wchar_t*, players::kMaxPlayers> g_asked{};  // by seat: the pending ask's words (null: none)
std::array<std::uint32_t, players::kMaxPlayers>  g_checkIn{};  // by seat: ticks left to the after-warp inside test
std::array<std::uint64_t, players::kMaxPlayers>  g_warped{};   // by seat: the warped member (weak)
bool                                             g_wasInBattle = false;

const void* Root() noexcept
{
    std::uintptr_t root = 0;
    return g_root != nullptr && platform::CopyGuarded(&root, g_root, sizeof(root)) ? reinterpret_cast<const void*>(root) : nullptr;
}

std::uint8_t EncounterState() noexcept
{
    return ReadField<std::uint8_t>(ReadPointer(Root(), facts::kRootModeHolder), facts::kEncounterState);
}

bool EventHoldsParty() noexcept
{
    const void* state = ReadPointer(Root(), facts::kRootGameState);
    return state != nullptr && (ReadField<std::int32_t>(state, facts::kGameStateEventCounterA) != 0 ||
                                ReadField<std::int32_t>(state, facts::kGameStateEventCounterB) != 0);
}

bool Test(CharacterTestFn fn, const void* character, bool& out) noexcept
{
    return platform::CallGuarded(fn, out, character);
}

/// Why `seat`'s member cannot be brought in now (null: it can).
const wchar_t* Refusal(int seat, void* member, const void* one) noexcept
{
    const std::uint8_t state = EncounterState();
    bool               flag  = false;
    if (member == nullptr)
    {
        return L"no character it plays now (riding along on its AI, or none)";
    }
    if (state == facts::kEncounterEscaping)
    {
        return L"player 1 is escaping the fight";
    }
    if (state != facts::kEncounterInBattle)
    {
        return L"no fight";
    }
    if (member == one)
    {
        return L"it is player 1's character";
    }
    if (!battle_warps::GatheredFor(member))
    {
        return L"this fight's start gathered no one, or kept this character out (the game's own data)";
    }
    if (!Test(g_alive, member, flag) || !flag)
    {
        return L"it is knocked out or petrified";
    }
    if (EventHoldsParty())
    {
        return L"an event holds the party";
    }
    if (rides::own_ride::Riding(seat))
    {
        return L"its player rides";
    }
    if (world::party_warp::InFieldAction(member))
    {
        return L"it is in a field action";
    }
    if (!Test(g_inside, member, flag))
    {
        return L"the battle area could not be read";
    }
    if (flag)
    {
        return L"it is already in the fight's area";
    }
    return nullptr;
}

void Bring(int seat, const wchar_t* how) noexcept
try
{
    void*       member = players::generation::MemberOf(seat);
    void*       one    = nullptr;
    const bool  haveOne = platform::CallGuarded(g_leader, one) && one != nullptr;
    const std::wstring who = std::format(L"{}'s {}", players::SeatName(seat), players::NameOf(member));
    if (!haveOne)
    {
        platform::Log(L"arena warp: {} asked to be brought into the fight ({}); not done: player 1's character not read", who, how);
        return;
    }
    if (const wchar_t* why = Refusal(seat, member, one); why != nullptr)
    {
        platform::Log(L"arena warp: {} asked to be brought into the fight ({}); not done: {}", who, how, why);
        return;
    }
    float oneAt[3] = {}, oneYaw = 0.0f;
    if (!players::unreal::ActorLocation(one, oneAt) || !players::unreal::ActorYaw(one, oneYaw))
    {
        platform::Log(L"arena warp: {} asked to be brought into the fight ({}); not done: player 1's place not read", who, how);
        return;
    }
    float dest[3] = {oneAt[0], oneAt[1], oneAt[2]};  // the gather's own fallback: the leader's spot
    bool  placed  = false;
    for (std::size_t t = 0; t < kTurnsDegrees.size() && !placed; ++t)
    {
        for (std::size_t d = 0; d < kDistancesCm.size() && !placed; ++d)
        {
            float point[3] = {};
            if (world::catch_up_move::PointAround(member, oneAt, oneYaw + kTurnsDegrees[t] * kPi / 180.0f, kDistancesCm[d], point) &&
                !world::catch_up_move::SpotTaken(point, member))
            {
                dest[0] = point[0];
                dest[1] = point[1];
                dest[2] = point[2];
                placed  = true;
            }
        }
    }
    const wchar_t* found = placed ? L"beside player 1's character"
                                  : L"no navmesh point beside player 1's character: on its spot, as the game's gather does";
    if (!world::catch_up_move::Move(member, dest, oneYaw * 180.0f / kPi, oneAt, true))
    {
        platform::LogWarning(L"arena warp: the move of {} raised; it may stand where it was", who);
        return;
    }
    g_warped[static_cast<std::size_t>(seat)]  = players::weak_object::WeakOf(member);
    g_checkIn[static_cast<std::size_t>(seat)] = kCheckAfterTicks;
    platform::Log(L"arena warp: {} brought into the fight ({}): {}, at ({:.1f}, {:.1f}, {:.1f}) m, with the game's battle warp", who,
                  how, found, dest[0] / 100, dest[1] / 100, dest[2] / 100);
}
catch (...)
{
    platform::Log(L"arena warp: an exception; not moved");
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto alive  = platform::FindFunction(image, facts::kIsBattleActive);
    const auto inside = platform::FindFunction(image, facts::kInsideAnyBattleArea);
    const auto leader = platform::FindFunction(image, facts::kResolvePartyLeader);
    const auto root   = image.Rebase(facts::kEndGameRoot, sizeof(std::uintptr_t));
    if (!alive || !inside || !leader || !root || !world::catch_up_move::Installed())
    {
        platform::Log(L"arena warp: an engine check failed; L3 twice in a fight brings no character in");
        return false;
    }
    g_alive     = reinterpret_cast<CharacterTestFn>(*alive);
    g_inside    = reinterpret_cast<CharacterTestFn>(*inside);
    g_leader    = reinterpret_cast<LeaderFn>(*leader);
    g_root      = reinterpret_cast<const std::uintptr_t*>(*root);
    g_installed = true;
    platform::Log(L"arena warp: L3 twice in a fight brings player 2's or 3's character shut outside the fight's area in, with "
                  L"the game's own placement and battle warp");
    return true;
}

bool InBattle() noexcept
{
    if (!g_installed)
    {
        return false;
    }
    const std::uint8_t state = EncounterState();
    return state == facts::kEncounterInBattle || state == facts::kEncounterEscaping;
}

void Ask(int seat, const wchar_t* how) noexcept
{
    if (players::SeatBuilt(seat))
    {
        g_asked[static_cast<std::size_t>(seat)] = how;
    }
}

void Tick() noexcept
{
    if (!g_installed)
    {
        return;
    }
    const bool inBattle = InBattle();
    if (g_wasInBattle && !inBattle)
    {
        battle_warps::ForgetGather();
    }
    g_wasInBattle = inBattle;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto i = static_cast<std::size_t>(seat);
        if (const wchar_t* how = std::exchange(g_asked[i], nullptr); how != nullptr)
        {
            Bring(seat, how);
        }
        if (g_checkIn[i] != 0 && --g_checkIn[i] == 0)
        {
            void* member = players::weak_object::ObjectOf(g_warped[i]);
            bool  inside = false;
            const bool read = member != nullptr && Test(g_inside, member, inside);
            platform::Log(L"arena warp: {}'s character half a second after its warp: {}", players::SeatName(seat),
                          !read ? L"not read" : inside ? L"inside the fight's area" : L"STILL OUTSIDE the fight's area");
        }
    }
}

}  // namespace lss::battle::arena_warp
