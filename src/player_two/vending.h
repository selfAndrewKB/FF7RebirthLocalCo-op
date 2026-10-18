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
//   - runs the activation in the leader-type scope (the default preset by player 2's member's type, §15.3); the camera stays
//     on player 1's camera manager, the one the whole-screen view renders, so the shot is taken from player 1's side
//     [known: the user, 2026-10-04, "a tad awkward" depending on where player 1 stands; the fix needs a whole-screen view of
//     player 2, left for later];
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

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::vending {

/// Installs the hooks (each checked). Logged.
void Install(const platform::GameImage& image) noexcept;

/// Whether player 2's pass should leave a point with this point-side preset open: a vending machine's, while no shop is up.
/// Game thread (inside the pass).
bool Admits(std::uint64_t pointPreset) noexcept;

/// Player 2 pressed the shopkeeper with this CharaPop key (player_two/shop_talk, inside player 2's pass).
void PressedShopkeeper(std::uint64_t key) noexcept;

/// No shop is up and no press of player 2's is pending (a vending machine's or a shopkeeper's).
bool ShopIdle() noexcept;

/// Player 2's press of a shopkeeper waits for its shop.
bool ShopkeeperPending() noexcept;

/// The shop up now is player 2's.
bool PlayerTwosShop() noexcept;

/// Once per frame (the engine-tick callback): a press that opened no shop is dropped; player 2 leaving ends their shop's pad.
void Tick() noexcept;

}  // namespace lss::player_two::vending
