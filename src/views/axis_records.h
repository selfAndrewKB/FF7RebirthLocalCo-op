#pragma once

// The players whose FOV rule the mod wrote (views/player_view_point.h): the value to put back, the rule the mod wrote,
// and the value last seen (so each change the game makes is noted once). Pure over its table, with the engine byte
// passed in, so it is tested on the host (tests/axis_records_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::views {

struct AxisRecord
{
    const void*  player   = nullptr;
    std::uint8_t original = 0;
    std::uint8_t applied  = 0;
    std::uint8_t seen     = 0;
};

enum class AxisApplied
{
    NoRoom,       // no record free to remember the original: the field is not changed
    First,        // the player's first split frame: the original remembered, the rule written
    RuleChanged,  // the mod's own rule changed: written again
    Unchanged,    // the field holds what was last seen
    GameWrote,    // the game wrote another value (a camera cut's): noted, left to the game
    GamePutBack,  // the game put the mod's rule back: noted
};

template <std::size_t N>
class AxisRecords
{
public:
    /// Gives `player`'s `field` the rule `rule` once: at its first split frame, or when the mod's rule changes. A value
    /// the game wrote over the rule is noted, not overwritten.
    AxisApplied Apply(const void* player, std::uint8_t& field, std::uint8_t rule) noexcept
    {
        AxisRecord* free = nullptr;
        for (auto& record : records_)
        {
            if (record.player == player)
            {
                if (record.applied != rule)  // the mod's own FOV mode changed
                {
                    record.applied = record.seen = field = rule;
                    return AxisApplied::RuleChanged;
                }
                if (field == record.seen)
                {
                    return AxisApplied::Unchanged;
                }
                record.seen = field;
                return field == record.applied ? AxisApplied::GamePutBack : AxisApplied::GameWrote;
            }
            if (record.player == nullptr && free == nullptr)
            {
                free = &record;
            }
        }
        if (free == nullptr)
        {
            return AxisApplied::NoRoom;
        }
        *free = {player, field, rule, rule};
        field = rule;
        return AxisApplied::First;
    }

    /// Puts every player's original back, except `removedPlayer`'s (its object is gone), and only where the field still
    /// holds a rule of the mod's (`isModRule`); `fieldOf(player)` gives the byte, or null to skip. `restored(original)`
    /// is called for each put back. Every record is forgotten.
    template <class FieldFn, class ModRuleFn, class RestoredFn>
    void Restore(const void* removedPlayer, FieldFn&& fieldOf, ModRuleFn&& isModRule, RestoredFn&& restored) noexcept
    {
        for (auto& record : records_)
        {
            if (record.player != nullptr && record.player != removedPlayer)
            {
                if (std::uint8_t* field = fieldOf(record.player); field != nullptr && isModRule(*field))
                {
                    *field = record.original;
                    restored(record.original);
                }
            }
            record = {};
        }
    }

    const std::array<AxisRecord, N>& Records() const noexcept { return records_; }

private:
    std::array<AxisRecord, N> records_{};
};

}  // namespace lss::views
