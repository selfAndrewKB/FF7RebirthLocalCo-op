#include "battle/synergy_partner.h"

#include "battle/synergy_rules.h"
#include "facts/controller.h"
#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/game_thread_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "scopes/leader_scope.h"
#include "session/party_member.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <utility>

namespace lss::battle::synergy_partner {

namespace {

using namespace synergy_rules;

// RE-p2-synergy-skills.md §6 and its S-SYNVERIFY section, RE-p2-synergy-abilities.md §6 (S-SYNSKILL, S-SYNABIL; every
// site read in both exes).
// S: the three calls of LinkPartner_Start_HandLeaderToAI(link manager, record, partner, initiator, 4 stack args) -> al
// (LinkMgr_Request's two command branches, LinkPartner_StartForType; its only callers; 1.005: the first crosses a
// block). T1: its hand-over test for modes 2/3 (mode 3 never reaches the AI request below), partner comp type ==
// GetPartyLeaderType() (both builds: crosses a block).
constexpr std::uintptr_t kStartCalls[] = {0x140F2C3B7, 0x1429A9B47, 0x143375806};
constexpr std::uint8_t   kStartBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x4C,
                                          0x89, 0x48, 0x20, 0x48, 0x89, 0x48, 0x08, 0x55};
constexpr platform::FunctionFact kStart{L"LinkPartner_Start_HandLeaderToAI", 0x140F2CDE0, kStartBytes};
constexpr std::uintptr_t kStartLeaderTest = 0x140F2D03C;
// R: the three calls of LinkPartner_RequestAI_HandLeaderToAI(rcx unused, record, initiator, partner, partner comp, weak,
// weak): LinkMgr_TickMode2 (modes 2/3), LinkPartner_Mode1_StartOrRequestAI (the deferred mode-1 start, both from
// LinkMgr_Tick), and LinkPartner_Start_HandLeaderToAI (mode 1 at the request). They are its only callers.
constexpr std::uintptr_t kRequestCalls[] = {0x140ED637A, 0x14337BD83, 0x140F2D000};
constexpr std::uint8_t   kRequestAiBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57,
                                              0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::FunctionFact kRequestAi{L"LinkPartner_RequestAI_HandLeaderToAI", 0x140F2AC8C, kRequestAiBytes};
// T2: its hand-over test, partner comp type == UEndPartyAPI_GetPartyLeaderType() (1.004: crosses a block).
constexpr std::uintptr_t kRequestLeaderTest = 0x140F2AD1E;
using facts::kGetPartyLeaderType;
// E1: LinkMember_End_RepossessLeaderWithPrimaryPC(rcx unused, record, member, index): every link member's end (sync
// start for the partner, command end, record end, battle teardown). 1.005: the first instruction crosses a block, the
// redirect goes on the second. T3: its give-back test, EndCharacter_GetPlayerTypeFromComponent(member) ==
// GetPartyLeaderType(); P: the controller it gives back to, GetPrimaryEndPlayerController_FromPartyManager() (1.005:
// both cross a block).
constexpr std::uint8_t   kMemberEndBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kMemberEnd{L"LinkMember_End_RepossessLeaderWithPrimaryPC", 0x140F2AB58,
                                            kMemberEndBytes};
constexpr std::size_t    kFirstInstruction  = 5;  // mov [rsp+8], rbx
constexpr std::size_t    kSecondInstruction = 5;  // mov [rsp+10h], rsi
constexpr std::uintptr_t kEndLeaderTest   = 0x140F2AB8A;
constexpr std::uintptr_t kEndPrimary      = 0x140F2ABBA;
using facts::kGetPrimaryEndPlayerController;
// The type the give-back test compares: the same function, on the remembered character.
using facts::kPlayerTypeFromComponent;
// Classic mode (deferred): the game's Classic hand-over keeps the controller (AI pause reason 5), so it is left alone.
constexpr std::uint8_t kClassicBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0xBB, 0x74, 0x01, 0x48, 0x2B,
                                          0xE0, 0x48, 0x8B, 0x05, 0xDC, 0x89, 0xA3, 0x08, 0x48, 0x8B, 0x88, 0x00};
constexpr platform::FunctionFact kClassic{L"Option_IsClassicMode_AutoPlayOn_NotHard", 0x140963368, kClassicBytes};
// G: the give-back LinkMember_End calls, AEndCharacter::SwitchAIControl_Async(member, 1, controller); on the game thread
// it possesses at once (S-SYNANYTHREAD Q1; also chocobo_ride, wheelie_ride).
using facts::kSwitchAiControlAsync;

// W: a parry-window skill (S-SYNCOUNTER). LinkPartner_Start's 7th argument is the initiator's BattleAbility handle
// (this vtable; +8 its row). The row's AnimationSelectType_Array (+0x90 data, +0x98 count; u8 kinds; a data word with
// its low bit set is an offset from the field) holds kind 26, the window that ends the link when the parry fails, in
// Counterfire's eight rows only (ChainCounter_PC0000_*). The game asks for such a skill's partner only when the counter
// animation opens the record's combination window (rec+0x50), through the AI request (T2); nothing before that reads
// the partner's AI.
constexpr std::uintptr_t kAbilityHandleVtable = 0x145793088;
constexpr std::uint32_t  kHandleRow           = 0x08;
constexpr std::uint32_t  kRowKinds            = 0x90;
constexpr std::uint32_t  kRowKindCount        = 0x98;
constexpr std::uint8_t   kParryWindow         = 26;
constexpr std::int32_t   kMaxKinds            = 16;

constexpr std::size_t  kNotes       = 8;   // member ends off the game thread between two ticks (a few per synergy)
constexpr auto         kLateAfter   = std::chrono::seconds(5);
constexpr auto         kGoneAfter   = std::chrono::seconds(1);  // a record gone, or a give-back not landed
constexpr auto         kStatusEvery = std::chrono::seconds(10);

using StartFn       = std::uint64_t (*)(void* manager, void* record, void* partner, void* initiator, void* a5, void* a6,
                                        void* a7, void* a8);
