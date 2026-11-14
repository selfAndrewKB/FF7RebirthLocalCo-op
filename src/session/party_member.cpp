#include "session/party_member.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/primary_controller.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace lss::session::party_member {

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
using SwitchAiFn        = void (*)(void* character, std::uint8_t toPlayer, void* controller);
using ActorTypeFn       = std::uint8_t (*)(void* actorInterface);
using SetActorTypeFn    = void (*)(void* actorInterface, std::uint8_t type);

struct Functions
{
    EnsureTriggerFn   ensureTrigger       = nullptr;
    SwitchLeaderFn    switchLeader        = nullptr;
    SwitchAiFn        switchAi            = nullptr;  // optional: without it a script's hand is the give-back
    RosterCountFn     rosterCount         = nullptr;
    RosterTypeFn      rosterType          = nullptr;
    CharacterByTypeFn characterByType     = nullptr;
    ObjectTestFn      isEndCharacter      = nullptr;
    ObjectToObjectFn  aiControllerOf      = nullptr;
    ObjectToObjectFn  validateAiController = nullptr;
    GlobalObjectFn    globalLeader        = nullptr;
    ObjectTestFn      isBattleActive      = nullptr;  // player 1's battle switch's member test T
    TypeFn            fieldLeaderType     = nullptr;
    TypeTestFn        isBattleMember      = nullptr;
};
Functions         g_fn;  // set once at install
bool              g_installed    = false;
const void* const* g_optionHolder = nullptr;  // facts/party.h: kOptionHolderGlobal
const void* const* g_battleState  = nullptr;  // facts/party.h: kBattleStateGlobal

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

/// Player 1's battle switch's own member test T, the game's function (research/RE-p2-party-switch.md): the
/// member is live, has its battle controller component, and is neither dead nor petrified. The first
/// rule for a seat's player is to be bound by it the way player 1 is.
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

