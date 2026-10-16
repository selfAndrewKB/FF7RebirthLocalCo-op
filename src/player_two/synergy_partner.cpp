#include "player_two/synergy_partner.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/game_thread_sites.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::player_two::synergy_partner {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-synergy-skills.md §6 and its S-SYNVERIFY section, RE-p2-synergy-abilities.md §6 (S-SYNSKILL, S-SYNABIL; every
// site read in both exes).
// S: the three calls of LinkPartner_Start_HandLeaderToAI(link manager, record, partner, initiator, 4 stack args) -> al
// (LinkMgr_Request's two command branches, LinkPartner_StartForType; its only callers; 1.005: the first crosses a
// block). T1: its hand-over test for modes 2/3 (mode 3 never reaches the AI request below), partner comp type ==
// GetPartyLeaderType() (both builds: crosses a block).
constexpr std::uintptr_t kStartCalls[] = {0x140F2C3B7, 0x1429A9B47, 0x143375806};
constexpr std::uintptr_t kStart        = 0x140F2CDE0;
constexpr std::uint8_t   kStartBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x4C,
                                          0x89, 0x48, 0x20, 0x48, 0x89, 0x48, 0x08, 0x55};
constexpr std::uintptr_t kStartLeaderTest = 0x140F2D03C;
// R: the three calls of LinkPartner_RequestAI_HandLeaderToAI(rcx unused, record, initiator, partner, partner comp, weak,
// weak): LinkMgr_TickMode2 (modes 2/3), LinkPartner_Mode1_StartOrRequestAI (the deferred mode-1 start, both from
// LinkMgr_Tick), and LinkPartner_Start_HandLeaderToAI (mode 1 at the request). They are its only callers.
constexpr std::uintptr_t kRequestCalls[] = {0x140ED637A, 0x14337BD83, 0x140F2D000};
constexpr std::uintptr_t kRequestAi      = 0x140F2AC8C;
constexpr std::uint8_t   kRequestAiBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57,
                                              0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// T2: its hand-over test, partner comp type == UEndPartyAPI_GetPartyLeaderType() (1.004: crosses a block).
constexpr std::uintptr_t kRequestLeaderTest = 0x140F2AD1E;
constexpr std::uintptr_t kLeaderType        = 0x1408070C4;
// E1: LinkMember_End_RepossessLeaderWithPrimaryPC(rcx unused, record, member, index): every link member's end (sync
// start for the partner, command end, record end, battle teardown). 1.005: the first instruction crosses a block, the
// redirect goes on the second. T3: its give-back test, EndCharacter_GetPlayerTypeFromComponent(member) ==
// GetPartyLeaderType(); P: the controller it gives back to, GetPrimaryEndPlayerController_FromPartyManager() (1.005:
// both cross a block).
constexpr std::uintptr_t kMemberEnd       = 0x140F2AB58;
constexpr std::uint8_t   kMemberEndBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::size_t    kFirstInstruction  = 5;  // mov [rsp+8], rbx
constexpr std::size_t    kSecondInstruction = 5;  // mov [rsp+10h], rsi
constexpr std::uintptr_t kEndLeaderTest   = 0x140F2AB8A;
constexpr std::uintptr_t kEndPrimary      = 0x140F2ABBA;
constexpr std::uintptr_t kPrimary         = 0x140A61D1C;
// The type the give-back test compares: the same function, on the remembered character.
constexpr std::uint8_t kComponentTypeBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD6, 0xF0, 0x6B, 0x01, 0x48, 0x2B,
                                                0xE0, 0x48, 0x8B, 0x89, 0x38, 0x06, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74};
constexpr platform::FunctionFact kComponentType{L"EndCharacter_GetPlayerTypeFromComponent", 0x1409EFE50,
                                                kComponentTypeBytes};
// Classic mode (deferred): the game's Classic hand-over keeps the controller (AI pause reason 5), so it is left alone.
constexpr std::uint8_t kClassicBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0xBB, 0x74, 0x01, 0x48, 0x2B,
                                          0xE0, 0x48, 0x8B, 0x05, 0xDC, 0x89, 0xA3, 0x08, 0x48, 0x8B, 0x88, 0x00};
