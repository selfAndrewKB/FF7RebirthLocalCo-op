#include "player_two/session/seat_out.h"

#include "player_two/battle/lock_on.h"
#include "player_two/camera/camera_override.h"
#include "player_two/feedback/ffb_manager_two.h"
#include "player_two/feedback/rumble_list_two.h"
#include "players/seats.h"

namespace lss::player_two::seat_out {

static_assert(players::kSeatsBuilt == 1, "the modules below are player 2's: each takes the seat before seat 2 is "
                                         "built (plan/seats.md step 3)");

void LetGo(int seat) noexcept
{
    if (seat != players::kPlayerTwoSeat)
    {
        return;  // the modules below hold player 2's only
    }
    camera_override::PlayerTwoOut(seat);  // the member's camera delegates back to the engine's, while it is still held
    lock_on::PlayerTwoOut(seat);
    rumble_list_two::PlayerTwoOut(seat);  // stops the seat's channels while it can still be the primary player
    ffb_manager_two::PlayerTwoOut(seat);  // the seat's component back on the game's force-feedback manager
}

}  // namespace lss::player_two::seat_out