using RequestFn     = std::uint64_t (*)(void* unused, void* record, void* initiator, void* partner, void* partnerComp,
                                        void* weakA, void* weakB);
using MemberEndFn   = std::uint64_t (*)(void* unused, void* record, void* member, std::uint64_t index);
using LeaderTypeFn  = std::uint8_t (*)();
using PrimaryFn     = void* (*)();
using TypeFn        = std::uint8_t (*)(const void* character);
using ClassicFn     = std::uint8_t (*)();
using SwitchAiFn    = void (*)(void* character, std::uint8_t toPlayer, void* controller);

// Set once at install / placement.
std::array<platform::CallRedirect, std::size(kStartCalls)>   g_startCalls;
std::array<platform::CallRedirect, std::size(kRequestCalls)> g_requestCalls;
platform::CallRedirect                                       g_startTest, g_requestTest, g_endTest, g_endPrimary;
platform::EntryRedirect                                      g_memberEnd;
TypeFn                                                       g_typeOf  = nullptr;
ClassicFn                                                    g_classic = nullptr;
SwitchAiFn                                                   g_switchAi = nullptr;
struct Sites
{
    std::array<std::uintptr_t, std::size(kStartCalls)> start{};
    std::uintptr_t startFn = 0, startTest = 0;
    std::array<std::uintptr_t, std::size(kRequestCalls)> request{};
    std::uintptr_t requestAi = 0, requestTest = 0, leaderType = 0;
    std::uintptr_t memberEnd = 0, endTest = 0, endPrimary = 0, primary = 0;
    std::uintptr_t abilityHandle = 0;  // the BattleAbility handle's vtable (0: no parry-window test; W)
    std::span<const std::uint8_t> memberEndFirst;  // this build's first instruction there (empty: the second is used)
    bool           ok = false;
};
Sites             g_sites;
std::atomic<bool> g_on{false};       // every site placed; false from Remove on: the wrappers pass through (any thread)
std::atomic<bool> g_pending{false};  // a placement queued and not yet run (Remove clears it: it then does nothing)

// Game thread. The characters handed to their AI for an approach (synergy_rules.h Held).
std::array<Held, kHeld> g_held{};

const void*    g_partner   = nullptr;  // the partner of the AI request being run
const void*    g_initiator = nullptr;
const void*    g_record    = nullptr;  // the link record of the start or AI request being run
bool           g_window    = false;    // the start being run is a parry-window skill's (W): T1 keeps a human's partner
const wchar_t* g_via       = L"";      // which call asks (for the log)
// Each seat's controller the remembered characters belong to (by seat; index 0 unused).
std::array<const void*, players::kMaxPlayers> g_sessions{};

/// The call being run, for the tests inside it (T1 and T2 read the partner, the record and the window); restored on
/// the way out, since the calls nest (a mode-1 start runs the AI request).
class CallContext
{
public:
    CallContext(const void* partner, const void* initiator, const void* record, bool window,
                const wchar_t* via) noexcept
        : partner_(g_partner), initiator_(g_initiator), record_(g_record), window_(g_window), via_(g_via)
    {
        g_partner   = partner;
        g_initiator = initiator;
        g_record    = record;
        g_window    = window;
        g_via       = via;
    }
    ~CallContext()
    {
        g_partner   = partner_;
        g_initiator = initiator_;
        g_record    = record_;
        g_window    = window_;
        g_via       = via_;
    }
    CallContext(const CallContext&)            = delete;
    CallContext& operator=(const CallContext&) = delete;

private:
    const void*    partner_;
    const void*    initiator_;
    const void*    record_;
    bool           window_;
    const wchar_t* via_;
};

// The member end being run on this thread (T3 answers only on the game thread; off it T3 fails for a seat's member
// and P never names a seat's controller).
thread_local const void* t_ending    = nullptr;  // the member of the member end being run
thread_local int         t_endPlayer = 0;        // the player whose character that member end gives back (T3's answer)
thread_local bool        t_gameGave  = false;    // off the game thread: the game's own test passed and P ran (it queued
                                                 // its own give-back)
thread_local bool        t_heldBack  = false;    // off the game thread: T3 failed for a seat's member (no give-back)

/// One member end's scope, restored on the way out (member ends nest: a record end runs several).
class EndScope
{
public:
    explicit EndScope(const void* member) noexcept
        : ending_(t_ending), player_(t_endPlayer), gameGave_(t_gameGave), heldBack_(t_heldBack)
    {
        t_ending    = member;
        t_endPlayer = 0;
        t_gameGave  = false;
        t_heldBack  = false;
    }
    ~EndScope()
    {
        t_ending    = ending_;
        t_endPlayer = player_;
        t_gameGave  = gameGave_;
        t_heldBack  = heldBack_;
    }
    EndScope(const EndScope&)            = delete;
    EndScope& operator=(const EndScope&) = delete;

private:
    const void* ending_;
    int         player_;
    bool        gameGave_;
    bool        heldBack_;
};

// A member end that ran off the game thread (any thread, under g_lock), for the game thread's tick (TakeNotes).
struct Note
{
    const void*       member   = nullptr;
    Clock::time_point at{};             // when it ran: a note older than the character's hand-over is not its end
    std::uint32_t     thread   = 0;
    bool              gameGave = false;  // the game's own test passed: it queued its own give-back
    bool              heldBack = false;  // T3 failed for a seat's member: no give-back by the game
    std::uint8_t      state    = 0;      // the record at the member's index, before the member end
    std::uint8_t      started  = 0;
    std::uint8_t      ended    = 0;
};
SRWLOCK                  g_lock = SRWLOCK_INIT;  // never throws, unlike std::mutex in noexcept code; held for a copy
std::array<Note, kNotes> g_notes{};
std::size_t              g_noteCount = 0;
std::uint32_t            g_notesLost = 0;