constexpr platform::FunctionFact kClassic{L"Option_IsClassicMode_AutoPlayOn_NotHard", 0x140963368, kClassicBytes};

constexpr std::uint8_t kNoType       = 16;  // the game's "no character type"
constexpr std::size_t  kHeld         = 4;   // characters handed to their AI at once: at most the two humans' (spare)
constexpr auto         kLateAfter    = std::chrono::seconds(5);
constexpr auto         kStatusEvery  = std::chrono::seconds(10);
constexpr int          kMaxLines     = 400;

using StartFn       = std::uint64_t (*)(void* manager, void* record, void* partner, void* initiator, void* a5, void* a6,
                                        void* a7, void* a8);
using RequestFn     = std::uint64_t (*)(void* unused, void* record, void* initiator, void* partner, void* partnerComp,
                                        void* weakA, void* weakB);
using MemberEndFn   = std::uint64_t (*)(void* unused, void* record, void* member, std::uint64_t index);
using LeaderTypeFn  = std::uint8_t (*)();
using PrimaryFn     = void* (*)();
using TypeFn        = std::uint8_t (*)(const void* character);
using ClassicFn     = std::uint8_t (*)();

// Set once at install / placement.
std::array<platform::CallRedirect, std::size(kStartCalls)>   g_startCalls;
std::array<platform::CallRedirect, std::size(kRequestCalls)> g_requestCalls;
platform::CallRedirect                                       g_startTest, g_requestTest, g_endTest, g_endPrimary;
platform::EntryRedirect                                      g_memberEnd;
TypeFn                                                       g_typeOf  = nullptr;
ClassicFn                                                    g_classic = nullptr;
struct Sites
{
    std::array<std::uintptr_t, std::size(kStartCalls)> start{};
    std::uintptr_t startFn = 0, startTest = 0;
    std::array<std::uintptr_t, std::size(kRequestCalls)> request{};
    std::uintptr_t requestAi = 0, requestTest = 0, leaderType = 0;
    std::uintptr_t memberEnd = 0, endTest = 0, endPrimary = 0, primary = 0;
    std::span<const std::uint8_t> memberEndFirst;  // this build's first instruction there (empty: the second is used)
    bool           ok = false;
};
Sites g_sites;

// Game thread.
// A human's character the game handed to its AI for a synergy approach, and whose controller gets it back.
struct Held
{
    const void*       character = nullptr;
    std::uint64_t     weak      = 0;
    int               player    = 0;  // 1 or 2; 0 = free
    Clock::time_point since{};
    bool              warned    = false;
};
std::array<Held, kHeld> g_held{};
const void*             g_partner   = nullptr;  // the partner of the AI request being run
const void*             g_initiator = nullptr;
const void*             g_ending    = nullptr;  // the member of the member end being run
int                     g_endPlayer = 0;        // the player whose character that member end gives back (T3's answer)
constexpr int           kRefused    = -1;       // g_endPlayer: T3 refused a remembered character (no longer owned)
const void*             g_session   = nullptr;  // player 2's controller the remembered characters belong to

struct Counts
{
    std::uint32_t requests = 0, aiPartners = 0, gameHandOvers = 0, modHandOvers = 0, givenBack = 0, memberEnds = 0;
};
Counts            g_counts;
Clock::time_point g_statusAt{};
int               g_lines = 0;

bool Line() noexcept
{
    return platform::Enabled(platform::Level::Debug) && g_lines < kMaxLines && ++g_lines > 0;
}

bool Bound() noexcept
{
    return generation::OnGameThread() && generation::Active() && generation::PlayerTwoController() != nullptr;
}

/// 1 or 2 if that player's controller drives `character` now, else 0.
int PlayerHolding(const void* character) noexcept
{
    const void* driver = ReadPointer(character, facts::kCharacterEffectiveController);
    if (driver == nullptr)
    {
        return 0;
    }
    return driver == generation::PlayerTwoController() ? 2 : driver == generation::PlayerOneController() ? 1 : 0;
}

