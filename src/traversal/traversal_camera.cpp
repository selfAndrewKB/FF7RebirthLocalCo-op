#include "traversal/traversal_camera.h"

#include "platform/dev_switch.h"
#include "platform/log.h"
#include "players/generation.h"
#include "players/seats.h"
#include "traversal/traversal_collision.h"
#include "traversal/traversal_shared.h"

namespace lss::traversal::traversal_camera {

void Install(const platform::GameImage& image) noexcept
{
    try
    {
        const executers::Placement    executers = executers::Place(image);
        const holders::Placement moves     = holders::Place(image);
        platform::Log(L"traversal camera: player 2's traversal cameras: {}; {} (a NO keeps that class on the follow "
                      L"camera for player 2)",
                      executers.classes, moves.classes);
        platform::Log(L"traversal camera: climbing walls, monkey bars and spider walls {}",
                      moves.seatHolders
                          ? L"keep a camera holder per player (neither player's climb evicts the other's)"
                          : L"keep the game's one holder (a hook or climb_context's Ticks are missing): the "
                            L"first climber keeps the camera, player 2's is evicted by player 1's");
        platform::Log(L"traversal camera: squeeze exit {}, crawl exit {}, one-shot end {} (player 2's camera off where "
                      L"the move ends, as player 1's)",
                      executers.squeezeExit ? L"yes" : L"NO", executers.crawlOff ? L"yes" : L"NO",
                      executers.oneShotLeave
                          ? L"yes"
                          : L"NO (player 2's one-shot camera ends at its own timer; the end "
                            L"can switch off player 1's slot 15)");
        [[maybe_unused]] const bool climbingWatched = collision::Install(image);
#if LSS_DEV
        const wchar_t* collisionText = climbingWatched
                                           ? L"on (Debug, a line per traversal camera)"
                                           : L"for player 2's ladder only (the climbing module's vtable is not found)";
#else
        const wchar_t* collisionText = L"off (dev tools)";
#endif
        platform::Log(L"traversal camera: player 2's ladder exit {}; climbing wall hip framing {}; collision watch {}",
                      executers.ladderExit ? L"blends into the follow camera as player 1's (the game's request)"
                                           : L"UNAVAILABLE (the request's bytes differ: a cut at the top)",
                      moves.hip ? L"follows player 2's hold phases"
                                : L"UNAVAILABLE (a call differs: player 2 climbs without it)",
                      collisionText);
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    const bool executers = executers::Remove();
    const bool moves     = holders::Remove();
    if (executers || moves)
    {
        platform::Log(L"traversal camera: a slot or call was changed after us; left in place (it passes through)");
    }
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const bool active = players::generation::Active(seat);
        executers::Tick(seat, active);
        holders::Tick(seat, active);
    }
    collision::Tick(players::generation::Active(players::kFirstFurtherSeat));  // a dev tool: player 1's and player 2's cameras
}

}  // namespace lss::traversal::traversal_camera
