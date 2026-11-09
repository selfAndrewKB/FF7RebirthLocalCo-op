#pragma once

// Whether another player keeps a member the level walks to its marks, and when the game takes it over at a mark (the
// player keeps control without breaking the scripting, guided by the mod's own marker;
// Rebirth/Documentation/research/RE-walk-gate.md, RE-walk-keep.md).
//
// The lead gate (session/lead_gate.h) finds a lead or a run to a mark the level needs the member to finish (a waiter on
// its arrival, the story's marker on it, a further move in its own chain). Left to the gate alone, every such move goes
// to the member's AI (a guide). With the holder keeping it (`lead-holder keeps`):
//   - the player keeps the member and is shown the move's point (that player's view only);
//   - within kHandRadius of the point (2D) the member goes to its AI, which resumes the move from where it stands,
//     walks the last steps and signals the arrival;
//   - it stays on its AI until the level writes its next setting, then comes back.
// A move whose point cannot be marked is a guide, as is every needed move under `lead-holder guide`.
//
// Pure (host-tested in tests/lead_keep_rules_test.cpp).

#include <cstdint>

namespace lss::session::lead_keep_rules {

enum class Verdict : std::uint8_t
{
    Staging,  // nothing needs the move finished: the player's, no marker
    Guide,    // to its AI until the setting ends (no point to mark, or `lead-holder guide`)
    Keep,     // the player's, the point marked
    AtMark,   // brought to the point: to its AI until the setting ends
};

/// How near the point (cm, 2D) the member goes to its AI: the AI then walks the last steps (a run arrives at once
/// within its goal radius; RE-walk-keep.md); narrow enough that the player walks nearly all of it.
inline constexpr float kHandRadius = 200.0f;

/// The member stands within kHandRadius of the point, `dx`, `dy` apart (cm).
constexpr bool Reached(float dx, float dy) noexcept
{
    return dx * dx + dy * dy <= kHandRadius * kHandRadius;
}

/// The verdict for a move. `needed`: the gate's reasons (a waiter, the marker, a further move). `keeps`: the
/// `lead-holder keeps` switch with a marker to show. `pointKnown`: the move's point was read. `signals`: the move names
/// an arrival trigger. `reached`: the member is (or was, this setting) within kHandRadius of the point.
/// `intoOwnBox`: the step moves on at the member's own armed story box, which the walk carries it into on the level's
/// clock (session/lead_gate OwnBoxAhead): a guide whatever the switch and whether or not anything else needs it, since
/// a player walking it there may come too late.
constexpr Verdict Decide(bool needed, bool keeps, bool pointKnown, bool signals, bool reached, bool intoOwnBox = false) noexcept
{
    if (intoOwnBox)
    {
        return Verdict::Guide;
    }
    if (!needed)
    {
        return Verdict::Staging;
    }
    if (!keeps || !pointKnown)
    {
        return Verdict::Guide;
    }
    return signals && reached ? Verdict::AtMark : Verdict::Keep;
}

/// The verdicts that hand the member to its AI; each holds until the move's setting ends.
constexpr bool ToAi(Verdict verdict) noexcept
{
    return verdict == Verdict::Guide || verdict == Verdict::AtMark;
}

constexpr const wchar_t* Name(Verdict verdict) noexcept
{
    switch (verdict)
    {
    case Verdict::Staging:
        return L"staging (the player's)";
    case Verdict::Guide:
        return L"GUIDE (to its AI, the player's view on it)";
    case Verdict::Keep:
        return L"KEEP (the player's; its point marked in that player's view)";
    case Verdict::AtMark:
        return L"AT THE MARK (to its AI: it finishes the arrival, then back to the player at the level's next setting)";
    }
    return L"?";
}

}  // namespace lss::session::lead_keep_rules
