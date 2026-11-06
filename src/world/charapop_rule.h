#pragma once

// CharaPop's Light/Middle rule, as its evaluators apply it to one camera (RE-p2-charapop-per-view.md §1.3:
// CharaPopEntry_EvalLOD_Light 0x1408B4644, _Middle 0x1408B3B84), for the entries a seat's camera needs
// (world/charapop_near.h). Pure, host-tested.
//
//   paused  when distance > should-tick and (in the FOV cone ? distance > in-camera : distance > out-of-camera)
//   hidden  when distance > draw distance (the entries that hide only)
//
// The in-camera distance gains the enemy bonus for enemy entries. `scale` widens every distance and
// `extraDegrees` the cone (1 and 0 for the game's own rule; more to keep an entry already raised, less to
// ask whether player 1's camera keeps it with room to spare).
//
// Each seat's camera is judged on its own against player 1's; an entry is
// raised when any seat's view needs it and kept while any does (SeatHolds, the union over the seats).

#include <cstdint>

namespace lss::world::charapop {

struct LodDistances
{
    float fovDegrees  = 0.0f;
    float shouldTick  = 0.0f;
    float inCamera    = 0.0f;
    float outCamera   = 0.0f;
    float enableDraw  = 0.0f;
    float enemyAdded  = 0.0f;
};

struct EntryView
{
    float distance     = 0.0f;  // cm, from the camera
    float angleDegrees = 0.0f;  // off the camera's forward
    bool  hides        = false;  // has the draw-distance hide
    bool  enemy        = false;
};

/// What the game would give the entry, judged from one camera.
struct Keeps
{
    bool awake = false;  // not tick-paused
    bool shown = false;  // not distance-hidden (only entries that hide)
};

constexpr Keeps KeepsOf(const LodDistances& lod, const EntryView& view, float scale = 1.0f,
                        float extraDegrees = 0.0f) noexcept
{
    const bool  inCone   = view.angleDegrees <= lod.fovDegrees + extraDegrees;
    const float inCamera = lod.inCamera + (view.enemy ? lod.enemyAdded : 0.0f);
    Keeps       keeps;
    keeps.awake = view.distance <= lod.shouldTick * scale || view.distance <= (inCone ? inCamera : lod.outCamera) * scale;
    keeps.shown = view.hides && view.distance <= lod.enableDraw * scale;
    return keeps;
}

/// True when, judged from this camera, the game would keep the entry ticking (or shown).
constexpr bool KeepsAwakeOrShown(const LodDistances& lod, const EntryView& view, float scale = 1.0f,
                                 float extraDegrees = 0.0f) noexcept
{
    const Keeps keeps = KeepsOf(lod, view, scale, extraDegrees);
    return keeps.awake || keeps.shown;
}

/// True when player 1's camera already gives the entry everything the seat's camera asks for, so the game
/// alone keeps it as the seat needs and raising it would add only cost.
constexpr bool Covers(const Keeps& one, const Keeps& forSeat) noexcept
{
    return (!forSeat.awake || one.awake) && (!forSeat.shown || one.shown);
}

// The margins that keep an entry from toggling on the edge: a raised entry stays raised until it is this far
// past the seat's distances, or this far inside player 1's.
inline constexpr float kKeepScale   = 1.1f;
inline constexpr float kKeepDegrees = 5.0f;

/// Whether to raise an entry not raised yet: the seat's view, at `reach` times the game's distances, keeps
/// it, and player 1's camera (when read) does not already.
constexpr bool ShouldRaise(const LodDistances& lod, const EntryView& seatView, const EntryView* one,
                           float reach) noexcept
{
    const Keeps forSeat = KeepsOf(lod, seatView, reach);
    if (!forSeat.awake && !forSeat.shown)
    {
        return false;
    }
    return one == nullptr || !Covers(KeepsOf(lod, *one), forSeat);
}

enum class Hold
{
    Keep,             // still needed
    OutOfReach,       // past the seat's distances (with the margin)
    PlayerOneCovers,  // player 1's camera keeps it (with the margin)
};

/// For an entry the mod raised: whether it is still needed, and why not.
constexpr Hold HoldOf(const LodDistances& lod, const EntryView& seatView, const EntryView* one, float reach) noexcept
{
    const Keeps forSeat = KeepsOf(lod, seatView, reach * kKeepScale, kKeepDegrees);
    if (!forSeat.awake && !forSeat.shown)
    {
        return Hold::OutOfReach;
    }
    if (one != nullptr && Covers(KeepsOf(lod, *one, 1.0f / kKeepScale, -kKeepDegrees), forSeat))
    {
        return Hold::PlayerOneCovers;
    }
    return Hold::Keep;
}

// ---- the seats: one view per seat, the union kept ----

/// A seat's bit in the raised record's mask (bit s for seat s, players/seats.h).
constexpr std::uint8_t SeatBit(int seat) noexcept
{
    return seat >= 0 && seat < 8 ? static_cast<std::uint8_t>(1u << seat) : std::uint8_t{0};
}

/// The holds of the seats' views of one raised entry, gathered in seat order: the entry stays raised while any seat's
/// view keeps it (the union); otherwise it is released as kept by player 1's camera when any seat's view found it so,
/// else as out of reach. With one seat the combined hold is that seat's own.
struct SeatHolds
{
    std::uint8_t keeping = 0;  // bit s: seat s's view still needs the entry (Hold::Keep)
    bool         covered = false;
    bool         any     = false;

    constexpr void Add(int seat, Hold hold) noexcept
    {
        any = true;
        keeping |= hold == Hold::Keep ? SeatBit(seat) : std::uint8_t{0};
        covered = covered || hold == Hold::PlayerOneCovers;
    }

    constexpr Hold Combined() const noexcept
    {
        return keeping != 0 ? Hold::Keep : covered ? Hold::PlayerOneCovers : Hold::OutOfReach;
    }
};

}  // namespace lss::world::charapop
