#include "battle/arena_warp.h"

#include "battle/battle_warps.h"
#include "battle/command_menu.h"
#include "battle/encounter_watch.h"
#include "facts/command_controller.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "rides/own_ride.h"
#include "world/catch_up_move.h"
#include "world/party_warp.h"
#include "world/party_warp_rules.h"

#include <array>
#include <chrono>
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
CharacterTestFn       g_inside    = nullptr;  // inside any battle area (read for the log; null when not found)
LeaderFn              g_leader    = nullptr;
const std::uintptr_t* g_root      = nullptr;

// Game thread.
std::array<const wchar_t*, players::kMaxPlayers> g_asked{};  // by player: the pending ask's words (null: none)
std::array<std::uint32_t, players::kMaxPlayers>  g_checkIn{};  // by player: ticks left to the after-warp inside test
std::array<std::uint64_t, players::kMaxPlayers>  g_warped{};   // by player: the warped character (weak)
bool                                             g_wasInBattle = false;
constexpr std::chrono::milliseconds              kOneWait{1500};  // how long player 1's ask waits for an action to end
std::chrono::steady_clock::time_point            g_oneUntil{};

const void* Root() noexcept
{
    std::uintptr_t root = 0;
    return g_root != nullptr && platform::CopyGuarded(&root, g_root, sizeof(root)) ? reinterpret_cast<const void*>(root) : nullptr;
}

std::uint8_t EncounterState() noexcept
{
    return ReadField<std::uint8_t>(players::actor_fields::RootEncounter(Root()), facts::kEncounterState);
}

bool EventHoldsParty() noexcept
{
    const void* state = players::actor_fields::RootGameState(Root());
    return state != nullptr && (ReadField<std::int32_t>(state, facts::kGameStateEventCounterA) != 0 ||
                                ReadField<std::int32_t>(state, facts::kGameStateEventCounterB) != 0);
}

bool Test(CharacterTestFn fn, const void* character, bool& out) noexcept
{
    return fn != nullptr && platform::CallGuarded(fn, out, character);
}

/// The game's battle-area test on `character`, in words for the log; `outside` names the outside answer.
const wchar_t* AreaReading(const void* character, const wchar_t* outside) noexcept
{
    bool inside = false;
    return !Test(g_inside, character, inside) ? L"its area test not read"
           : inside                           ? L"inside the fight's area by the game's test"
                                              : outside;
}

/// Player 1's character (the game's party leader), or null when not read.
void* PlayerOne() noexcept
{
    void* one = nullptr;
    return platform::CallGuarded(g_leader, one) ? one : nullptr;
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
    return nullptr;
}

/// The gather's own placement for `mover` around the character at `anchorAt` facing `anchorYaw` (radians): its navmesh
/// query at its angles and distances, a spot another player stands on skipped; false (`dest` the anchor's own spot, the
/// gather's fallback) when none is found.
bool Destination(void* mover, const float anchorAt[3], float anchorYaw, float dest[3]) noexcept
{
    dest[0] = anchorAt[0];
    dest[1] = anchorAt[1];
    dest[2] = anchorAt[2];
    for (const float turn : kTurnsDegrees)
    {
        for (const float distance : kDistancesCm)
        {
            float point[3] = {};
            if (world::catch_up_move::PointAround(mover, anchorAt, anchorYaw + turn * kPi / 180.0f, distance, point) &&
                !world::catch_up_move::SpotTaken(point, mover))
            {
                dest[0] = point[0];
                dest[1] = point[1];
                dest[2] = point[2];
                return true;
            }
        }
    }
    return false;
}

