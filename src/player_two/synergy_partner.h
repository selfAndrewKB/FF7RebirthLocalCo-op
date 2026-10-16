#pragma once

// Synergy skills and synergy abilities with a human's character as the partner (research/RE-p2-synergy-skills.md §4-§6,
// S-SYNSKILL; research/RE-p2-synergy-abilities.md §4-§6, S-SYNABIL).
//
// A synergy partner walks into place through its own AI's behaviour tree, so the game first hands "the player's
// character" to its AI (AEndCharacter::SwitchAIControl_Async(partner, 1, nullptr)) and gives it back to the primary
// player controller when the paired action starts or the link ends. Its only test for "the player's character" is the
// partner's type against the party leader type (PM+0x150), and the only controller it gives back to is PM+0x140. With
// two players that is one slot for two humans:
//   - player 2's member as the partner of player 1's synergy was never handed over (its AI stays paused by player 2's
//     possession), so it never joined;
//   - player 1's character as the partner of player 2's synergy was handed over only when the request ran outside
//     player 2's command window (a buffered press, a mode-2/3 ability), so it worked only sometimes;
//   - player 2's member handed over inside player 2's window would have been given back to nobody (or to player 1).
//
// The game's own hand-off answered for whichever human holds the partner:
//   - the three calls of LinkPartner_Start_HandLeaderToAI (at the request) and the three of
//     LinkPartner_RequestAI_HandLeaderToAI (the mode-2 tick, the deferred mode-1 start, the partner start) name the
//     partner for the call; the leader-type test inside each (modes 2/3 at the start, the AI request for modes 1/2)
//     answers the partner's own type when player 1's or player 2's controller holds the partner, so the game hands it
//     to its AI itself, and the character is remembered with its player;
//   - LinkMember_End_RepossessLeaderWithPrimaryPC names the member for the call; for a remembered character its
//     leader-type test answers the member's type and its primary-controller read answers that player's controller, so
//     the game gives it back to its own player, unless that player has switched to another character meanwhile (then it
//     stays with its AI, as in single player). Player 2's member is always given back to player 2's controller.
// Nothing else reads a changed leader: only those four calls inside the three functions are answered. Classic mode
// (deferred) is left to the game: its Classic hand-over keeps the controller. Placed from the game thread
// (game_thread_sites; some calls cross a 16-byte block), all or nothing. Game thread only; other threads pass through.

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::synergy_partner {

/// Resolves and checks the sites and queues their placement (logged when it runs).
void Install(const platform::GameImage& image) noexcept;

/// Game thread, once per engine tick: forgets characters of a finished session, warns about a partner the game has not
/// given back after a while (Info), and the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::player_two::synergy_partner
