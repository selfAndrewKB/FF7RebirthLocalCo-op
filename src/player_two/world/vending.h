#pragma once

// Player 2 uses vending machines with the game's own field action and shop (RE-p2-field-interact.md §15; plan, "Player 2
// triggers field prompts").
//
// Player 2's field pass (player_two/field_pickup) admits a vending machine's point (its point-side preset is the FName
// rpaVendingMachine, §15.5) while no shop is up. Pressed there, the game's End2 listener asks the leader, which the pass's
// scope makes player 2's member, for the game's FieldActionCommon state: the member turns to the machine and the machine
// posts its ShopActionID to the level script, which opens the one whole-screen shop on its next tick (FEndShowShopMenu).
// That shop runs outside the pass. Its script passes the machine's own name as the camera's target (the machine's Tags[0];
// the live log, 2026-10-04), so the camera already aims at the machine used. For a shop player 2's press opened (the shop
// camera activated within 60 frames of the press, naming the pressed machine), this module, at the shop camera's entry
// (every shop path: FEndShowShopMenu's and the others, such as the book vendor's):
//   - skips the activation (the user, 2026-10-18: "When P2 initiates them, just disable the entire scripted camera zoom
//     action on P1's view"; RE-p2-shop-camera-skip.md): on player 1's camera manager, the one the whole-screen view
//     renders, it framed player 1's character and left player 1's camera at the shop's angle on close. Nothing in the
//     shop waits on it; the deactivation at the close still runs and does nothing. Chadley's shop keeps it (its VR return
//     activates the camera again outside the hook), run in the leader-type scope (the default preset by player 2's
//     member's type, §15.3);
//   - from the activation to the camera's deactivation (every close), makes pad 2 the menu pad (player_two/pad_buttons
//     SetMenuPlayer): the pad of whoever opened the shop drives it.
// Logged (Info: a press, a shop open and close are rare): each End2 press (whose run, the leader), whose shop opened with
// its camera target, preset and calling site, the close and how far player 2's member moved while it was up (the mod's own
// movement of the member must not fight the shop), a press that opened no shop within 5 s, and a press a shop did not
// match. Game thread.
//
// Shopkeepers (player_two/shop_talk, RE-p2-shopkeeper-talk.md) open the same shop command: player 2's press of one is
// recorded here (PressedShopkeeper) and claims the first shop camera within 15 s whose target, the NPC's ActorName,
// shares the pressed CharaPop key's text after its first '_' (Npc2100_shopweapon01 / oba2100_ShopWeapon01); the press
// stays pending through any other shop.
//
// Per seat (players/seats.h): "player 2" above is each seat. Each seat has its own pending machine press and shopkeeper
// press; a shop camera is claimed by the first seat, in seat order, whose machine press names it, else whose
// shopkeeper press does. One shop is up at a time (the game has one), owned by player 1 or by a seat, and the owning
// seat's pad drives it (pad_buttons numbers its players as the seats).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::vending {

/// Installs the hooks (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with player 2 out).
void Remove() noexcept;

/// Whether a seat's pass should leave a point with this point-side preset open: a vending machine's, while no shop is
/// up and no seat's shopkeeper press waits. Game thread (inside the pass).
bool Admits(std::uint64_t pointPreset) noexcept;

/// `seat` pressed the shopkeeper with this CharaPop key (player_two/shop_talk, inside the seat's pass).
void PressedShopkeeper(int seat, std::uint64_t key) noexcept;

/// No shop is up and no seat's press is pending (a vending machine's or a shopkeeper's).
bool ShopIdle() noexcept;

/// A seat's press of a shopkeeper waits for its shop (any seat's; with `seat`, that seat's).
bool ShopkeeperPending() noexcept;
bool ShopkeeperPending(int seat) noexcept;

/// The seat whose shop is up now, or -1 (none, or player 1's).
int ShopSeat() noexcept;

/// Once per frame (the engine-tick callback): a press that opened no shop is dropped; a seat leaving ends its shop's
/// pad.
void Tick() noexcept;

}  // namespace lss::player_two::vending
