#pragma once

// A seat's player picks up collectibles and materials with the game's own field prompts (research/RE-p2-field-interact.md).
//
// FieldActionMgr keeps every field prompt as a point and runs one selection per frame around one probe, the
// party leader, with one slot block: the candidates, the visibility-trace handles, eight button slots with
// their rows, records and latches. Nothing in it is per player. So right after the game's own update
// (player 1's; its call in the game mode's tick is redirected here), the same game code runs once more for
// each seat's player:
//   - inside the seat's scopes: the seat's pad's sticks and buttons, the seat's controller as the primary one, the
//     seat's member as the leader (and its type). Every gate and the probe then resolve to the seat's player;
//   - with the seat's copy of the manager's per-player ranges swapped in (player 1's put back after),
//     and of each point's carried target bits (+0x104 bits 1 and 2), kept by point key;
//   - with the game's own "point disabled" bit (+0x104 bit 4, which the candidate query skips) set on every
//     point that is neither one of WorldItemMgr's items, one of TreasurePointMgr's chests (the chest listener's
//     own test), a vending machine (its point preset rpaVendingMachine; world/vending), a shopkeeper (the game's
//     data names a shop action for its key; world/shop_talk) nor the seat's own box point (the point the seat's
//     leader-action component adds for the breakable box Yuffie's throw selects, by the key that component holds;
//     research/RE-yuffie-box-throw.md), and cleared again after: the seat's player is offered collectibles,
//     materials, chests, vending machines, shopkeepers and their own box; talk, doors, scripts and menus stay player
//     1's. A chest or machine the seat's player uses runs the game's own state (treasure, field action) on the seat's
//     member (the listener asks for the leader, which the scope makes the seat's); a shopkeeper's press fires the
//     NPC's own state trigger; the box gets the seat's own marker and prompt row from the game's code, and its key
//     becomes the seat's slot-0 target, which the throw's gate in the member tick compares with (SlotTargetHold);
//   - the input step's FieldAction part in the game's order (its gate, the slot poll into the mod's own
//     buffers, the latch, the press/execute tick), then the selection update;
//   - with the seat's controller first in the world's controller list for each visibility request the pass
//     makes: the request's screen test projects the point through "the first local player", so the
//     offer follows the seat's own screen, not player 1's;
//   - with each point's marker widget (+0x78) swapped too, on every live point, the seat's kept by point key:
//     the game's own marker code makes, drives and sweeps the seat's own widget in the pass, so the
//     two players no longer write one widget's state in turn. The seat's widgets the game would not destroy
//     (point gone between passes, field exit, the seat's player leaving) go through its own destroy call.
// Selection, traces, rows, markers, the look-at, the press and the grant are all the game's. The one global
// byte the press tick writes (root+0x210 -> +8) is put back after. The game's field enter/exit resets run
// on each seat's copy too, and its field-exit clear runs on it when the seat's player leaves.
//
// Three more hooks carry the pass's edges: the game mode tick's call of the selection update (the pass runs right
// after it), WorldItemMgr's mark by key (a key a seat's pass marks is noted) and the WorldItem tick's call of the
// grant (run with that seat's controller as the primary one for a noted key: the item-get rumble's only player read).
// Each tells the one observer (world/interact_watch, a dev tool) what passed through it.
//
// Logged: the scopes or gates that stopped a pass (counted), each seat's slot targets and marker widgets on
// change, each widget the mod destroys, a status line once a minute. Game thread only.
//
// Per seat (players/seats.h): each seat has its own pickup record (the copy of the five
// ranges, the carried target bits and marker widgets by point key, the WorldItem keys its pass marked, its counts),
// and the passes run in seat order after the game's own update, one seat's swap closed before the next one's opens.
// The visibility request puts the seat's controller first in the world's controller list, its entry found by the
// controller (never by its position). Shared: the scratch of a pass, player 1's markers as the last pass left them
// and the doomed widgets.

#include "platform/byte_scope.h"
#include "players/seats.h"

#include <array>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::world::field_pickup {