class Locked
{
public:
    Locked() noexcept { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
    Locked(const Locked&)            = delete;
    Locked& operator=(const Locked&) = delete;
};

// Game thread.
struct Counts
{
    std::uint32_t requests = 0, aiPartners = 0, gameHandOvers = 0, modHandOvers = 0, givenBack = 0, memberEnds = 0,
                  offThread = 0, kept = 0;
};
Counts            g_counts;
platform::Every g_status{kStatusEvery};

bool Line() noexcept
{
    return platform::Enabled(platform::Level::Debug);
}

/// A seat's session is running: a built seat active with its controller (any thread: the generation's
/// atomics).
bool Session() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && players::generation::ControllerOf(seat) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/// The player (1 player 1, seat + 1 a seat's player: Held.player's numbering) whose controller `controller` is, else
/// 0. The seats' are compared first.
int PlayerOfController(const void* controller) noexcept
{
    if (controller == nullptr)
    {
        return 0;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (controller == players::generation::ControllerOf(seat))
        {
            return seat + 1;
        }
    }
    return controller == players::generation::PlayerOneController() ? 1 : 0;
}

/// The built seat whose member `character` is, or 0.
int SeatPlaying(const void* character) noexcept
{
    const int seat = players::generation::SeatOfMember(character);
    return players::SeatBuilt(seat) ? seat : 0;
}

/// The seat a player number names (Held.player: 1 player 1, so seat 0).
constexpr int SeatOfPlayer(int player) noexcept
{
    return player - 1;
}

/// The hand-over's wrappers and the game-thread member end act only here.
bool Bound() noexcept
{
    return players::generation::OnGameThread() && Session();
}

/// The player (1 player 1, seat + 1 a seat's player) whose controller drives `character` now, else 0.
int PlayerHolding(const void* character) noexcept
{
    return PlayerOfController(ReadPointer(character, facts::kCharacterEffectiveController));
}

const wchar_t* Holder(const void* character) noexcept
{
    const void* driver = ReadPointer(character, facts::kCharacterEffectiveController);
    const int   player = PlayerOfController(driver);
    return driver == nullptr ? L"nobody" : player != 0 ? players::SeatName(SeatOfPlayer(player)) : L"its AI";
}

/// Why `player` no longer owns `character` (nullptr: it does). A seat's player owns only its seat's member
/// (players::generation::MemberOf: cleared at once when the seat gives it back, loses it or leaves, set again only by a take),
/// player 1 every character no seat plays; none owns one another human's controller drives. The weak pointer of a
/// remembered character proves only that it is the same object, not that its player still has it.
const wchar_t* LostBy(int player, const void* character) noexcept
{
    static constexpr const wchar_t* kNotPlaying[players::kMaxPlayers] = {
        L"", L"player 2 does not play it (a give-back, a lost member, a leave, or a take not finished)",
        L"player 3 does not play it (a give-back, a lost member, a leave, or a take not finished)"};
    static constexpr const wchar_t* kPlaysNow[players::kMaxPlayers] = {L"", L"player 2 plays it now",
                                                                        L"player 3 plays it now"};
    static constexpr const wchar_t* kDrives[players::kMaxPlayers] = {
        L"player 1's controller drives it", L"player 2's controller drives it", L"player 3's controller drives it"};
    static_assert(players::kMaxPlayers == 3, "LostBy's texts name every player");
    const int memberSeat = SeatPlaying(character);
    if (player >= 2 && player <= players::kMaxPlayers && memberSeat != SeatOfPlayer(player))
    {
        return kNotPlaying[SeatOfPlayer(player)];
    }
    if (player == 1 && memberSeat != 0)
    {
        return kPlaysNow[memberSeat];
    }
    const int holding = PlayerHolding(character);
    if (holding != 0 && holding != player)
    {
        return kDrives[SeatOfPlayer(holding)];
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
            if (scopes::leader_scope::StillNames(held.weak, character))
            {
                return &held;
            }
            held = {};  // a destroyed character whose address came back
        }
    }
    return nullptr;
}

/// Remembers `character` for `player` from this hand-over on; a character already remembered starts afresh (a new
/// approach: nothing of the last one counts).
void Remember(const void* character, int player, std::uint8_t type) noexcept
{
    Held* slot = Find(character);
    if (slot == nullptr)
    {
        slot = &SlotFor(g_held, character);
    }
    if (slot->player != 0 && slot->character != character && Line())
    {
        platform::LogDebug(L"synergy partner: forgot {} (player {}) to remember another", players::NameOf(slot->character),
                           slot->player);
    }
    *slot           = {};
    slot->character = character;
    slot->weak      = scopes::leader_scope::WeakOf(const_cast<void*>(character));
    slot->player    = player;
    slot->type      = type;
    slot->since     = Clock::now();
}

void ForgetAll() noexcept
{
    g_held = {};
}

/// W: true if `handle` is a BattleAbility handle whose row has the parry-window kind. Guarded reads; game thread.
bool HasParryWindow(const void* handle) noexcept
{
    if (handle == nullptr || g_sites.abilityHandle == 0)
    {
        return false;
    }
    std::uintptr_t vtable = 0;
    std::uintptr_t row    = 0;
    if (!platform::CopyGuarded(&vtable, handle, sizeof(vtable)))
    {
        return false;
    }
    if (vtable != g_sites.abilityHandle)
    {
        static bool logged = false;  // a start whose 7th argument is not the handle: the research expects none
        if (!logged && vtable != 0)
        {
            logged = true;
            platform::LogDebug(L"synergy partner: a start's ability argument has another vtable (0x{:X}); no parry-window "
                               L"test for it",
                               vtable);
        }
        return false;
    }
    if (!platform::CopyGuarded(&row, static_cast<const std::byte*>(handle) + kHandleRow, sizeof(row)) || row == 0)
    {
        return false;
    }
    std::int64_t data  = 0;
    std::int32_t count = 0;
    if (!platform::CopyGuarded(&data, reinterpret_cast<const void*>(row + kRowKinds), sizeof(data)) ||
        !platform::CopyGuarded(&count, reinterpret_cast<const void*>(row + kRowKindCount), sizeof(count)))
    {
        return false;
    }
    count = std::clamp(count, 0, kMaxKinds);
    // As BattleAbility_GetAnimSelectType0: an odd word is an offset from the field (arithmetic shift), else a pointer.
    const std::uintptr_t kinds = (data & 1) != 0 ? row + kRowKinds + static_cast<std::uintptr_t>(data >> 1)
                                                 : static_cast<std::uintptr_t>(data);
    std::array<std::uint8_t, kMaxKinds> bytes{};
    if (count == 0 || kinds == 0 ||
        !platform::CopyGuarded(bytes.data(), reinterpret_cast<const void*>(kinds), static_cast<std::size_t>(count)))
    {
        return false;
    }
    return std::find(bytes.begin(), bytes.begin() + count, kParryWindow) != bytes.begin() + count;
}

