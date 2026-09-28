#include "player_two/party_member.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::party_member {

namespace {

// The predicates return their answer in al only, so they are read as bytes.
using EnsureTriggerFn   = void* (*)(void* controller);  // its return value is not the trigger
using SwitchLeaderFn    = void (*)(void* controller, void* character, bool inputTriggered, bool force);
using RosterCountFn     = std::int32_t (*)();
using RosterTypeFn      = std::uint8_t (*)(std::int32_t ordinal);
using CharacterByTypeFn = void* (*)(std::uint8_t lane, std::uint8_t playerType);
using ObjectTestFn      = std::uint8_t (*)(const void* object);
using ObjectToObjectFn  = void* (*)(const void* object);
using GlobalObjectFn    = void* (*)();
using TypeFn            = std::uint8_t (*)();                           // al only
using TypeTestFn        = std::uint8_t (*)(std::uint8_t playerType);    // al only

struct Functions
{
    EnsureTriggerFn   ensureTrigger       = nullptr;
    SwitchLeaderFn    switchLeader        = nullptr;
    RosterCountFn     rosterCount         = nullptr;
    RosterTypeFn      rosterType          = nullptr;
    CharacterByTypeFn characterByType     = nullptr;
    ObjectTestFn      isEndCharacter      = nullptr;
    ObjectToObjectFn  aiControllerOf      = nullptr;
    ObjectToObjectFn  validateAiController = nullptr;
    GlobalObjectFn    globalLeader        = nullptr;
    ObjectTestFn      isBattleActive      = nullptr;  // player 1's battle switch's member test T
    TypeFn            fieldLeaderType     = nullptr;
    TypeFn            defaultBattleLeader = nullptr;
    TypeTestFn        isBattleMember      = nullptr;
};
Functions         g_fn;  // set once at install
bool              g_installed    = false;
const void* const* g_optionHolder = nullptr;  // engine_facts.h: kOptionHolderGlobal
const void* const* g_battleState  = nullptr;  // engine_facts.h: kBattleStateGlobal

// The two engine calls that change game state run inside structured-exception guards
// (platform/guarded.h).

bool GuardedEnsureTrigger(void* controller) noexcept
{
    __try
    {
        g_fn.ensureTrigger(controller);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool GuardedSwitchLeader(void* controller, void* character) noexcept
{
    __try
    {
        g_fn.switchLeader(controller, character, false, false);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Player 1's battle switch's own member test T, the game's function (RE-p2-party-switch.md §1.2): the
/// member is live, has its battle controller component, and is neither dead nor petrified. The first
/// rule for player 2 is to be bound by it the way player 1 is.
bool BattleReady(const void* character) noexcept
{
    __try
    {
        return g_fn.isBattleActive(character) != 0;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The types the game hands back to player 1: the field leader (at every battle end) and the default
/// battle leader (at some battle starts).
bool LeaderSlot(std::uint8_t playerType) noexcept
{
    return playerType == g_fn.fieldLeaderType() || playerType == g_fn.defaultBattleLeader();
}

template <class Fn>
bool Find(const platform::GameImage& image, const platform::FunctionFact& fact, Fn& out) noexcept
{
    const auto live = platform::FindFunction(image, fact);
    out             = live ? reinterpret_cast<Fn>(*live) : nullptr;
    return live.has_value();
}

MemberSnapshot Snapshot(void* character, void* playerOneController, std::uint8_t playerType) noexcept
{
    MemberSnapshot member;
    member.character = character;
    member.leaderSlot = LeaderSlot(playerType);
    if (character == nullptr)
    {
        return member;
    }
    member.isEndCharacter = g_fn.isEndCharacter(character) != 0;
    member.isPlayerOnes   = character == g_fn.globalLeader() ||
                          character == ReadPointer(playerOneController, facts::kControllerPawn) ||
                          character == ReadPointer(playerOneController, facts::kControllerLeader);
    if (!member.isEndCharacter)
    {
        return member;  // the lease fields exist only on End characters
    }
    member.requested         = ReadPointer(character, facts::kCharacterRequestedController);
    member.effective         = ReadPointer(character, facts::kCharacterEffectiveController);
    member.retainedAi        = ReadPointer(character, facts::kCharacterRetainedAi);
    member.controller        = ReadPointer(character, facts::kPawnController);
    member.aiPawn            = ReadPointer(member.retainedAi, facts::kControllerPawn);
    member.playerControlled  = ReadField<std::uint8_t>(character, facts::kCharacterPlayerControlled);
    member.aiControllerValid = g_fn.validateAiController(g_fn.aiControllerOf(character)) != nullptr;
    member.battleReady       = member.aiControllerValid && BattleReady(character);
    return member;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    // Every check runs (and logs) even after one fails.
    bool found = Find(image, facts::kEnsureEndPlayerTrigger, g_fn.ensureTrigger);
    found      = Find(image, facts::kSwitchLeaderInternal, g_fn.switchLeader) && found;
    found      = Find(image, facts::kRosterCount, g_fn.rosterCount) && found;
    found      = Find(image, facts::kRosterPlayerType, g_fn.rosterType) && found;
    found      = Find(image, facts::kCharacterByPlayerType, g_fn.characterByType) && found;
    found      = Find(image, facts::kIsEndCharacter, g_fn.isEndCharacter) && found;
    found      = Find(image, facts::kAiControllerComponent, g_fn.aiControllerOf) && found;
    found      = Find(image, facts::kValidateAiController, g_fn.validateAiController) && found;
    found      = Find(image, facts::kResolvePartyLeader, g_fn.globalLeader) && found;
    found      = Find(image, facts::kIsBattleActive, g_fn.isBattleActive) && found;
    found      = Find(image, facts::kFieldLeaderType, g_fn.fieldLeaderType) && found;
    found      = Find(image, facts::kDefaultBattleLeaderType, g_fn.defaultBattleLeader) && found;
    found      = Find(image, facts::kIsBattleMember, g_fn.isBattleMember) && found;
    const auto optionHolder = image.Rebase(facts::kOptionHolderGlobal, sizeof(void*));
    const auto battleState  = image.Rebase(facts::kBattleStateGlobal, sizeof(void*));
    if (!optionHolder || !battleState)
    {
        return false;
    }
    g_optionHolder = reinterpret_cast<const void* const*>(*optionHolder);
    g_battleState  = reinterpret_cast<const void* const*>(*battleState);
    g_installed   = found && primary_controller::Installed();
    return g_installed;
}

bool Installed() noexcept
{
    return g_installed;
}

bool GameReady() noexcept
{
    return g_installed && *g_battleState != nullptr && *g_optionHolder != nullptr &&
           ReadPointer(*g_optionHolder, facts::kOptionHolderData) != nullptr;
}

void* Choose(void* playerOneController, bool log) noexcept
{
    if (!g_installed)
    {
        return nullptr;
    }
    const std::int32_t count = g_fn.rosterCount();
    for (std::int32_t ordinal = 0; ordinal < count && ordinal < facts::kMaxRosterEntries; ++ordinal)
    {
        const std::uint8_t type = g_fn.rosterType(ordinal);
        if (type >= facts::kNoPlayerType)
        {
            continue;
        }
        void*             character = g_fn.characterByType(facts::kFieldLane, type);
        const Eligibility verdict   = Assess(Snapshot(character, playerOneController, type));
        if (log)
        {
            platform::Log(L"party member {} (type {}): {}", ordinal, static_cast<unsigned>(type), Describe(verdict));
        }
        if (verdict == Eligibility::Eligible)
        {
            return character;
        }
    }
    return nullptr;
}

namespace {

// The party reads, guarded: they also run inside the game's status commit (player_two/party_events.h).

bool IsLeaderGuarded(const void* member) noexcept
{
    __try
    {
        return member == g_fn.globalLeader();
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool IsBattleMemberGuarded(std::uint8_t playerType) noexcept
{
    __try
    {
        return g_fn.isBattleMember(playerType) != 0;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return true;  // unread: not given up for it
    }
}

}  // namespace

const wchar_t* WhyNotInParty(const void* member) noexcept
{
    if (!g_installed || member == nullptr)
    {
        return nullptr;
    }
    if (IsLeaderGuarded(member))
    {
        return L"now the party leader, player 1's (a story change)";
    }
    const auto type = PlayerTypeOf(member);  // the battle-member list: the deployed party
    if (!type)
    {
        return L"no longer one of the deployed party";
    }
    if (!IsBattleMemberGuarded(*type))
    {
        return L"away from the battle party";
    }
    return nullptr;
}

bool HasBattleController(const void* member) noexcept
{
    if (!g_installed || member == nullptr)
    {
        return true;
    }
    __try
    {
        return g_fn.validateAiController(g_fn.aiControllerOf(member)) != nullptr;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return true;  // unread: not given up for it
    }
}

const wchar_t* WhyNotPlayable(const void* member) noexcept
{
    if (const wchar_t* why = WhyNotInParty(member))
    {
        return why;
    }
    // Downed or petrified stays player 2's, as player 1 stays on a downed leader; only a member
    // without its battle controller component is given up.
    return HasBattleController(member) ? nullptr : kWithoutBattleController;
}

namespace {

/// The roster walk, guarded: its callers include the command menu's tick. 0xFF if not found (or faulted).
std::uint8_t PlayerTypeGuarded(const void* character) noexcept
{
    __try
    {
        const std::int32_t count = g_fn.rosterCount();
        for (std::int32_t ordinal = 0; ordinal < count && ordinal < facts::kMaxRosterEntries; ++ordinal)
        {
            const std::uint8_t type = g_fn.rosterType(ordinal);
            if (type < facts::kNoPlayerType && g_fn.characterByType(facts::kFieldLane, type) == character)
            {
                return type;
            }
        }
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
    }
    return 0xFF;
}

}  // namespace

std::optional<std::uint8_t> PlayerTypeOf(const void* character) noexcept
{
    if (!g_installed || character == nullptr)
    {
        return std::nullopt;
    }
    const std::uint8_t type = PlayerTypeGuarded(character);
    return type < facts::kNoPlayerType ? std::optional<std::uint8_t>(type) : std::nullopt;
}

bool Take(void* playerTwoController, void* member) noexcept
{
    if (!g_installed || playerTwoController == nullptr || member == nullptr)
    {
        return false;
    }
    // The trigger is read from the controller afterwards: the function does not return it.
    if (!GuardedEnsureTrigger(playerTwoController) || ReadPointer(playerTwoController, facts::kControllerTrigger) == nullptr)
    {
        platform::Log(L"take refused: player 2's trigger actor could not be created");
        return false;
    }
    const primary_controller::Scope scope;
    if (!scope.Held())
    {
        platform::Log(L"take refused: the primary controller could not be scoped to player 2 ({})",
                      primary_controller::Describe(scope.Why()));
        return false;
    }
    if (!GuardedSwitchLeader(playerTwoController, member))
    {
        platform::Log(L"take failed: the switch raised an exception");
        return false;
    }
    return true;
}

bool GiveBack(void* playerTwoController) noexcept
{
    if (!g_installed || ReadPointer(playerTwoController, facts::kControllerTrigger) == nullptr)
    {
        return false;  // the switch writes through the trigger unconditionally
    }
    // The release writes nothing through the primary controller, so it runs even unscoped; the
    // scope keeps every read inside it on player 2's objects.
    const primary_controller::Scope scope;
    if (!scope.Held())
    {
        platform::Log(L"give-back runs unscoped: {}", primary_controller::Describe(scope.Why()));
    }
    if (!GuardedSwitchLeader(playerTwoController, nullptr))
    {
        platform::Log(L"give-back failed: the switch raised an exception");
        return false;
    }
    return true;
}

LeaseState ReadLease(const void* member, const void* playerTwoController) noexcept
{
    LeaseState state;
    if (member != nullptr)
    {
        state.memberEffective  = ReadPointer(member, facts::kCharacterEffectiveController);
        state.memberRetainedAi = ReadPointer(member, facts::kCharacterRetainedAi);
        state.aiPawn           = ReadPointer(state.memberRetainedAi, facts::kControllerPawn);
    }
    state.playerTwoPawn   = ReadPointer(playerTwoController, facts::kControllerPawn);
    state.playerTwoLeader = ReadPointer(playerTwoController, facts::kControllerLeader);
    return state;
}

PlayerOneBytes ReadPlayerOneBytes(const void* playerOneController) noexcept
{
    if (playerOneController == nullptr)
    {
        return {};
    }
    return {ReadField<std::uint8_t>(playerOneController, facts::kPrimaryControllerByte897),
            ReadField<std::uint8_t>(playerOneController, facts::kPrimaryControllerByte8AB)};
}

}  // namespace lss::player_two::party_member