const wchar_t* Holder(const void* character) noexcept
{
    const void* driver = ReadPointer(character, facts::kCharacterEffectiveController);
    return driver == nullptr                           ? L"nobody"
           : driver == generation::PlayerOneController() ? L"player 1"
           : driver == generation::PlayerTwoController() ? L"player 2"
                                                         : L"its AI";
}

/// Why `player` no longer owns `character` (nullptr: it does). Player 2 owns only the session's member
/// (generation::Member(): cleared at once when player 2 gives it back, loses it or leaves, set again only by a take),
/// player 1 every other character; neither owns one the other human's controller drives. The weak pointer of a
/// remembered character proves only that it is the same object, not that its player still has it.
const wchar_t* LostBy(int player, const void* character) noexcept
{
    const bool playerTwos = character == generation::Member();
    if (player == 2 && !playerTwos)
    {
        return L"player 2 does not play it (a give-back, a lost member, a leave, or a take not finished)";
    }
    if (player == 1 && playerTwos)
    {
        return L"player 2 plays it now";
    }
    const int holding = PlayerHolding(character);
    if (holding != 0 && holding != player)
    {
        return holding == 1 ? L"player 1's controller drives it" : L"player 2's controller drives it";
    }
    return nullptr;
}

std::uint8_t TypeOf(const void* character) noexcept
{
    std::uint8_t type = kNoType;
    return g_typeOf != nullptr && platform::CallGuarded(g_typeOf, type, character) ? type : kNoType;
}

Held* Find(const void* character) noexcept
{
    for (Held& held : g_held)
    {
        if (held.player != 0 && held.character == character)
        {
            if (leader_scope::StillNames(held.weak, character))
            {
                return &held;
            }
            held = {};  // a destroyed character whose address came back
        }
    }
    return nullptr;
}

void Remember(const void* character, int player) noexcept
{
    if (Held* held = Find(character))
    {
        held->player = player;
        return;
    }
    Held* slot = &g_held[0];
    for (Held& held : g_held)
    {
        if (held.player == 0)
        {
            slot = &held;
            break;
        }
        if (held.since < slot->since)
        {
            slot = &held;
        }
    }
    if (slot->player != 0 && Line())
    {
        platform::LogDebug(L"synergy partner: forgot {} (player {}) to remember another", NameOf(slot->character),
                           slot->player);
    }
    *slot = {character, leader_scope::WeakOf(const_cast<void*>(character)), player, Clock::now(), false};
}

void ForgetAll() noexcept
{
    g_held = {};
}

// ---------------------------------------------------------------------------------------------
// The hand-over: the AI request names its partner; its leader test answers a human's partner
// ---------------------------------------------------------------------------------------------

std::uint64_t Start(std::size_t site, void* manager, void* record, void* partner, void* initiator, void* a5, void* a6,
                    void* a7, void* a8) noexcept
{
    const auto original = reinterpret_cast<StartFn>(g_startCalls[site].Original());
    if (!Bound())
    {
        return original(manager, record, partner, initiator, a5, a6, a7, a8);
    }
    const void* savedPartner   = g_partner;
    const void* savedInitiator = g_initiator;
    g_partner                  = partner;
    g_initiator                = initiator;
    const auto result          = original(manager, record, partner, initiator, a5, a6, a7, a8);  // al: started
    g_partner                  = savedPartner;
    g_initiator                = savedInitiator;
    return result;
}

std::uint64_t StartCommand(void* m, void* r, void* p, void* i, void* a5, void* a6, void* a7, void* a8) noexcept
{
    return Start(0, m, r, p, i, a5, a6, a7, a8);
}

std::uint64_t StartMounted(void* m, void* r, void* p, void* i, void* a5, void* a6, void* a7, void* a8) noexcept
{
    return Start(1, m, r, p, i, a5, a6, a7, a8);
}

std::uint64_t StartForType(void* m, void* r, void* p, void* i, void* a5, void* a6, void* a7, void* a8) noexcept
{
    return Start(2, m, r, p, i, a5, a6, a7, a8);
}

