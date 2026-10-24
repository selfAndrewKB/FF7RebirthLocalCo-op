#pragma once

// The field-interaction modules' pure pieces (vending, shop_talk, trigger_two, scope_leaks), tested on the host
// (tests/field_rules_test.cpp).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <string>
#include <string_view>

namespace lss::player_two::field_rules {

// ---- vending: the shopkeeper's name ----

/// The text after the first '_', lower case (the NPC's ActorName and its CharaPop key share it: Npc2100_shopweapon01
/// and oba2100_ShopWeapon01; RE-p2-shopkeeper-talk.md §7.3).
inline std::wstring AfterPrefix(const std::wstring& name)
{
    const auto   at   = name.find(L'_');
    std::wstring rest = at == std::wstring::npos ? name : name.substr(at + 1);
    for (wchar_t& c : rest)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return rest;
}

// ---- shop_talk: the game's mapped data ----

/// A mapped field's data: a pointer, or a signed offset from the field when the low bit is set (shared strings point
/// back). Null for an empty field.
inline const std::byte* MappedPointer(const std::byte* field, std::uint64_t value) noexcept
{
    if (value == 0)
    {
        return nullptr;
    }
    return (value & 1) != 0 ? field + (static_cast<std::int64_t>(value) >> 1) : reinterpret_cast<const std::byte*>(value);
}

/// Row names are ASCII: anything else means the offset is wrong.
constexpr bool IsRowName(std::wstring_view text) noexcept
{
    for (const wchar_t c : text)
    {
        if (c < 0x20 || c > 0x7E)
        {
            return false;
        }
    }
    return true;
}

// ---- trigger_two: the seats' trigger events ----

enum class TriggerRoute
{
    Original,   // no seat's trigger: the game's filter as is
    PlayerOne,  // player 1's event while a seat is inside a switch volume: the game's filter, then the union
    PlayerTwo,  // a seat's trigger (the route's seat): admitted classes go to the game as that seat's player
};

/// Where a trigger event goes, and for a seat's trigger, which seat it is.
struct Route
{
    TriggerRoute route = TriggerRoute::Original;
    int          seat  = -1;  // PlayerTwo: the seat whose trigger it is; else -1
};

/// `ownerSeat`: the seat whose trigger raised the event (1.., players/seats.h), 0 for player 1's trigger, -1 for none.
constexpr Route TriggerRouteOf(int ownerSeat, bool watchingSwitchVolumes) noexcept
{
    if (ownerSeat >= 1)
    {
        return {TriggerRoute::PlayerTwo, ownerSeat};
    }
    return {ownerSeat == 0 && watchingSwitchVolumes ? TriggerRoute::PlayerOne : TriggerRoute::Original, -1};
}

enum class TriggerVerdict
{
    NotAdmitted,    // its class is not admitted: held back
    PromptZipLine,  // a button-prompt zip line (only auto-start lines are admitted): held back
    Deliver,        // delivered as player 2
};

/// `classIndex` is the admitted class (negative: none); `enterEffectOnly` is that class's rule; `zipEnterEffect` the
/// actor's bUseEnterEffect.
constexpr TriggerVerdict TriggerVerdictOf(int classIndex, bool enterEffectOnly, std::uint8_t zipEnterEffect) noexcept
{
    if (classIndex < 0)
    {
        return TriggerVerdict::NotAdmitted;
    }
    return enterEffectOnly && zipEnterEffect == 0 ? TriggerVerdict::PromptZipLine : TriggerVerdict::Deliver;
}

// ---- scope_leaks: the input clock ----

/// The seconds from which adding `deltaTime` the game's way (float added to double) lands exactly on `target`, if one
/// is within `nudges` representable steps.
inline double SecondsBefore(double target, float deltaTime, int nudges) noexcept
{
    const double step   = static_cast<double>(deltaTime);
    double       before = target - step;
    for (int i = 0; i < nudges && step + before != target; ++i)
    {
        before = std::nextafter(before, step + before < target ? std::numeric_limits<double>::infinity()
                                                               : -std::numeric_limits<double>::infinity());
    }
    return before;
}

}  // namespace lss::player_two::field_rules