/// The controller of `player` (1 player 1, seat + 1 a seat's player), or null.
void* ControllerOf(int player) noexcept
{
    return player >= 1 && player <= players::kSeatsBuilt + 1 ? players::generation::ControllerOf(SeatOfPlayer(player)) : nullptr;
}

/// The pawn `controller` plays instead of `character` (null if it plays nothing, or that character).
const void* PlaysInstead(const void* controller, const void* character) noexcept
{
    const void* pawn = ReadPointer(controller, facts::kControllerPawn);
    return pawn != character ? pawn : nullptr;
}

/// Any thread: a member end off the game thread, for the next tick.
void AddNote(const Note& note) noexcept
{
    Locked lock;
    if (g_noteCount < g_notes.size())
    {
        g_notes[g_noteCount++] = note;
    }
    else
    {
        ++g_notesLost;
    }
}

/// The members' action ids (they name the skill offline), the ability's mode and category, and the no-link flag, as
/// one log phrase. Game thread.
std::wstring RecordText(const session::LinkRecordView& record) noexcept
{
    try
    {
        const std::size_t partner = std::min<std::size_t>(record.partner, std::size(record.state) - 1);
        return std::format(L"serials {}/{}/{} (mode {}, category {}, no-link flag {}, window {}, partner state {})",
                           record.actions[0], record.actions[1], record.actions[2], record.mode, record.kind,
                           record.noLink, record.window, record.state[partner]);
    }
    catch (...)
    {
        return {};
    }
}

/// The record at a member's index, as one log phrase. Game thread.
std::wstring MemberText(std::uint8_t state, std::uint8_t started, std::uint8_t ended) noexcept
{
    try
    {
        return std::format(L"state {}, started {}, ended {}", state, started, ended);
    }
    catch (...)
    {
        return {};
    }
}

// ---------------------------------------------------------------------------------------------
// The hand-over: the AI request names its partner; its leader test answers a human's partner
// ---------------------------------------------------------------------------------------------

/// The start call of site `Site` (S): the partner, the record and the window named for T1 inside it.
template <std::size_t Site>
std::uint64_t Start(void* manager, void* record, void* partner, void* initiator, void* a5, void* a6, void* a7,
                    void* a8) noexcept
{
    const auto original = reinterpret_cast<StartFn>(g_startCalls[Site].Original());
    if (!g_on.load(std::memory_order_relaxed) || !Bound())
    {
        return original(manager, record, partner, initiator, a5, a6, a7, a8);
    }
    static constexpr const wchar_t* kVia[] = {L"the start (command)", L"the start (Cait Sith mounted)",
                                              L"the start (by type)"};
    // The window is read before the call: it overwrites the handle on its way out.
    const CallContext context(partner, initiator, record, HasParryWindow(a7), kVia[Site]);
    return original(manager, record, partner, initiator, a5, a6, a7, a8);  // al: started
}

/// The AI request call of site `Site` (R): the partner and the record named for T2 inside it.
template <std::size_t Site>
std::uint64_t Request(void* unused, void* record, void* initiator, void* partner, void* partnerComp, void* weakA,
                      void* weakB) noexcept
{
    const auto original = reinterpret_cast<RequestFn>(g_requestCalls[Site].Original());
    if (!g_on.load(std::memory_order_relaxed) || !Bound())
    {
        return original(unused, record, initiator, partner, partnerComp, weakA, weakB);
    }
    ++g_counts.requests;
    static constexpr const wchar_t* kVia[] = {L"the window's AI request (modes 2/3)", L"the deferred AI request (mode 1)",
                                              L"the AI request at the start (mode 1)"};
    // The game asks for the partner now: T2 hands a human's partner over (no parry window here).
    const CallContext context(partner, initiator, record, false, kVia[Site]);
    return original(unused, record, initiator, partner, partnerComp, weakA, weakB);
}

bool Classic() noexcept
{
    std::uint8_t on = 0;
    return g_classic != nullptr && platform::CallGuarded(g_classic, on) && on != 0;
}