std::uint64_t Request(std::size_t site, void* unused, void* record, void* initiator, void* partner, void* partnerComp,
                      void* weakA, void* weakB) noexcept
{
    const auto original = reinterpret_cast<RequestFn>(g_requestCalls[site].Original());
    if (!Bound())
    {
        return original(unused, record, initiator, partner, partnerComp, weakA, weakB);
    }
    ++g_counts.requests;
    const void* savedPartner   = g_partner;
    const void* savedInitiator = g_initiator;
    g_partner                  = partner;
    g_initiator                = initiator;
    const auto result          = original(unused, record, initiator, partner, partnerComp, weakA, weakB);
    g_partner                  = savedPartner;
    g_initiator                = savedInitiator;
    return result;
}

std::uint64_t RequestTick(void* u, void* r, void* i, void* p, void* c, void* a, void* b) noexcept
{
    return Request(0, u, r, i, p, c, a, b);
}

std::uint64_t RequestDeferred(void* u, void* r, void* i, void* p, void* c, void* a, void* b) noexcept
{
    return Request(1, u, r, i, p, c, a, b);
}

std::uint64_t RequestAtStart(void* u, void* r, void* i, void* p, void* c, void* a, void* b) noexcept
{
    return Request(2, u, r, i, p, c, a, b);
}

bool Classic() noexcept
{
    std::uint8_t on = 0;
    return g_classic != nullptr && platform::CallGuarded(g_classic, on) && on != 0;
}

/// T1 / T2: "is the partner the player's character?", answered yes for whichever human holds it.
std::uint8_t PartnerLeaderType(std::uint8_t original) noexcept
{
    const void* partner = g_partner;
    if (partner == nullptr || !Bound() || Classic())
    {
        return original;
    }
    const int player = PlayerHolding(partner);
    if (player == 0)
    {
        ++g_counts.aiPartners;
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {}'s partner {} is driven by {}: the game's own approach",
                               NameOf(g_initiator), NameOf(partner), Holder(partner));
        }
        return original;
    }
    if (const wchar_t* why = LostBy(player, partner))
    {
        // Player 2's controller still drives a member the session gave up (its give-back is under way): the game's
        // own answer, as before player 2 had it.
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {}'s partner {} is driven by player {} but not owned ({}): the game's "
                               L"own answer ({})",
                               NameOf(g_initiator), NameOf(partner), player, why, original);
        }
        return original;
    }
    const std::uint8_t type = TypeOf(partner);
    if (type >= kNoType)
    {
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {}'s partner {} (player {}'s) has no readable type: the game's own "
                               L"answer ({})",
                               NameOf(g_initiator), NameOf(partner), player, original);
        }
        return original;
    }
    Remember(partner, player);
    (type == original ? g_counts.gameHandOvers : g_counts.modHandOvers) += 1;
    if (Line())
    {
        platform::LogDebug(L"synergy partner: {}'s partner {} is player {}'s: handed to its AI for the approach ({}; "
                           L"the leader type read {}, the partner's {})",
                           NameOf(g_initiator), NameOf(partner), player,
                           type == original ? L"the game's own answer" : L"answered as the player's character",
                           original, type);
    }
    return type;
}

std::uint8_t StartLeaderType() noexcept
{
    return PartnerLeaderType(reinterpret_cast<LeaderTypeFn>(g_startTest.Original())());
}

std::uint8_t RequestLeaderType() noexcept
{
    return PartnerLeaderType(reinterpret_cast<LeaderTypeFn>(g_requestTest.Original())());
}

// ---------------------------------------------------------------------------------------------
// The give-back: the member end names its member; its test and controller answer that member's player
// ---------------------------------------------------------------------------------------------

std::uint64_t MemberEnd(void* unused, void* record, void* member, std::uint64_t index) noexcept
{
    const auto original = reinterpret_cast<MemberEndFn>(g_memberEnd.Original());
    if (member == nullptr || !Bound())
    {
        return original(unused, record, member, index);
    }
    ++g_counts.memberEnds;
    const void* savedEnding = g_ending;
    const int   savedPlayer = g_endPlayer;
    g_ending                = member;
    g_endPlayer             = 0;
    const auto result       = original(unused, record, member, index);
    if (g_endPlayer > 0)
    {
        if (Held* held = Find(member))
        {
            *held = {};  // given back (or Classic's flag cleared): the next end of it is the game's own
        }
    }
    g_ending    = savedEnding;
    g_endPlayer = savedPlayer;
    return result;
}