/// Moves `mover` (asked by `player`) beside the character at `anchorAt` with the game's battle warp, logged as `who` going
/// to `toWhom`; the area test half a second after.
void MoveBeside(int player, void* mover, const float anchorAt[3], float anchorYaw, const std::wstring& who, const wchar_t* how,
                const wchar_t* toWhom)
{
    float          dest[3] = {};
    const bool     placed  = Destination(mover, anchorAt, anchorYaw, dest);
    const wchar_t* area    = AreaReading(mover, L"outside the fight's area by the game's test");  // where the game counted it
    if (!world::catch_up_move::Move(mover, dest, anchorYaw * 180.0f / kPi, anchorAt, true))
    {
        platform::LogWarning(L"arena warp: the move of {} raised; it may stand where it was", who);
        return;
    }
    g_warped[static_cast<std::size_t>(player)]  = players::weak_object::WeakOf(mover);
    g_checkIn[static_cast<std::size_t>(player)] = kCheckAfterTicks;
    platform::Log(L"arena warp: {} brought into the fight ({}; it was {}): {} {}, at ({:.1f}, {:.1f}, {:.1f}) m, with the "
                  L"game's battle warp",
                  who, how, area, placed ? L"beside" : L"no navmesh point beside, so onto the spot of", toWhom, dest[0] / 100,
                  dest[1] / 100, dest[2] / 100);
}

void Bring(int seat, const wchar_t* how) noexcept
try
{
    void*       member  = players::generation::MemberOf(seat);
    void*       one     = PlayerOne();
    const bool  haveOne = one != nullptr;
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
    MoveBeside(seat, member, oneAt, oneYaw, who, how, L"player 1's character");
}
catch (...)
{
    platform::Log(L"arena warp: an exception; not moved");
}

/// Player 1's command menu is open (its wait mode set, or its member list up): a move under it is refused.
bool OneMenuOpen() noexcept
{
    const void* ctrl = command_menu::PlayerOneCommandController();
    return ctrl != nullptr && (ReadField<std::int32_t>(ctrl, facts::kCmdWaitFilter) != 0 ||
                               ReadField<std::uint8_t>(ctrl, facts::kCmdSwitchingLayer) != 0);
}

/// Player 1's character is in its idle control state: the battle warp's reset to idle cuts nothing.
bool OneIdle(const void* one) noexcept
{
    return ReadField<std::uint8_t>(ReadPointer(one, facts::kCharacterControlHolder), facts::kCharCtrlHolderState) ==
           facts::kControlStateIdle;
}

/// Why player 1's character cannot be moved to `starter`'s (whose character is `anchor`, at `anchorAt`) now (null: it
/// can). `wait`: set when only a moment is wanting (player 1 mid-action): the ask is tried again.
const wchar_t* OneRefusal(void* one, int starter, const void* anchor, const float anchorAt[3], bool& wait) noexcept
{
    const std::uint8_t state = EncounterState();
    bool               flag  = false;
    wait                     = false;
    if (state == facts::kEncounterEscaping)
    {
        return L"player 1 is escaping the fight";
    }
    if (state != facts::kEncounterInBattle)
    {
        return L"no fight";
    }
    if (!players::SeatBuilt(starter))
    {
        return L"the fight is player 1's own (the enemies noticed player 1)";
    }
    if (anchor == nullptr)
    {
        return L"the player who started it plays no character now";
    }
    if (!battle_warps::GatheredFor(one))
    {
        return L"this fight's start gathered no one, or kept player 1's character out (the game's own data)";
    }
    if (!Test(g_alive, one, flag) || !flag)
    {
        return L"player 1's character is knocked out or petrified";
    }
    if (EventHoldsParty())
    {
        return L"an event holds the party";
    }
    if (world::party_warp::InFieldAction(one))
    {
        return L"player 1's character is in a field action";
    }
    float at[3] = {};
    if (!players::unreal::ActorLocation(one, at))
    {
        return L"player 1's place not read";
    }
    if (world::party_warp_rules::Beside(std::hypot(at[0] - anchorAt[0], at[1] - anchorAt[1]), std::fabs(at[2] - anchorAt[2])))
    {
        return L"player 1's character already stands beside theirs";
    }
    if (OneMenuOpen())
    {
        return L"player 1's command menu is open (close it first)";
    }
    if (ReadPointer(one, facts::kCharacterControlHolder) == nullptr)
    {
        return L"player 1's control state not read";
    }
    if (!OneIdle(one))
    {
        wait = true;
        return L"player 1's character is mid-action (an attack, an ability, a guard, a dodge or a hit)";
    }
    return nullptr;
}