/// T1 / T2: "is the partner the player's character?", answered yes for whichever human holds it.
std::uint8_t PartnerLeaderType(std::uint8_t original) noexcept
{
    const void*    partner = g_partner;
    const bool     known   = partner != nullptr && Bound() && !Classic();
    const int      player  = known ? PlayerHolding(partner) : 0;
    const wchar_t* why     = nullptr;
    const auto     decision = DecideHandOver(
        original, known, player, [&] { why = LostBy(player, partner); return why != nullptr; },
        [&] { return TypeOf(partner); }, g_window);
    const std::uint8_t type = decision.type;
    switch (decision.what)
    {
        case HandOver::GamesOwn: return original;
        case HandOver::AiPartner:
            ++g_counts.aiPartners;
            if (Line())
            {
                platform::LogDebug(L"synergy partner: {}'s partner {} is driven by {}: the game's own approach ({}; the "
                                   L"synergy {})",
                                   players::NameOf(g_initiator), players::NameOf(partner), Holder(partner), g_via,
                                   RecordText(session::party_member::ReadLinkRecord(g_record)));
            }
            return original;
        case HandOver::Lost:
            // A seat's controller still drives a member the session gave up (its give-back is under way): the game's
            // own answer, as before that seat had it.
            if (Line())
            {
                platform::LogDebug(L"synergy partner: {}'s partner {} is driven by player {} but not owned ({}): the game's "
                                   L"own answer ({})",
                                   players::NameOf(g_initiator), players::NameOf(partner), player, why, original);
            }
            return original;
        case HandOver::NoType:
            if (Line())
            {
                platform::LogDebug(L"synergy partner: {}'s partner {} (player {}'s) has no readable type: the game's own "
                                   L"answer ({})",
                                   players::NameOf(g_initiator), players::NameOf(partner), player, original);
            }
            return original;
        case HandOver::Kept:
        case HandOver::HandedOver: break;
    }
    if (decision.what == HandOver::Kept)
    {
        // A parry-window skill's stance (W): its partner is asked for only when the counter animation opens the window,
        // by the AI request (T2), which then hands it over; a failed parry never asks. Until then its player keeps it.
        // A game's own yes (the leader type is the partner's) waits for T2 as well. The counter repeats the start (by
        // type) just before its AI request: the same stance, still kept. (The stance's outcome was followed for the log
        // until the counter's keep-until-counter was confirmed live, v0.74.7; the follow-up is retired.)
        ++g_counts.kept;
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {}'s partner {} is player {}'s: kept by its player through the parry "
                               L"window; the game's own request at the counter hands it over ({}; the synergy {})",
                               players::NameOf(g_initiator), players::NameOf(partner), player, g_via,
                               RecordText(session::party_member::ReadLinkRecord(g_record)));
        }
        return decision.answer;
    }
    Remember(partner, player, type);
    (type == original ? g_counts.gameHandOvers : g_counts.modHandOvers) += 1;
    if (Line())
    {
        platform::LogDebug(L"synergy partner: {}'s partner {} is player {}'s: handed to its AI for the approach ({}; "
                           L"the leader type read {}, the partner's {}; {}); the synergy {}",
                           players::NameOf(g_initiator), players::NameOf(partner), player,
                           type == original ? L"the game's own answer" : L"answered as the player's character",
                           original, type, g_via, RecordText(session::party_member::ReadLinkRecord(g_record)));
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
//
// The game runs LinkMember_End on the game thread (the sync start, the record end, the battle teardown) and from a
// member's own command end, wherever that member's command executor finishes. A mode-2 synergy skill's partner plays
// its own half and is given back only there, and the executor's finish is written for worker threads
// (RE-p2-synergy-skills.md S-SYNSTUCK, S-SYNSTUCK2). On the game thread T3 and P answer for a remembered character.
// Off it T3 fails for a seat's member and P never names a seat's controller, and the member end is noted: the next
// tick gives a remembered character back on the game thread with the game's own call, SwitchAIControl_Async(member, 1,
// its player's controller), after checking that its player still owns it and plays nothing else. The game's own
// off-thread give-back would queue that possession with raw pointers and check nothing when it runs (S-SYNANYTHREAD
// Q1), so it could outlive the player's ownership or controller (a leave, a party change in between); the tick's call
// runs at once and only on what is true then.

/// Any thread: the member end and the record at the member's index, before the member end.
Note NoteOf(const void* record, const void* member, std::uint64_t index) noexcept
{
    Note note;
    note.member = member;
    note.at     = Clock::now();
    note.thread = GetCurrentThreadId();
    if (index < std::size(session::LinkRecordView{}.state))
    {
        const session::LinkRecordView view = session::party_member::ReadLinkRecord(record);
        note.state                = view.state[index];
        note.started              = view.started[index];
        note.ended                = view.ended[index];
    }
    return note;
}

std::uint64_t MemberEnd(void* unused, void* record, void* member, std::uint64_t index) noexcept
{
    const auto original = reinterpret_cast<MemberEndFn>(g_memberEnd.Original());
    if (!g_on.load(std::memory_order_relaxed) || member == nullptr || !Session())
    {
        return original(unused, record, member, index);
    }
    if (!players::generation::OnGameThread())
    {
        Note       note = NoteOf(record, member, index);
        EndScope   scope(member);
        const auto result = original(unused, record, member, index);  // T3 / P: nothing for a seat here
        note.gameGave     = t_gameGave;
        note.heldBack     = t_heldBack;
        AddNote(note);  // the tick gives a remembered character back
        return result;
    }
    ++g_counts.memberEnds;
    EndScope   scope(member);
    const auto result = original(unused, record, member, index);
    if (Held* held = Find(member))
    {
        if (t_endPlayer > 0)
        {
            *held = {};  // given back (or Classic's flag cleared): the next end of it is the game's own
        }
        else
        {
            held->endSeen = EndSeen::GameThread;
        }
    }
    return result;
}

/// T3: "is the member the player's character?", answered yes for a character handed over for its player.
std::uint8_t EndLeaderType() noexcept
{
    const std::uint8_t original = reinterpret_cast<LeaderTypeFn>(g_endTest.Original())();
    const void*        member   = t_ending;
    if (member == nullptr || !Session())
    {
        return original;
    }
    if (!players::generation::OnGameThread())
    {
        // A seat's member is never given back from a worker: the game would queue its possession by the seat's
        // controller with raw pointers that nothing checks when it runs. The game's compare fails; the tick gives a
        // remembered member back on the game thread. Atomics only.
        if (SeatPlaying(member) != 0)
        {
            t_heldBack = true;
            return kNoMatch;
        }
        return original;
    }
    Held* held = Find(member);
    if (held == nullptr)
    {
        return original;
    }
    // Only while its player still owns it: a seat may have given it back during the approach (a party change, the
    // game making it player 1's leader); re-possessing it then would undo that. Tested here as well as in Tick: the
    // give-back and the member end can run in one frame. The game's own test then decides (player 1's new leader goes
    // to player 1; a character nobody holds stays with its AI, as a non-leader partner in single player).
    if (const wchar_t* why = LostBy(held->player, member))
    {
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {} not given back to player {}: {} (driven by {}); the game's own "
                               L"give-back test",
                               players::NameOf(member), held->player, why, Holder(member));
        }
        *held       = {};
        t_endPlayer = kRefused;  // an earlier answer in this member end no longer stands
        return original;
    }
    // Only while its player has no other character: a player who switched to another member during the approach keeps
    // that one, and the old one stays with its AI, as in single player (the game's test then fails for it as well).
    void* const       controller = ControllerOf(held->player);
    const void* const other      = PlaysInstead(controller, member);
    if (controller == nullptr || other != nullptr)
    {
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {} not given back: player {} plays {} now", players::NameOf(member),
                               held->player, players::NameOf(other));
        }
        *held       = {};
        t_endPlayer = kRefused;
        return original;
    }
    const std::uint8_t type = TypeOf(member);
    if (type >= kNoType)
    {
        return original;
    }
    t_endPlayer = held->player;
    return type;
}

