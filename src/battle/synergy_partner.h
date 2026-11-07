#pragma once

// Synergy skills and synergy abilities with a human's character as the partner (research/RE-p2-synergy-skills.md §4-§6,
// S-SYNSKILL; research/RE-p2-synergy-abilities.md §4-§6, S-SYNABIL).
//
// A synergy partner walks into place through its own AI's behaviour tree, so the game first hands "the player's
// character" to its AI (AEndCharacter::SwitchAIControl_Async(partner, 1, nullptr)) and gives it back to the primary
// player controller when the paired action starts or the link ends. Its only test for "the player's character" is the
// partner's type against the party leader type (PM+0x150), and the only controller it gives back to is PM+0x140. With
// seats in, that is one slot for all the humans:
//   - a seat's member as the partner of player 1's synergy was never handed over (its AI stays paused by the seat's
//     possession), so it never joined;
//   - player 1's character as the partner of a seat's synergy was handed over only when the request ran outside
//     the seat's command window (a buffered press, a mode-2/3 ability), so it worked only sometimes;
//   - a seat's member handed over inside the seat's window would have been given back to nobody (or to player 1).
//
// The game's own hand-off answered for whichever human holds the partner:
//   - the three calls of LinkPartner_Start_HandLeaderToAI (at the request) and the three of
//     LinkPartner_RequestAI_HandLeaderToAI (the mode-2 tick, the deferred mode-1 start, the partner start) name the
//     partner for the call; the leader-type test inside each (modes 2/3 at the start, the AI request for modes 1/2)
//     answers the partner's own type when player 1's or a seat's controller holds the partner, so the game hands it
//     to its AI itself, and the character is remembered with its player;
//   - LinkMember_End_RepossessLeaderWithPrimaryPC names the member for the call; for a remembered character its
//     leader-type test answers the member's type and its primary-controller read answers that player's controller, so
//     the game gives it back to its own player, unless that player has switched to another character meanwhile (then it
//     stays with its AI, as in single player) or no longer owns it (a seat gave it back during the approach, or
//     another human drives it: the game's own test decides). A seat's member is always given back to that seat's
//     controller.
// A parry-window skill (Counterfire: its initiator's ability row has kind 26, the window that ends the link when the
// parry fails; RE S-SYNCOUNTER) keeps a human's partner with its player at the start: the game asks for that partner
// only when the counter animation opens the record's combination window, through the AI request, whose test then hands
// it over as above; after a failed parry it never asks (confirmed live).
// Nothing else reads a changed leader: only those four calls inside the three functions are answered. Classic mode
// (deferred) is left to the game: its Classic hand-over keeps the controller. Placed from the game thread
// (game_thread_sites; some calls cross a 16-byte block), all or nothing.
//
// Threads: the hand-over runs on the game thread (other threads pass through). A mode-2 partner's own command end calls
// LinkMember_End wherever its command executor finishes, which the game writes for worker threads
// (RE-p2-synergy-skills.md S-SYNSTUCK2). There the member end is only noted (T3 fails for a seat's member and passes
// through otherwise; P never names a seat's controller), so nothing the game queues is ever possessed by a seat,
// and the next tick gives a remembered character back on the game thread with the game's own call,
// SwitchAIControl_Async(member, 1, its player's controller), after the same ownership tests as T3, read live. Answering T3/P on the worker instead would let the game queue that possession with raw pointers
// that nothing checks when it runs (S-SYNANYTHREAD Q1): it could outlive the player's ownership or controller. The tick
// also watches each remembered partner's live link record and logs one that vanished with it still not given back.
//
// Per seat: every built seat's player is a human (its member, its
// controller); a remembered character carries its player's number (1 player 1, seat + 1 a seat's player), the seat
// found from the controller that drives it or the member it is. A change of any seat's session forgets every
// remembered character, as player 2's did.

namespace lss::platform {
class GameImage;
}

namespace lss::battle::synergy_partner {

/// Resolves and checks the sites and queues their placement (logged when it runs).
void Install(const platform::GameImage& image) noexcept;

/// Switches the answers off, puts the member end's entry back and forgets the humans' partners. The call redirects
/// stay (some cross a block, which only the game thread may change); each passes through once off.
void Remove() noexcept;

/// Game thread, once per engine tick: forgets characters of a finished session, warns about a partner the game has not
/// given back after a while (Info), and the status line (Debug, every 10 s with activity).
void Tick() noexcept;

}  // namespace lss::battle::synergy_partner