/// Player 1's ask: their character beside the character of the player 2 or 3 whose fight it is (the enemies noticed
/// them). True when the ask is done (made or refused); false while it waits for player 1 to finish an action.
bool BringOne(const wchar_t* how, bool lastTry) noexcept
try
{
    void*       one     = PlayerOne();
    const bool  haveOne = one != nullptr;
    const int   starter = encounter_watch::NoticedPlayer();
    const void* anchor  = players::SeatBuilt(starter) ? players::generation::MemberOf(starter) : nullptr;
    float       anchorAt[3] = {}, anchorYaw = 0.0f;
    const bool  placeRead = anchor != nullptr && players::unreal::ActorLocation(anchor, anchorAt) &&
                           players::unreal::ActorYaw(anchor, anchorYaw);
    bool           wait = false;
    const wchar_t* why  = !haveOne                     ? L"player 1's character not read"
                          : anchor != nullptr && !placeRead ? L"the starter's place not read"
                                                            : OneRefusal(one, starter, anchor, anchorAt, wait);
    if (why != nullptr)
    {
        if (wait && !lastTry)
        {
            return false;
        }
        platform::Log(L"arena warp: player 1 asked to go to the fight's starter ({}); not done: {}", how, why);
        return true;
    }
    const std::wstring who  = std::format(L"player 1's {}", players::NameOf(one));
    const std::wstring whom = std::format(L"{}'s character (the fight's starter)", players::SeatName(starter));
    MoveBeside(0, one, anchorAt, anchorYaw, who, how, whom.c_str());
    return true;
}
catch (...)
{
    platform::Log(L"arena warp: an exception; player 1 not moved");
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto alive  = platform::FindFunction(image, facts::kIsBattleActive);
    const auto inside = platform::FindFunction(image, facts::kInsideAnyBattleArea);
    const auto leader = platform::FindFunction(image, facts::kResolvePartyLeader);
    const auto root   = players::game_globals::Address(players::game_globals::Global::GameRoot);
    if (!alive || !leader || !root || !world::catch_up_move::Installed())
    {
        platform::Log(L"arena warp: an engine check failed; L3 twice in a fight brings no character in");
        return false;
    }
    g_alive     = reinterpret_cast<CharacterTestFn>(*alive);
    g_inside    = inside ? reinterpret_cast<CharacterTestFn>(*inside) : nullptr;
    g_leader    = reinterpret_cast<LeaderFn>(*leader);
    g_root      = reinterpret_cast<const std::uintptr_t*>(root);
    g_installed = true;
    platform::Log(L"arena warp: L3 twice in a fight brings player 2's or 3's character in beside player 1's, with the game's own "
                  L"placement and battle warp{}",
                  inside ? L"" : L" (the game's battle-area test not found: the log does not say where the game counted it)");
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
    if (players::PlayerBuilt(seat))
    {
        g_asked[static_cast<std::size_t>(seat)] = how;
    }
    if (seat == 0)
    {
        g_oneUntil = std::chrono::steady_clock::now() + kOneWait;
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
    for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
    {
        const auto i = static_cast<std::size_t>(seat);
        if (seat == 0 && g_asked[0] != nullptr)
        {
            // Player 1's ask waits a moment for an action to end, then is made or refused.
            if (BringOne(g_asked[0], std::chrono::steady_clock::now() >= g_oneUntil))
            {
                g_asked[0] = nullptr;
            }
        }
        else if (const wchar_t* how = std::exchange(g_asked[i], nullptr); how != nullptr)
        {
            Bring(seat, how);
        }
        if (g_checkIn[i] != 0 && --g_checkIn[i] == 0)
        {
            void* member = players::weak_object::ObjectOf(g_warped[i]);
            platform::Log(L"arena warp: {}'s character half a second after its warp: {}", players::SeatName(seat),
                          member != nullptr ? AreaReading(member, L"STILL OUTSIDE the fight's area by the game's test") : L"gone");
        }
    }
}

}  // namespace lss::battle::arena_warp