/// P: the controller the member is given back to: its own player's.
void* EndPrimary() noexcept
{
    void* const original = reinterpret_cast<PrimaryFn>(g_endPrimary.Original())();
    const void* member   = t_ending;
    if (member == nullptr || !Session())
    {
        return original;
    }
    const bool offThread = !players::generation::OnGameThread();
    if (offThread)
    {
        // The game's own give-back of a character not a seat's (T3 passed by its own test: the leader type; T3 fails
        // for a seat's member here). The primary controller it reads may catch a seat's scope of the game thread's;
        // its queued possession is never by a seat's controller: player 1's instead, or none (its AI keeps it).
        // Atomics only. That queued possession is the game's single-player path for player 1's character; the tick does
        // not give the character back again.
        t_gameGave = true;
    }
    // A remembered character goes back to its player; a seat's member to that seat in any case (a member end that
    // ran with the seat's member as the leader type, inside the seat's command window, without the primary controller
    // swapped, would otherwise give it to player 1). A refused give-back never lands on a seat's controller, even
    // when the primary controller reads it (a member end inside a seat's scope): the game's single-player target,
    // player 1's, or the game's own.
    const int  originalPlayer = PlayerOfController(original);
    const int  originalSeat   = originalPlayer >= 2 ? SeatOfPlayer(originalPlayer) : 0;
    const auto decision       = DecideGiveBack(offThread, t_endPlayer, originalSeat,
                                               players::generation::PlayerOneController() != nullptr, SeatPlaying(member));
    switch (decision.to)
    {
        case GiveBackTo::Original: return original;
        case GiveBackTo::PlayerOne:
            if (!offThread && Line())
            {
                platform::LogDebug(L"synergy partner: {} (refused) would go to {}'s controller as the primary; "
                                   L"player 1's instead",
                                   players::NameOf(member), players::SeatName(originalSeat));
            }
            return players::generation::PlayerOneController();
        case GiveBackTo::Player: break;
    }
    const int   player     = decision.player;
    void* const controller = ControllerOf(player);
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
        platform::LogDebug(L"synergy partner: {} given back to player {}'s controller (driven by {} now; {} ms after the "
                           L"hand-over, -1 if not remembered; the game's primary controller {})",
                           players::NameOf(member), player, Holder(member), ms,
                           controller == original ? L"is the same" : L"was the other player's");
    }
    return controller;
}

// ---------------------------------------------------------------------------------------------
// The tick's side: member ends off the game thread, and the record of a partner not given back
// ---------------------------------------------------------------------------------------------

const wchar_t* SeenText(EndSeen seen) noexcept
{
    switch (seen)
    {
    case EndSeen::GameThread:
        return L"ran on the game thread without a give-back";
    case EndSeen::Never:
        break;
    }
    return L"not seen";
}

/// Why `held` is not given back now (null: it is, to `controller`): the same tests as T3 on the game thread, read live.
const wchar_t* WhyNotGiveBack(const Held& held, void*& controller) noexcept
{
    controller = nullptr;
    if (const wchar_t* why = LostBy(held.player, held.character))
    {
        return why;
    }
    controller = ControllerOf(held.player);
    if (controller == nullptr)
    {
        return L"its player has no controller";
    }
    if (PlaysInstead(controller, held.character) != nullptr)
    {
        return L"its player plays another character now";
    }
    if (Classic())
    {
        return L"Classic mode keeps the controller";
    }
    if (TypeOf(held.character) >= kNoType)
    {
        return L"no battle-player type (the game's own compare fails)";
    }
    return nullptr;
}