/// T3: "is the member the player's character?", answered yes for a character handed over for its player.
std::uint8_t EndLeaderType() noexcept
{
    const std::uint8_t original = reinterpret_cast<LeaderTypeFn>(g_endTest.Original())();
    const void*        member   = g_ending;
    if (member == nullptr || !Bound())
    {
        return original;
    }
    Held* held = Find(member);
    if (held == nullptr)
    {
        return original;
    }
    // Only while its player still owns it: player 2 may have given it back during the approach (a party change, the
    // game making it player 1's leader); re-possessing it then would undo that. Tested here as well as in Tick: the
    // give-back and the member end can run in one frame. The game's own test then decides (player 1's new leader goes
    // to player 1; a character nobody holds stays with its AI, as a non-leader partner in single player).
    if (const wchar_t* why = LostBy(held->player, member))
    {
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {} not given back to player {}: {} (driven by {}); the game's own "
                               L"give-back test",
                               NameOf(member), held->player, why, Holder(member));
        }
        *held       = {};
        g_endPlayer = kRefused;  // an earlier answer in this member end no longer stands
        return original;
    }
    // Only while its player has no other character: a player who switched to another member during the approach keeps
    // that one, and the old one stays with its AI, as in single player (the game's test then fails for it as well).
    void* const       controller = held->player == 2 ? generation::PlayerTwoController() : generation::PlayerOneController();
    const void* const pawn       = ReadPointer(controller, facts::kControllerPawn);
    if (controller == nullptr || (pawn != nullptr && pawn != member))
    {
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {} not given back: player {} plays {} now", NameOf(member),
                               held->player, NameOf(pawn));
        }
        *held       = {};
        g_endPlayer = kRefused;
        return original;
    }
    const std::uint8_t type = TypeOf(member);
    if (type >= kNoType)
    {
        return original;
    }
    g_endPlayer = held->player;
    return type;
}

/// P: the controller the member is given back to: its own player's.
void* EndPrimary() noexcept
{
    void* const original = reinterpret_cast<PrimaryFn>(g_endPrimary.Original())();
    const void* member   = g_ending;
    if (member == nullptr || !Bound())
    {
        return original;
    }
    // A remembered character goes back to its player; player 2's member to player 2 in any case (a member end that
    // ran with player 2's member as the leader type, inside player 2's command window, without the primary controller
    // swapped, would otherwise give it to player 1).
    int player = g_endPlayer;
    if (player == kRefused)
    {
        // A refused give-back never lands on player 2's controller, even when the primary controller reads it (a member
        // end inside a player-2 scope): the game's single-player target, player 1's, or the game's own.
        void* const playerOne = generation::PlayerOneController();
        if (original == generation::PlayerTwoController() && playerOne != nullptr && member != generation::Member())
        {
            if (Line())
            {
                platform::LogDebug(L"synergy partner: {} (refused) would go to player 2's controller as the primary; "
                                   L"player 1's instead",
                                   NameOf(member));
            }
            return playerOne;
        }
        return original;
    }
    if (player == 0 && member == generation::Member())
    {
        player = 2;
    }
    if (player == 0)
    {
        return original;
    }
    void* const controller = player == 2 ? generation::PlayerTwoController() : generation::PlayerOneController();
    if (controller == nullptr)
    {
        return original;
    }
    ++g_counts.givenBack;
    if (Line())
    {
        const Held* held = Find(member);
        const auto  ms   = held != nullptr
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - held->since).count()
                               : -1;
        platform::LogDebug(L"synergy partner: {} given back to player {}'s controller (driven by {} now{}; the game's "
                           L"primary controller {})",
                           NameOf(member), player, Holder(member),
                           ms >= 0 ? std::format(L", {} ms after the hand-over", ms) : std::wstring(),
                           controller == original ? L"is the same" : L"was the other player's");
    }
    return controller;
}

// ---------------------------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------------------------

