#pragma once

// A further player's scope ladder over the modules' scope types (player_two/scopes/scope_ladder.h is the mechanism).
//
// The parts, in the order they open: Focus (player 2's UI context, focus_scope), Sticks (pad 2's
// sticks, pad_sticks), Primary (player 2 as the primary controller, primary_controller), Buttons
// (pad 2's buttons, pad_buttons), Targets (player 2's target fields, lock_on), Leader and LeaderType
// (the party leader and its type answering player 2's member, leader_scope), Rules (the command
// rules turned around, command_rules). `Which` names them: a site opens the ones its call reads,
// required unless it says they are optional, and its own module-local scopes after the ladder. The ladder is a seat's
// (Args::seat, player 2's unless a site names another): Primary, Buttons, Focus, Targets and the member's pad follow
// it; the other parts are still player 2's until their modules are per seat (plan/seats.md step 3).
// Game thread only.

#include "player_two/battle/command_rules.h"
#include "player_two/battle/focus_scope.h"
#include "player_two/battle/lock_on.h"
#include "player_two/generation.h"
#include "player_two/input/pad_buttons.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/scopes/primary_controller.h"
#include "player_two/scopes/scope_ladder.h"
#include "player_two/session/party_member.h"
#include "players/seats.h"

#include <cstdint>

namespace lss::player_two::p2_scope {

enum Which : unsigned
{
    Focus      = 1u << 0,
    Sticks     = 1u << 1,
    Primary    = 1u << 2,
    Buttons    = 1u << 3,
    Targets    = 1u << 4,
    Leader     = 1u << 5,
    LeaderType = 1u << 6,
    Rules      = 1u << 7,
};

/// What the parts open with. A part not chosen needs nothing.
struct Args
{
    int           seat       = players::kPlayerTwoSeat;  // whose ladder: Primary and FillMember's pad
    void*         controller = nullptr;  // Focus: the seat's command controller
    std::int32_t  padId      = -1;       // Sticks: the pad (pad_sticks::ControllerIdOf)
    std::uint64_t withoutKey = 0;        // Buttons: an action the seat's view shows as not held (0 none)
    std::uint64_t memberWeak = 0;        // Leader: player 2's member's weak value (leader_scope::WeakOf)
    std::uint8_t  memberType = 0;        // LeaderType: the member's player type
    std::uint8_t  leaderType = 0;        // Rules: player 1's leader type
};

/// The member's weak value and type, and the seat's pad, filled in for `member` (`args.seat`'s). False, nothing
/// filled, when the member or its type cannot be read.
inline bool FillMember(Args& args, void* member) noexcept
{
    const auto type = party_member::PlayerTypeOf(member);
    const auto weak = leader_scope::WeakOf(member);
    if (!type || weak == 0)
    {
        return false;
    }
    args.memberWeak = weak;
    args.memberType = *type;
    args.padId      = pad_sticks::ControllerIdOf(generation::ControllerOf(args.seat));
    return true;
}

struct FocusPart
{
    static constexpr unsigned       kBit  = Focus;
    static constexpr const wchar_t* kName = L"player 2's UI context";
    explicit FocusPart(const Args& args) noexcept : scope(args.seat, args.controller) {}
    bool           Held() const noexcept { return scope.Entered(); }
    const wchar_t* Refusal() const noexcept { return scope.Refusal(); }
    focus_scope::FocusScope scope;
};

struct SticksPart
{
    static constexpr unsigned       kBit  = Sticks;
    static constexpr const wchar_t* kName = L"pad 2's sticks";
    explicit SticksPart(const Args& args) noexcept : scope(args.padId) {}
    bool Held() const noexcept { return scope.Held(); }
    pad_sticks::StickScope scope;
};

struct PrimaryPart
{
    static constexpr unsigned       kBit  = Primary;
    static constexpr const wchar_t* kName = L"the primary controller";
    explicit PrimaryPart(const Args& args) noexcept : scope(args.seat) {}
    bool           Held() const noexcept { return scope.Held(); }
    const wchar_t* Refusal() const noexcept
    {
        return scope.Held() ? nullptr : primary_controller::Describe(scope.Why());
    }
    primary_controller::Scope scope;
};

struct ButtonsPart
{
    static constexpr unsigned       kBit  = Buttons;
    static constexpr const wchar_t* kName = L"pad 2's buttons";
    explicit ButtonsPart(const Args& args) noexcept : scope(args.seat, args.withoutKey) {}
    bool Held() const noexcept { return scope.Held(); }
    pad_buttons::ButtonScope scope;
};

struct TargetsPart
{
    static constexpr unsigned       kBit  = Targets;
    static constexpr const wchar_t* kName = L"player 2's target fields";
    explicit TargetsPart(const Args& args) noexcept : scope(args.seat) {}
    bool Held() const noexcept { return scope.Held(); }
    lock_on::TargetScope scope;
};

struct LeaderPart
{
    static constexpr unsigned       kBit  = Leader;
    static constexpr const wchar_t* kName = L"the party leader";
    explicit LeaderPart(const Args& args) noexcept : scope(args.memberWeak) {}
    bool Held() const noexcept { return scope.Held(); }
    leader_scope::LeaderScope scope;
};

struct LeaderTypePart
{
    static constexpr unsigned       kBit  = LeaderType;
    static constexpr const wchar_t* kName = L"the party leader's type";
    explicit LeaderTypePart(const Args& args) noexcept : scope(args.memberType) {}
    bool Held() const noexcept { return scope.Held(); }
    leader_scope::LeaderTypeScope scope;
};

struct RulesPart
{
    static constexpr unsigned       kBit  = Rules;
    static constexpr const wchar_t* kName = L"the command rules";
    explicit RulesPart(const Args& args) noexcept : rules(args.leaderType) {}
    bool Held() const noexcept { return true; }
    command_rules::PlayerTwoRules rules;
};

using P2Scope = Ladder<Args, FocusPart, SticksPart, PrimaryPart, ButtonsPart, TargetsPart, LeaderPart, LeaderTypePart,
                       RulesPart>;

}  // namespace lss::player_two::p2_scope