/// Game thread: the member ends that ran off the game thread since the last tick. A remembered character is given back
/// to its player's controller here, with the game's own call, when it still may be (Info: a few per synergy that has a
/// human partner, since member ends come only from link records); the others are logged at Debug.
void TakeNotes() noexcept
{
    std::array<Note, kNotes> notes{};
    std::size_t              count = 0;
    std::uint32_t            lost  = 0;
    {
        Locked lock;
        count       = g_noteCount;
        lost        = g_notesLost;
        std::copy_n(g_notes.begin(), count, notes.begin());
        g_noteCount = 0;
        g_notesLost = 0;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        const Note& note = notes[i];
        ++g_counts.memberEnds;
        ++g_counts.offThread;
        const std::wstring at   = MemberText(note.state, note.started, note.ended);
        Held* const        held = Find(note.member);
        const auto         action =
            DecideNote(held != nullptr, held != nullptr && note.at < held->since,
                       held != nullptr && held->givenAt != Clock::time_point{},
                       [&] { return PlayerHolding(held->character) == held->player; }, note.gameGave);
        if (action == NoteAction::NotRemembered)
        {
            if (Line())
            {
                // The game's queued give-back may not have run yet: "now" is this tick's holder.
                const wchar_t* const what = note.heldBack ? L"no give-back (player 2's or 3's member is given back "
                                                            L"only on the game thread)"
                                            : note.gameGave ? L"the game queued its own give-back"
                                                            : L"no give-back (the game's own test failed)";
                platform::LogDebug(L"synergy partner: member end of {} ran off the game thread (thread {}): not "
                                   L"remembered; {}; driven by {} now (the record: {})",
                                   players::NameOf(note.member), note.thread, what, Holder(note.member), at);
            }
            continue;
        }
        if (action == NoteAction::Stale)
        {
            continue;  // an end of an earlier approach, or one given back already
        }
        if (action == NoteAction::AlreadyHeld)
        {
            *held = {};  // its player drives it already
            continue;
        }
        if (action == NoteAction::GameGave)
        {
            held->givenAt = Clock::now();  // the game queued its own give-back: Confirm checks it
            platform::Log(L"synergy partner: {} given back to player {} by the game's own give-back after its member end "
                          L"off the game thread (thread {}; the record: {})",
                          players::NameOf(held->character), held->player, note.thread, at);
            continue;
        }
        void* controller = nullptr;
        if (const wchar_t* why = WhyNotGiveBack(*held, controller))
        {
            platform::Log(L"synergy partner: {} not given back to player {} after its member end off the game thread "
                          L"(thread {}): {}; it stays with its AI (the record: {})",
                          players::NameOf(held->character), held->player, note.thread, why, at);
            *held = {};
            continue;
        }
        if (!platform::CallGuarded(g_switchAi, const_cast<void*>(held->character), std::uint8_t{1}, controller))
        {
            platform::LogWarning(L"synergy partner: the give-back of {} to player {} faulted", players::NameOf(held->character),
                                 held->player);
            *held = {};
            continue;
        }
        ++g_counts.givenBack;
        held->givenAt = Clock::now();  // Confirm checks the possession
        platform::Log(L"synergy partner: {} given back to player {}'s controller on the game thread, after its member "
                      L"end ran off it (thread {}; the record: {})",
                      players::NameOf(held->character), held->player, note.thread, at);
    }
    if (lost != 0)
    {
        platform::LogWarning(L"synergy partner: {} member ends off the game thread were not noted (more than {} in one "
                             L"frame); a remembered character among them stays with its AI",
                             lost, kNotes);
    }
}

/// Game thread: the tick's give-back after a member end off the game thread. True once it is decided: the possession
/// landed (Debug), or kGoneAfter passed without it (Info).
bool Confirm(const Held& held, Clock::time_point now) noexcept
{
    if (PlayerHolding(held.character) == held.player)
    {
        if (Line())
        {
            platform::LogDebug(L"synergy partner: {} is driven by player {}'s controller again (the give-back landed, "
                               L"{} ms after the call)",
                               players::NameOf(held.character), held.player,
                               std::chrono::duration_cast<std::chrono::milliseconds>(now - held.givenAt).count());
        }
        return true;
    }
    if (now - held.givenAt < kGoneAfter)
    {
        return false;
    }
    platform::Log(L"synergy partner: {}'s give-back to player {} did not land (driven by {})",
                  players::NameOf(held.character), held.player, Holder(held.character));
    return true;
}