/// The field leader's character (null if none is listed). Plain reads; the caller holds the SEH guard.
void* FieldLeaderCharacter() noexcept
{
    const std::uint8_t type = g_fn.fieldLeaderType();
    return type < facts::kNoPlayerType ? g_fn.characterByType(facts::kFieldLane, type) : nullptr;
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
    if (character == nullptr)
    {
        return member;
    }
    member.isEndCharacter = g_fn.isEndCharacter(character) != 0;
    member.isPlayerOnes   = character == g_fn.globalLeader() ||
                          character == players::actor_fields::Pawn(playerOneController) ||
                          character == players::actor_fields::LeaderCharacter(playerOneController);
    if (!member.isEndCharacter)
    {
        return member;  // the lease fields exist only on End characters
    }
    member.requested         = players::actor_fields::RequestedController(character);
    member.effective         = players::actor_fields::EffectiveController(character);
    member.retainedAi        = players::actor_fields::RetainedAi(character);
    member.controller        = players::actor_fields::Controller(character);
    member.aiPawn            = players::actor_fields::Pawn(member.retainedAi);
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
    found      = Find(image, facts::kIsBattleMember, g_fn.isBattleMember) && found;
    found      = facts::InBattleKnown() && found;  // the game's in-battle test, found at startup (in_battle.h)
    if (!Find(image, facts::kSwitchAiControlAsync, g_fn.switchAi))  // optional (HandToAi)
    {
        g_fn.switchAi = nullptr;
        platform::Log(L"party member: the game's hand to the AI is not available (optional): a script that takes a "
                      L"player's member gives it back and the player waits for it");
    }
    const auto optionHolder = image.Rebase(facts::kOptionHolderGlobal, sizeof(void*));
    const auto battleState  = image.Rebase(facts::kBattleStateGlobal, sizeof(void*));
    if (!optionHolder || !battleState)
    {
        return false;
    }
    g_optionHolder = reinterpret_cast<const void* const*>(*optionHolder);
    g_battleState  = reinterpret_cast<const void* const*>(*battleState);
    g_installed   = found && scopes::primary_controller::Installed();
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

AiSnapshot ReadAi(const void* member) noexcept
{
    AiSnapshot ai;
    const void* brain = players::actor_fields::ActiveBrain(players::actor_fields::RetainedAi(member));
    const void* state = players::actor_fields::BrainState(brain);
    if (state == nullptr)
    {
        return ai;
    }
    ai.brainIndex = ReadField<std::uint8_t>(brain, facts::kBrainIndex);
    ai.state      = ReadField<std::uint8_t>(state, facts::kAiStateType);
    ai.read       = true;
    return ai;
}

namespace {

/// "2nd" for player 2's slot of the party order, "3rd" for player 3's (the log's name for it).
const wchar_t* SlotName(int seat) noexcept
{
    return seat == 1 ? L"2nd" : seat == 2 ? L"3rd" : L"player's";
}

/// What ChooseSlot reads from the game (plain values, so the read can run inside an SEH guard).
struct SlotRead
{
    std::int32_t   count       = 0;
    std::uint8_t   fieldLeader = 0xFF;
    std::int32_t   anchor      = 0;
    std::int32_t   ordinal     = -1;  // -1: no slot (fewer than two members, or no row for the seat)
    std::uint8_t   type        = 0xFF;
    bool           standsIn    = false;  // the stand-in mark's row was taken
    bool           fieldLeaderInstead = false;  // the seat's row is player 1's: the field leader's row
    void*          character   = nullptr;
    MemberSnapshot member{};
    AiSnapshot     ai{};
    bool           linkBusy = false;
    LinkRecordView link{};
};

// The battle system's link manager and its records (research/RE-p2-synergy-skills.md; the same
// offsets in both builds). The battle system is kBattleStateGlobal's object.
constexpr std::uint32_t kLinkManager      = 0x2F0;
constexpr std::uint32_t kLinkRecords      = 0x00;  // TArray of kLinkRecordSize-byte records: data, count
constexpr std::uint32_t kLinkRecordCount  = 0x08;
constexpr std::uint32_t kRecordAbility    = 0x10;
constexpr std::uint32_t kAbilityKind      = 0x8B;
constexpr std::uint32_t kAbilityMode      = 0x8F;
constexpr std::uint32_t kRecordTypes      = 0x29;
constexpr std::uint32_t kRecordActions    = 0x2C;
constexpr std::uint32_t kRecordPartner    = 0x39;
constexpr std::uint32_t kRecordNoLink     = 0x3A;
constexpr std::uint32_t kRecordWindow     = 0x50;
constexpr std::uint32_t kRecordState      = 0x56;
constexpr std::uint32_t kRecordStarted    = 0x100;
constexpr std::uint32_t kRecordEnded      = 0x103;
constexpr std::int32_t  kMaxLinkRecords   = 32;  // a sanity bound: a few records live at once

}  // namespace

LinkRecordView ReadLinkRecord(const void* record) noexcept
{
    LinkRecordView view;
    if (record == nullptr)
    {
        return view;
    }
    for (std::uint32_t member = 0; member < std::size(view.types); ++member)
    {
        view.types[member]   = ReadField<std::uint8_t>(record, kRecordTypes + member);
        view.state[member]   = ReadField<std::uint8_t>(record, kRecordState + member);
        view.started[member] = ReadField<std::uint8_t>(record, kRecordStarted + member);
        view.ended[member]   = ReadField<std::uint8_t>(record, kRecordEnded + member);
        view.actions[member] = ReadField<std::uint32_t>(record, kRecordActions + 4 * member);
    }
    view.partner        = ReadField<std::uint8_t>(record, kRecordPartner);
    view.noLink         = ReadField<std::uint8_t>(record, kRecordNoLink);
    view.window         = ReadField<std::uint8_t>(record, kRecordWindow);
    const void* ability = ReadPointer(record, kRecordAbility);
    if (ability != nullptr)
    {
        view.mode = ReadField<std::uint8_t>(ability, kAbilityMode);
        view.kind = ReadField<std::uint8_t>(ability, kAbilityKind);
    }
    return view;
}

namespace {

/// The first of the game's link records that `accept` takes. Plain reads; the caller holds the SEH guard.
template <class Accept>
bool FindLink(Accept accept, LinkRecordView& out) noexcept
{
    const void* manager = ReadPointer(*g_battleState, kLinkManager);
    const auto* records = static_cast<const std::byte*>(ReadPointer(manager, kLinkRecords));
    if (records == nullptr)
    {
        return false;
    }
    const std::int32_t count = std::min(ReadField<std::int32_t>(manager, kLinkRecordCount), kMaxLinkRecords);
    for (std::int32_t index = 0; index < count; ++index)
    {
        const LinkRecordView view = ReadLinkRecord(records + static_cast<std::size_t>(index) * kLinkRecordSize);
        if (accept(view))
        {
            out = view;
            return true;
        }
    }
    return false;
}

/// The game's link record in which `playerType` is a partner it has not given back yet (party_lease.h
/// LinkPartnerBusy).
bool FindBusyLink(std::uint8_t playerType, LinkRecordView& out) noexcept
{
    return FindLink([playerType](const LinkRecordView& view) { return LinkPartnerBusy(view, playerType); }, out);
}

bool FindPartnerLinkGuarded(std::uint8_t playerType, LinkRecordView& out) noexcept
{
    __try
    {
        return facts::InBattle() &&
               FindLink(
            [playerType](const LinkRecordView& view) { return IsPartnerOf(view, playerType); },
            out);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void ReadSlot(int seat, void* playerOneController, std::uint8_t standIn, SlotRead& out) noexcept
{
    std::uint8_t types[facts::kMaxRosterEntries]{};
    out.count = std::min(g_fn.rosterCount(), facts::kMaxRosterEntries);
    for (std::int32_t ordinal = 0; ordinal < out.count; ++ordinal)
    {
        types[ordinal] = g_fn.rosterType(ordinal);
    }
    if (out.count < 2)
    {
        return;
    }
    out.fieldLeader = g_fn.fieldLeaderType();
    out.anchor      = LeaderOrdinal(types, out.count, out.fieldLeader);
    const auto snapshot = [&](std::int32_t at, void*& character) noexcept {
        character = g_fn.characterByType(facts::kFieldLane, types[at]);
        return Snapshot(character, playerOneController, types[at]);
    };
    // The stand-in mark's row first (party_lease.h StandInFor): player 1's former character, while it is deployed
    // and not player 1's again.
    void*              character = nullptr;
    MemberSnapshot     member{};
    std::int32_t       ordinal = OrdinalOfType(types, out.count, standIn);
    if (ordinal >= 0)
    {
        member = snapshot(ordinal, character);
        if (member.isPlayerOnes)
        {
            ordinal = -1;
        }
        out.standsIn = ordinal >= 0;
    }
    // Else the seat's own row; player 1 on it means the field leader's row instead (RowOrFieldLeader).
    if (ordinal < 0)
    {
        const std::int32_t row = SeatSlotOrdinal(out.anchor, out.count, seat, [&](std::int32_t at) noexcept {
            return types[at] >= facts::kNoPlayerType;
        });
        if (row < 0)
        {
            return;
        }
        member  = snapshot(row, character);
        // The field leader's row only when its type is read (none during a load) and the anchor is its row.
        ordinal = RowOrFieldLeader(row, member.isPlayerOnes, out.anchor,
                                   out.fieldLeader < facts::kNoPlayerType && types[out.anchor] == out.fieldLeader);
        if (ordinal != row)
        {
            member                 = snapshot(ordinal, character);
            out.fieldLeaderInstead = true;
        }
    }
    const std::uint8_t type = types[ordinal];
    out.ordinal             = ordinal;
    out.type                = type;
    out.character           = character;
    out.member              = member;
    out.ai                  = ReadAi(character);
    // In battle only: synergy records come only from battle commands, and the battle teardown keeps a
    // record whose started member never ended, which must not hold a seat after it.
    out.linkBusy = facts::InBattle() && FindBusyLink(type, out.link);
}

/// The party reads, guarded: during a reload the party's objects may be torn down under them. False
/// (no slot this time) if they faulted.
bool ReadSlotGuarded(int seat, void* playerOneController, std::uint8_t standIn, SlotRead& out) noexcept
{
    __try
    {
        ReadSlot(seat, playerOneController, standIn, out);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool IsFieldLeaderGuarded(const void* character) noexcept
{
    __try
    {
        return FieldLeaderCharacter() == character;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool LeaderHomeGuarded() noexcept
{
    __try
    {
        void* const leader = g_fn.globalLeader();
        return leader != nullptr && leader == FieldLeaderCharacter();
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

}  // namespace

bool IsFieldLeader(const void* character) noexcept
{
    return g_installed && character != nullptr && IsFieldLeaderGuarded(character);
}

bool LeaderHome() noexcept
{
    return g_installed && LeaderHomeGuarded();
}

bool FindPartnerLink(std::uint8_t playerType, LinkRecordView& out) noexcept
{
    return g_installed && *g_battleState != nullptr && FindPartnerLinkGuarded(playerType, out);
}

SlotChoice ChooseSlot(int seat, void* playerOneController, std::uint8_t standIn, bool log) noexcept
{
    SlotChoice choice;
    SlotRead   read;
    if (!g_installed)
    {
        return choice;
    }
    if (!ReadSlotGuarded(seat, playerOneController, standIn, read))
    {
        if (log)
        {
            platform::Log(L"party slot: the party could not be read (a fault, as in a reload); waiting");
        }
        choice.wait = SlotWait::Unread;
        return choice;
    }
    if (read.ordinal < 0)
    {
        if (log)
        {
            platform::Log(L"party slot: the party has {} member(s), no {} slot for {}", read.count, SlotName(seat),
                          players::SeatName(seat));
        }
        return choice;
    }
    choice.type               = read.type;
    choice.ordinal            = read.ordinal;
    choice.standsIn           = read.standsIn;
    choice.fieldLeaderInstead = read.fieldLeaderInstead;
    choice.verdict            = Assess(read.member);
    choice.ai      = read.ai;
    const AiReadiness readiness = AssessAi(choice.ai);
    choice.wait                 = SlotWaitOf(choice.verdict, readiness, read.linkBusy);
    if (choice.wait == SlotWait::LinkPartner)
    {
        choice.link = read.link;
    }
    else if (choice.wait == SlotWait::None)
    {
        choice.member = read.character;
    }
    if (log)
    {
        platform::Log(L"party slot: field leader type {} at ordinal {}; the {} slot is ordinal {} (type {}){}: {}; its "
                      L"AI brain {} state {}: {}{}",
                      static_cast<unsigned>(read.fieldLeader), read.anchor, SlotName(seat), read.ordinal,
                      static_cast<unsigned>(read.type),
                      read.standsIn             ? L" (player 1's former character, until the game's leader goes home)"
                      : read.fieldLeaderInstead ? L" (player 1 plays this player's row: the field leader's character instead)"
                                                : L"",
                      Describe(choice.verdict), static_cast<unsigned>(choice.ai.brainIndex),
                      static_cast<unsigned>(choice.ai.state), Describe(readiness),
                      choice.wait == SlotWait::LinkPartner ? L"; a synergy partner the game has not given back yet" : L"");
    }
    return choice;
}

namespace {

// The party reads, guarded: they also run inside the game's status commit (session/party_events.h).

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
    // Downed or petrified stays the seat's, as player 1 stays on a downed leader; only a member
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

bool Take(int seat, void* controller, void* member) noexcept
{
    if (!g_installed || controller == nullptr || member == nullptr)
    {
        return false;
    }
    // The trigger is read from the controller afterwards: the function does not return it.
    if (!GuardedEnsureTrigger(controller) || players::actor_fields::Trigger(controller) == nullptr)
    {
        platform::Log(L"take refused: {}'s trigger actor could not be created", players::SeatName(seat));
        return false;
    }
    const scopes::primary_controller::Scope scope(seat);
    if (!scope.Held())
    {
        platform::Log(L"take refused: the primary controller could not be scoped to {} ({})", players::SeatName(seat),
                      scopes::primary_controller::Describe(scope.Why()));
        return false;
    }
    if (!GuardedSwitchLeader(controller, member))
    {
        platform::Log(L"take failed: the switch raised an exception");
        return false;
    }
    return true;
}

bool GiveBack(int seat, void* controller) noexcept
{
    if (!g_installed || players::actor_fields::Trigger(controller) == nullptr)
    {
        return false;  // the switch writes through the trigger unconditionally
    }
    // The release writes nothing through the primary controller, so it runs even unscoped; the
    // scope keeps every read inside it on the seat's objects.
    const scopes::primary_controller::Scope scope(seat);
    if (!scope.Held())
    {
        platform::Log(L"give-back runs unscoped: {}", scopes::primary_controller::Describe(scope.Why()));
    }
    if (!GuardedSwitchLeader(controller, nullptr))
    {
        platform::Log(L"give-back failed: the switch raised an exception");
        return false;
    }
    return true;
}

bool HandToAi(void* member) noexcept
{
    // The game's own hand of a character to its AI (its events hand player 1's leader so): the AI must be valid, or the
    // call unpossesses the player's controller and possesses nothing (research/RE-held-member-scripts.md).
    if (!g_installed || g_fn.switchAi == nullptr || member == nullptr ||
        g_fn.validateAiController(g_fn.aiControllerOf(member)) == nullptr)
    {
        return false;
    }
    if (!platform::CallGuarded(g_fn.switchAi, member, std::uint8_t{1}, static_cast<void*>(nullptr)))
    {
        platform::Log(L"hand to the AI failed: the game's call raised an exception");
        return false;
    }
    return true;
}

bool ReturnTo(int seat, void* member, void* controller) noexcept
{
    if (!g_installed || g_fn.switchAi == nullptr || member == nullptr || controller == nullptr)
    {
        return false;
    }
    // In the seat's primary scope: the possession's leader-action reset reads "the" primary controller's component.
    const scopes::primary_controller::Scope scope(seat);
    if (!scope.Held())
    {
        platform::LogDebug(L"the return to {} waits: the primary controller could not be scoped ({})",
                           players::SeatName(seat), scopes::primary_controller::Describe(scope.Why()));
        return false;
    }
    if (!platform::CallGuarded(g_fn.switchAi, member, std::uint8_t{1}, controller))
    {
        platform::LogDebug(L"the return to {} failed: the game's call raised an exception", players::SeatName(seat));
        return false;
    }
    return true;
}

bool IsGameLeader(const void* character) noexcept
{
    return g_installed && character != nullptr && IsLeaderGuarded(character);
}

namespace {

/// `character`'s actor interface, and the function its table holds at `slot` (null when unreadable).
template <class Fn>
Fn ActorInterfaceFunction(void* character, std::uint32_t slot, void*& actorInterface) noexcept
{
    actorInterface = static_cast<std::byte*>(character) + facts::kCharacterActorInterface;
    return reinterpret_cast<Fn>(platform::ReadPointer(platform::ReadPointer(actorInterface, 0), slot));
}

}  // namespace

std::optional<std::uint8_t> ActorType(void* character) noexcept
{
    if (character == nullptr)
    {
        return std::nullopt;
    }
    void*             actorInterface = nullptr;
    const ActorTypeFn getter = ActorInterfaceFunction<ActorTypeFn>(character, facts::kActorTypeGetSlot, actorInterface);
    std::uint8_t      type   = 0;
    if (getter == nullptr || !platform::CallGuarded(getter, type, actorInterface))
    {
        return std::nullopt;
    }
    return type;
}

bool SetActorType(void* character, std::uint8_t type) noexcept
{
    if (character == nullptr)
    {
        return false;
    }
    void*                actorInterface = nullptr;
    const SetActorTypeFn setter = ActorInterfaceFunction<SetActorTypeFn>(character, facts::kActorTypeSetSlot, actorInterface);
    return setter != nullptr && platform::CallGuarded(setter, actorInterface, type);
}

LeaseState ReadLease(const void* member, const void* seatController) noexcept
{
    LeaseState state;
    if (member != nullptr)
    {
        state.memberEffective  = players::actor_fields::EffectiveController(member);
        state.memberRetainedAi = players::actor_fields::RetainedAi(member);
        state.aiPawn           = players::actor_fields::Pawn(state.memberRetainedAi);
    }
    state.seatPawn   = players::actor_fields::Pawn(seatController);
    state.seatLeader = players::actor_fields::LeaderCharacter(seatController);
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

}  // namespace lss::session::party_member