/// Finds the game's functions (each checked) and installs the field enter/exit redirects and the three hooks. Logged.
void Install(const platform::GameImage& image) noexcept;

/// Puts the game's code back (at mod uninstall, with every seat out).
void Remove() noexcept;

/// The seats' passes, right after the game's own selection update of `mgr` (the update hook calls it).
void AfterUpdate(void* mgr, float dt) noexcept;

/// What the three hooks tell the observer (each entry may be null): after the game's update and the seats' passes
/// (game thread); a WorldItem key marked (the game's answer, the caller's return address, and the seat whose pass
/// marked it, 0 for none; any thread); a grant, before the game's call and after it, inside the same scope (`seat`:
/// the seat whose controller the grant ran with as the primary one, 0 for none). The observer is a dev tool.
struct HookObserver
{
    void (*afterUpdate)(void* mgr) noexcept                                                          = nullptr;
    void (*marked)(std::uint64_t key, std::uint8_t taken, std::uintptr_t caller, int seat) noexcept = nullptr;
    void (*granting)(std::uint64_t key, int seat) noexcept                                           = nullptr;
    void (*granted)(std::uint64_t key, int seat) noexcept                                            = nullptr;
};

/// The one observer (a copy is kept). Set before any field exists, cleared at unload.
void SetHookObserver(const HookObserver& observer) noexcept;

/// `seat`'s copy of the slot rows (FWeakObjectPtr values, 0 for none), for hud/prompt_rows: the rows the seat's
/// pass made. All 0 while the seat has no copy, or inside a pass.
using Weaks = std::array<std::uint64_t, 8>;
void RowWeaks(int seat, Weaks& out) noexcept;

/// True while a seat's pass runs (game thread).
bool InPass() noexcept;

/// The seat whose pass runs now, or -1 (game thread).
int PassSeat() noexcept;

/// Whether a CharaPop entry's key is one of TreasurePointMgr's chests (its CharaPop key -> treasure key map at +0x60,
/// the spawn handler's own lookup). Game thread.
bool IsChestEntry(std::uint64_t popKey) noexcept;

/// A WorldItem key `seat`'s pass marked taken (the mark hook, inside the pass).
void NoteMarked(int seat, std::uint64_t key) noexcept;

/// The seat whose pass marked `key` (forgotten once asked), or -1: the grant then runs with that seat's controller
/// as the primary one, so the item-get rumble plays on its pad (the only player read in the grant,
/// research/RE-p2-field-interact.md).
int TakeMarked(std::uint64_t key) noexcept;

/// Whose point marker widget is (a UFieldTargetWidget at a point's +0x78), as of the seats' last passes: 0
/// player 1's, a seat (1..) that seat's, -1 neither (another owner's marker, or no seat has a copy). For
/// hud/field_markers, which shows each half only its own player's.
int MarkerOwner(const void* widget) noexcept;

/// The seat's slot-0 target (its copy's) in the live manager for the length of a call, player 1's back after (a Hold
/// range of platform/byte_scope.h): the member tick, where the seat's leader-action component compares its shooting
/// point's key with the manager's slot-0 target before Yuffie's throw fires. Opens only while the component holds a
/// shooting point (a box selected), the seat has a copy of the live manager and no pass runs. At its end, the throw's
/// gate as the tick left it (the box shown, can shoot, the point's key against the held target, a write of the game's
/// put back) at Debug on change, and the component's key records' names once per seat. `controller` is the seat's.
/// Game thread.
class SlotTargetHold
{
public:
    SlotTargetHold(int seat, const void* controller) noexcept;
    ~SlotTargetHold();
    SlotTargetHold(const SlotTargetHold&)            = delete;
    SlotTargetHold& operator=(const SlotTargetHold&) = delete;

private:
    platform::ByteStore<1, sizeof(std::uint64_t)> context_;
    int                                           seat_       = 0;
    const void*                                   controller_ = nullptr;
    std::uint64_t                                 target_     = 0;  // the seat's slot-0 target held
    bool                                          held_       = false;
};

}  // namespace lss::world::field_pickup