/// Game thread: follows the link record that names `held` as the partner. True once that record is gone: silently
/// when its player drives it again; logged when the character is still not given back after kGoneAfter, since nothing
/// in the game gives it back after that (S-SYNSTUCK causes 2 and 3).
bool WatchRecord(Held& held, Clock::time_point now) noexcept
{
    session::LinkRecordView record;
    if (session::party_member::FindPartnerLink(held.type, record))
    {
        held.record     = record;
        held.recordSeen = true;
        held.goneSince  = {};
        return false;
    }
    if (!held.recordSeen)
    {
        return false;
    }
    if (PlayerHolding(held.character) == held.player)
    {
        return true;
    }
    if (held.goneSince == Clock::time_point{})
    {
        held.goneSince = now;  // the member end that removed it may still be noting itself on another thread
        return false;
    }
    if (now - held.goneSince < kGoneAfter)
    {
        return false;
    }
    const std::uint8_t partner = std::min<std::uint8_t>(held.record.partner, std::size(held.record.state) - 1);
    platform::Log(L"synergy partner: {} (player {}'s) is still driven by {} and its synergy record is gone; nothing in "
                  L"the game gives it back now (its member end: {}; the synergy {}; the partner's record: {})",
                  players::NameOf(held.character), held.player, Holder(held.character), SeenText(held.endSeen),
                  RecordText(held.record),
                  MemberText(held.record.state[partner], held.record.started[partner], held.record.ended[partner]));
    return true;
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
    if (!g_pending.exchange(false))
    {
        return;  // removed before the session's first tick
    }
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
        reinterpret_cast<std::uintptr_t>(&Request<0>), reinterpret_cast<std::uintptr_t>(&Request<1>),
        reinterpret_cast<std::uintptr_t>(&Request<2>)};
    for (std::size_t i = 0; ok && i < requests.size(); ++i)
    {
        ok = g_requestCalls[i].InstallFromOwnThread(g_sites.request[i], g_sites.requestAi, requests[i]);
    }
    const std::array<std::uintptr_t, std::size(kStartCalls)> starts{reinterpret_cast<std::uintptr_t>(&Start<0>),
                                                                    reinterpret_cast<std::uintptr_t>(&Start<1>),
                                                                    reinterpret_cast<std::uintptr_t>(&Start<2>)};
    for (std::size_t i = 0; ok && i < starts.size(); ++i)
    {
        ok = g_startCalls[i].InstallFromOwnThread(g_sites.start[i], g_sites.startFn, starts[i]);
    }
    if (ok)
    {
        const auto bytes = kMemberEnd.entryBytes;
        const auto replacement = reinterpret_cast<std::uintptr_t>(&MemberEnd);
        ok = g_memberEnd.Install(g_sites.memberEnd, g_sites.memberEndFirst, replacement) ||
             g_memberEnd.InstallAtSecondInstruction(g_sites.memberEnd, bytes.first(kFirstInstruction),
                                                    bytes.subspan(kFirstInstruction, kSecondInstruction), replacement);
    }
    if (!ok)
    {
        RestoreAll();
    }
    g_on.store(ok, std::memory_order_release);
    platform::Log(L"synergy partner: {}",
                  ok ? L"a synergy partner held by any human player is handed to its AI for the approach and given "
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
    const auto found = [&image](const platform::FunctionFact& fact) {
        return platform::FindFunction(image, fact).value_or(0);
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
    g_sites.startFn     = found(kStart);
    g_sites.startTest   = at(kStartLeaderTest, 5);
    g_sites.requestAi   = found(kRequestAi);
    g_sites.requestTest = at(kRequestLeaderTest, 5);
    g_sites.leaderType  = at(kGetPartyLeaderType.address);
    g_sites.memberEnd   = found(kMemberEnd);
    g_sites.endTest     = at(kEndLeaderTest, 5);
    g_sites.endPrimary  = at(kEndPrimary, 5);
    g_sites.primary     = at(kGetPrimaryEndPlayerController);
    // Optional: without it a parry-window skill's human partner is handed over at the stance, as before.
    g_sites.abilityHandle = at(kAbilityHandleVtable, sizeof(void*));
    const auto typeOf   = platform::FindFunction(image, kPlayerTypeFromComponent);
    g_typeOf            = typeOf ? reinterpret_cast<TypeFn>(*typeOf) : nullptr;
    const auto classic  = platform::FindFunction(image, kClassic);
    g_classic           = classic ? reinterpret_cast<ClassicFn>(*classic) : nullptr;
    const auto switchAi = platform::FindFunction(image, kSwitchAiControlAsync);
    g_switchAi          = switchAi ? reinterpret_cast<SwitchAiFn>(*switchAi) : nullptr;
    g_sites.ok = ok && g_sites.startFn != 0 && g_sites.startTest != 0 && g_classic != nullptr && g_switchAi != nullptr &&
                 g_sites.requestAi != 0 && g_sites.requestTest != 0 && g_sites.leaderType != 0 &&
                 g_sites.memberEnd != 0 && g_sites.endTest != 0 && g_sites.endPrimary != 0 && g_sites.primary != 0 &&
                 g_typeOf != nullptr;
    if (g_sites.ok)
    {
        g_sites.memberEndFirst =
            image.EntryInstruction(kMemberEnd.address, kMemberEnd.entryBytes.first(kFirstInstruction));
    }
    if (!g_sites.ok)
    {
        platform::Log(L"synergy partner: the sites are not what the research read; synergies keep the game's "
                      L"single-player hand-over");
        return;
    }
    g_pending = true;
    if (!platform::game_thread_sites::Queue(&Place))
    {
        g_pending = false;
        platform::Log(L"synergy partner: the game-thread placement queue is full; synergies keep the game's "
                      L"single-player hand-over");
    }
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // the start, the AI request and the member end pass through now
    g_pending.store(false, std::memory_order_release);
    if (g_memberEnd.IsInstalled() && !g_memberEnd.Restore())
    {
        platform::Log(L"synergy partner: the member end's entry was changed after us; left in place (it passes "
                      L"through)");
    }
    // The call redirects stay: on 1.005 some cross a block, which only the one thread that runs them may put back,
    // and unloading is not that thread (platform/call_redirect.h). Every one passes through once off.
    ForgetAll();
    g_sessions = {};
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    // The remembered characters belong to one session of each seat: every one forgotten when any seat's ends or
    // starts (as with player 2 alone).
    std::array<const void*, players::kMaxPlayers> sessions{};
    int                                           changed = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        sessions[static_cast<std::size_t>(seat)] = players::generation::Active(seat) ? players::generation::ControllerOf(seat) : nullptr;
        if (changed == 0 && sessions[static_cast<std::size_t>(seat)] != g_sessions[static_cast<std::size_t>(seat)])
        {
            changed = seat;
        }
    }
    if (changed != 0)
    {
        bool any = false;
        for (const Held& held : g_held)
        {
            any = any || held.player != 0;
        }
        if (any)
        {
            platform::Log(L"synergy partner: {}'s session changed with a character still handed to its AI; "
                          L"forgotten (the game's own give-back applies from now on)",
                          players::SeatName(changed));
        }
        ForgetAll();
        g_sessions = sessions;
    }
    TakeNotes();
    const auto now = Clock::now();
    for (Held& held : g_held)
    {
        if (held.player == 0)
        {
            continue;
        }
        if (!scopes::leader_scope::StillNames(held.weak, held.character))
        {
            held = {};
            continue;
        }
        if (held.givenAt != Clock::time_point{})
        {
            if (Confirm(held, now))
            {
                held = {};
            }
            continue;
        }
        if (const wchar_t* why = LostBy(held.player, held.character))
        {
            if (Line())
            {
                platform::LogDebug(L"synergy partner: {} forgotten (handed to its AI for player {}): {}; the game's own "
                                   L"give-back applies to it",
                                   players::NameOf(held.character), held.player, why);
            }
            held = {};
            continue;
        }
        if (WatchRecord(held, now))
        {
            held = {};
            continue;
        }
        if (!held.warned && now - held.since >= kLateAfter && PlayerHolding(held.character) != held.player)
        {
            held.warned = true;
            platform::Log(L"synergy partner: {} (player {}'s) is still driven by {} {} s after its synergy hand-over; "
                          L"the game has not ended its link yet (its member end: {})",
                          players::NameOf(held.character), held.player, Holder(held.character),
                          std::chrono::duration_cast<std::chrono::seconds>(kLateAfter).count(), SeenText(held.endSeen));
        }
    }
    if (!g_status.Due(now))
    {
        return;
    }
    const Counts c = std::exchange(g_counts, {});
    if (c.requests + c.memberEnds + c.kept == 0)
    {
        return;
    }
    platform::LogDebug(L"synergy partner (10 s): AI requests {}, AI partners {}, humans' partners handed over by the "
                       L"game's own answer {} / by the answered test {}, kept through a parry window {}; member ends {} "
                       L"({} off the game thread), given back to their own player {}",
                       c.requests, c.aiPartners, c.gameHandOvers, c.modHandOvers, c.kept, c.memberEnds, c.offThread,
                       c.givenBack);
}

}  // namespace lss::battle::synergy_partner