void RestoreAll() noexcept
{
    for (auto* call : {&g_startCalls[0], &g_startCalls[1], &g_startCalls[2], &g_requestCalls[0], &g_requestCalls[1],
                       &g_requestCalls[2], &g_startTest, &g_requestTest, &g_endTest, &g_endPrimary})
    {
        if (call->IsInstalled())
        {
            call->Restore();
        }
    }
    if (g_memberEnd.IsInstalled())
    {
        g_memberEnd.Restore();
    }
}

/// Game thread, from the session's tick (outside every link call): every site, or none.
void Place() noexcept
{
    // The answers first, then what names their character: until the names are in, the answers pass through.
    bool ok = g_startTest.InstallFromOwnThread(g_sites.startTest, g_sites.leaderType,
                                               reinterpret_cast<std::uintptr_t>(&StartLeaderType)) &&
              g_requestTest.InstallFromOwnThread(g_sites.requestTest, g_sites.leaderType,
                                                 reinterpret_cast<std::uintptr_t>(&RequestLeaderType)) &&
              g_endTest.InstallFromOwnThread(g_sites.endTest, g_sites.leaderType,
                                             reinterpret_cast<std::uintptr_t>(&EndLeaderType)) &&
              g_endPrimary.InstallFromOwnThread(g_sites.endPrimary, g_sites.primary,
                                                reinterpret_cast<std::uintptr_t>(&EndPrimary));
    const std::array<std::uintptr_t, std::size(kRequestCalls)> requests{
        reinterpret_cast<std::uintptr_t>(&RequestTick), reinterpret_cast<std::uintptr_t>(&RequestDeferred),
        reinterpret_cast<std::uintptr_t>(&RequestAtStart)};
    for (std::size_t i = 0; ok && i < requests.size(); ++i)
    {
        ok = g_requestCalls[i].InstallFromOwnThread(g_sites.request[i], g_sites.requestAi, requests[i]);
    }
    const std::array<std::uintptr_t, std::size(kStartCalls)> starts{reinterpret_cast<std::uintptr_t>(&StartCommand),
                                                                    reinterpret_cast<std::uintptr_t>(&StartMounted),
                                                                    reinterpret_cast<std::uintptr_t>(&StartForType)};
    for (std::size_t i = 0; ok && i < starts.size(); ++i)
    {
        ok = g_startCalls[i].InstallFromOwnThread(g_sites.start[i], g_sites.startFn, starts[i]);
    }
    if (ok)
    {
        const std::span<const std::uint8_t> bytes(kMemberEndBytes);
        const auto replacement = reinterpret_cast<std::uintptr_t>(&MemberEnd);
        ok = g_memberEnd.Install(g_sites.memberEnd, g_sites.memberEndFirst, replacement) ||
             g_memberEnd.InstallAtSecondInstruction(g_sites.memberEnd, bytes.first(kFirstInstruction),
                                                    bytes.subspan(kFirstInstruction, kSecondInstruction), replacement);
    }
    if (!ok)
    {
        RestoreAll();
    }
    platform::Log(L"synergy partner: {}",
                  ok ? L"a synergy partner held by player 1 or player 2 is handed to its AI for the approach and given "
                       L"back to its own player (skills and abilities)"
                     : L"UNCHANGED (a site did not install): synergies keep the game's single-player hand-over");
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto at = [&image](std::uintptr_t address, std::size_t length = 1) {
        const auto live = image.Rebase(address, length);
        return live ? *live : 0;
    };
    bool ok = true;
    for (std::size_t i = 0; i < std::size(kRequestCalls); ++i)
    {
        g_sites.request[i] = at(kRequestCalls[i], 5);
        ok                 = ok && g_sites.request[i] != 0;
    }
    for (std::size_t i = 0; i < std::size(kStartCalls); ++i)
    {
        g_sites.start[i] = at(kStartCalls[i], 5);
        ok               = ok && g_sites.start[i] != 0;
    }
    g_sites.startFn     = at(kStart);
    g_sites.startTest   = at(kStartLeaderTest, 5);
    g_sites.requestAi   = at(kRequestAi);
    g_sites.requestTest = at(kRequestLeaderTest, 5);
    g_sites.leaderType  = at(kLeaderType);
    g_sites.memberEnd   = at(kMemberEnd, std::size(kMemberEndBytes));
    g_sites.endTest     = at(kEndLeaderTest, 5);
    g_sites.endPrimary  = at(kEndPrimary, 5);
    g_sites.primary     = at(kPrimary);
    const auto typeOf   = platform::FindFunction(image, kComponentType);
    g_typeOf            = typeOf ? reinterpret_cast<TypeFn>(*typeOf) : nullptr;
    const auto classic  = platform::FindFunction(image, kClassic);
    g_classic           = classic ? reinterpret_cast<ClassicFn>(*classic) : nullptr;
    g_sites.ok = ok && g_sites.startFn != 0 && g_sites.startTest != 0 && g_classic != nullptr &&
                 image.BytesMatch(kStart, kStartBytes) && g_sites.requestAi != 0 && g_sites.requestTest != 0 && g_sites.leaderType != 0 &&
                 g_sites.memberEnd != 0 && g_sites.endTest != 0 && g_sites.endPrimary != 0 && g_sites.primary != 0 &&
                 g_typeOf != nullptr && image.BytesMatch(kRequestAi, kRequestAiBytes) &&
                 image.BytesMatch(kMemberEnd, kMemberEndBytes);
    if (g_sites.ok)
    {
        g_sites.memberEndFirst = image.EntryInstruction(kMemberEnd, std::span(kMemberEndBytes).first(kFirstInstruction));
    }
    if (!g_sites.ok)
    {
        platform::Log(L"synergy partner: the sites are not what the research read; synergies keep the game's "
                      L"single-player hand-over");
    }
    else if (!game_thread_sites::Queue(&Place))
    {
        platform::Log(L"synergy partner: the game-thread placement queue is full; synergies keep the game's "
                      L"single-player hand-over");
    }
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    // The remembered characters belong to one session of player 2: forgotten when it ends.
    const void* session = generation::Active() ? generation::PlayerTwoController() : nullptr;
    if (session != g_session)
    {
        bool any = false;
        for (const Held& held : g_held)
        {
            any = any || held.player != 0;
        }
        if (any)
        {
            platform::Log(L"synergy partner: player 2's session changed with a character still handed to its AI; "
                          L"forgotten (the game's own give-back applies from now on)");
        }
        ForgetAll();
        g_session = session;
    }
    const auto now = Clock::now();
    for (Held& held : g_held)
    {
        if (held.player == 0)
        {
            continue;
        }
        if (!leader_scope::StillNames(held.weak, held.character))
        {
            held = {};
            continue;
        }
        if (const wchar_t* why = LostBy(held.player, held.character))
        {
            if (Line())
            {
                platform::LogDebug(L"synergy partner: {} forgotten (handed to its AI for player {}): {}; the game's own "
                                   L"give-back applies to it",
                                   NameOf(held.character), held.player, why);
            }
            held = {};
            continue;
        }
        if (!held.warned && now - held.since >= kLateAfter && PlayerHolding(held.character) != held.player)
        {
            held.warned = true;
            platform::Log(L"synergy partner: {} (player {}'s) is still driven by {} {} s after its synergy hand-over; "
                          L"the game has not ended its link yet",
                          NameOf(held.character), held.player, Holder(held.character),
                          std::chrono::duration_cast<std::chrono::seconds>(kLateAfter).count());
        }
    }
    if (now - g_statusAt < kStatusEvery)
    {
        return;
    }
    g_statusAt     = now;
    const Counts c = g_counts;
    g_counts       = {};
    if (c.requests + c.memberEnds == 0)
    {
        return;
    }
    platform::LogDebug(L"synergy partner (10 s): AI requests {}, AI partners {}, humans' partners handed over by the "
                       L"game's own answer {} / by the answered test {}; member ends {}, given back to their own "
                       L"player {}",
                       c.requests, c.aiPartners, c.gameHandOvers, c.modHandOvers, c.memberEnds, c.givenBack);
}

}  // namespace lss::player_two::synergy_partner
