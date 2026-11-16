#include "world/levers.h"

#include "facts/engine.h"
#include "facts/field_action.h"
#include "facts/traversal.h"
#include "input/member_movement.h"
#include "platform/entry_install.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/call_site.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "scopes/primary_controller.h"
#include "traversal/traversal_facts.h"
#include "world/field_pickup.h"
#include "world/lever_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <type_traits>

#include <intrin.h>

namespace lss::world::levers {

namespace {

using field_rules::PointView;

/// The two classes this module carries: the switches and the tower terminals.
enum class Kind : std::uint8_t
{
    Switch,
    Tower,
};
constexpr std::size_t kKinds = 2;

constexpr std::size_t IndexOf(Kind kind) noexcept
{
    return static_cast<std::size_t>(kind);
}

// The switch class's slots, the executer's tick slot and the point setter (research/RE-further-player-levers.md).
constexpr std::uint8_t kNotifyBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                         0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
constexpr platform::SlotFact kNotifySlot{L"the switch's notify slot", 0x145871E20,
                                         {L"SwitchAccess_Vt748_NotifyNear_StartOnHoldProgress", 0x140E83420, kNotifyBytes}};
constexpr std::uint8_t kRegisterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                           0x00, 0x00, 0x00, 0xE8, 0xB8, 0x9D, 0xCC, 0x00};
constexpr platform::SlotFact kRegisterSlot{L"the switch's register slot", 0x145871DF8,
                                           {L"SwitchAccess_Vt720_RegisterWithLeaderPC_Set9E0_PointOn", 0x1413E5168,
                                            kRegisterBytes}};
constexpr std::uint8_t kUnregisterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                             0x00, 0x00, 0x00, 0xE8, 0x64, 0x9D, 0xCC, 0x00};
constexpr platform::SlotFact kUnregisterSlot{L"the switch's unregister slot", 0x145871E00,
                                             {L"SwitchAccess_Vt728_UnregisterLeader_PointOff_Clear9E0",
                                              facts::kSwitchAccessUnregister, kUnregisterBytes}};
constexpr std::uint8_t kExecTickBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                           0xF8, 0x4E, 0x8F, 0x01, 0x48, 0x2B, 0xE0, 0x80};
constexpr platform::SlotFact kExecTickSlot{L"the switch executer's tick slot", 0x145796F38,
                                           {L"SwitchAccessExecuter_Vt20_Tick_CallsVt88", 0x1407BA02C, kExecTickBytes}};
// The setter's entry: mov [rsp+10h],rbx (the 5 bytes replaced), inside one 16-byte block.
constexpr std::uint8_t kSetterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                         0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr platform::FunctionFact kSetter{L"SwitchAccess_SetFieldActionMgrPoint_HoldTime728", 0x140E835F8, kSetterBytes};
// The setter's callers whose off is the actor's own, never a player's wish: the actor tick's (its point-off request)
// and EndPlay's (an unconditional off for an actor leaving play). Known by the return address.
constexpr std::uintptr_t kSetterOwnOffs[] = {0x140E8302D, 0x141444DE0};

// The tower terminal class's slots, its executer's tick slot and its point setter (research/RE-tower-activation.md).
constexpr std::uint8_t kTowerNotifyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                              0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::SlotFact kTowerNotifySlot{L"the tower terminal's notify slot", 0x1458728F0,
                                              {L"OneShotAccess_Vt748_NotifyNear_StartOnHoldProgress", 0x1436A13AC,
                                               kTowerNotifyBytes}};
constexpr std::uint8_t kTowerRegisterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kTowerRegisterSlot{L"the tower terminal's register slot", 0x1458728C8,
                                                {L"OneShotAccess_Vt720_RegisterWithLeaderPC_StoreLeader8B0", 0x1436A0A70,
                                                 kTowerRegisterBytes}};
constexpr std::uint8_t kTowerUnregisterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x20,
                                                  0x00, 0x00, 0x00, 0xE8, 0x80, 0xE3, 0xA0, 0xFE};
constexpr platform::SlotFact kTowerUnregisterSlot{L"the tower terminal's unregister slot", 0x1458728D0,
                                                  {L"OneShotAccess_Vt728_UnregisterLeader_PointOff_Clear8B0",
                                                   facts::kOneShotAccessUnregister, kTowerUnregisterBytes}};
// The executer's tick is the one other executers share: only the tower executer's slot is replaced.
constexpr platform::SlotFact kTowerExecTickSlot{L"the tower terminal executer's tick slot", 0x146706CF0,
                                                traversal::traversal_camera::kSharedTick};
// The setter's entry: mov [rsp+10h],rbx (the 5 bytes replaced), inside one 16-byte block.
constexpr std::uint8_t kTowerSetterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                              0x24, 0x18, 0x57, 0xB8, 0x70, 0x01, 0x00, 0x00};
constexpr platform::FunctionFact kTowerSetter{L"OneShotAccess_SetFieldActionMgrPoint", 0x141445AC0, kTowerSetterBytes};
// The actor's own offs: its tick's point-off request (which the executer's end asks for), EndPlay's, the wake timer's
// (never on a tower; checked all the same, a site the research named).
constexpr std::uintptr_t kTowerSetterOwnOffs[] = {0x1436AA034, 0x141444FE1, 0x1436A0653};
// The one call outside a notify that is a player's: the unregister's off (that player's leave). The actor's state
// update ends in the decide, which calls the setter outside any notify on every actor tick: such a call is the actor's,
// never a player's wish.
constexpr std::uintptr_t kTowerSetterLeaves[] = {0x1436A0BE2};

/// What differs between the two classes: their words in the log, their slots and setter, and the actor's own fields.
struct KindFacts
{
    const wchar_t*                  word;
    const platform::SlotFact&       notify;
    const platform::SlotFact&       registerSlot;
    const platform::SlotFact&       unregister;
    const platform::SlotFact&       execTick;
    const platform::FunctionFact&   setter;
    std::span<const std::uintptr_t> ownOffs;
    // Empty: a call outside a notify is the open scope's player's (the switch: only its unregister calls so). Else the
    // only such calls that are a player's; any other outside a notify is the actor's own.
    std::span<const std::uintptr_t> leaves;
    std::uint32_t                   inRange;      // the switch's byte; the tower's registered character (a weak)
    bool                            inRangeWeak;  // the tower's: an 8-byte weak, not a byte
    std::uint32_t                   latch;
    std::uint32_t                   pointShown;
};
const std::array<KindFacts, kKinds> kKindFacts{{
    {L"switch", kNotifySlot, kRegisterSlot, kUnregisterSlot, kExecTickSlot, kSetter, kSetterOwnOffs, {},
     facts::kSwitchInRange, false, facts::kSwitchPressLatch, facts::kSwitchPointShown},
    {L"tower", kTowerNotifySlot, kTowerRegisterSlot, kTowerUnregisterSlot, kTowerExecTickSlot, kTowerSetter,
     kTowerSetterOwnOffs, kTowerSetterLeaves, facts::kOneShotAccessRegistered, true, facts::kOneShotAccessPressLatch,
     facts::kOneShotAccessPointShown},
}};
constexpr std::size_t kMaxOwnOffs = 4;
constexpr std::size_t kMaxLeaves  = 2;

// Every listed switch and a few towers can hold a record at once; the refused ring remembers actors of the classes
// that are not players 2's and 3's, so the admit test runs once per actor. The delivered points' keys have room for
// every listed switch and every tower terminal (the hub table names 43).
constexpr std::size_t kMaxRecords  = lever_rules::kSwitches.size() + 6;
constexpr std::size_t kMaxKeys     = lever_rules::kSwitches.size() + 48;
constexpr std::size_t kRefusedRing = 16;

using NotifyFn       = void (*)(void* actor, void* context);
using UnregisterFn   = void (*)(void* actor);
using ExecTickFn     = void (*)(void* executer, float dt);
using SwitchSetterFn = void (*)(void* actor, std::uint8_t show, std::uint8_t check, std::uint8_t ng);
using TowerSetterFn  = void (*)(void* actor, std::uint8_t show, std::uint8_t notUsable);

/// The setter's arguments after `show`, as a wishing player's own call last passed them (the tower's one argument in
/// `check`).
struct SetterArgs
{
    std::uint8_t check = 0;
    std::uint8_t ng    = 0;
};

/// One switch's or tower's shared state, per player: the point wished on, the arguments and the point's bits their own
/// call left, in range (and for a tower, the character their register stored), the view their selection last asked;
/// and its press: who pressed, the latch as last looked at, and which players' notifies were said to run with it down.
struct Record
{
    std::uint64_t actor = 0;  // the actor's weak value
    std::uint64_t key   = 0;  // its point's key (its first tag)
    Kind          kind  = Kind::Switch;
    bool          wish[players::kMaxPlayers]      = {};
    SetterArgs    args[players::kMaxPlayers]      = {};
    std::uint8_t  bits[players::kMaxPlayers]      = {};
    bool          inRange[players::kMaxPlayers]   = {};
    std::uint64_t rangeWeak[players::kMaxPlayers] = {};  // a tower's: the registered character each player's register left
    PointView     view[players::kMaxPlayers]       = {};
    PointView     viewLogged[players::kMaxPlayers] = {};
    int           presser   = -1;
    std::uint8_t  lastLatch = 0;
    std::uint8_t  downSaid  = 0;  // by player bit, for the current press
};

/// One class's replaced slots and redirect, and its actor's own off calls by live return address.
struct KindHooks
{
    platform::VTableEntry                   notify, registerSlot, unregister, execTick;
    platform::EntryRedirect                 setter;
    std::array<std::uintptr_t, kMaxOwnOffs> ownOffReturns{};
    std::array<std::uintptr_t, kMaxLeaves>  leaveReturns{};
    bool                                    ready = false;
};
std::array<KindHooks, kKinds> g_hooks;

// Game thread.
std::array<Record, kMaxRecords>         g_records{};
std::array<std::uint64_t, kMaxKeys>     g_keys{};  // delivered actors' point keys
std::size_t                             g_nextKey = 0;
std::array<std::uint64_t, kRefusedRing> g_refused{};  // weak values of actors not players 2's and 3's
std::size_t                             g_nextRefused = 0;
bool                                    g_fullLogged = false, g_scopeLogged = false;
std::array<bool, players::kMaxPlayers>  g_noCopySaid{};  // by seat: a call ran without its slot copy (said once)
constexpr int                           kNotifiedNone  = -1;  // no notify runs (the unregister: the open scope's player)
constexpr int                           kNotifiedOther = -2;  // a notify for a character no player plays
thread_local int                        t_notified     = kNotifiedNone;  // else the player whose character's notify runs
std::array<const void*, players::kMaxPlayers> g_execSeen{};  // by seat: the executer last ticked, for the log

using field_rules::AnyOn;
using field_rules::FirstOn;
using players::actor_fields::FirstTag;
using players::generation::PlayerOfCharacter;

const KindFacts& FactsOf(Kind kind) noexcept
{
    return kKindFacts[IndexOf(kind)];
}

KindHooks& HooksOf(Kind kind) noexcept
{
    return g_hooks[IndexOf(kind)];
}

bool AnyReady() noexcept
{
    return HooksOf(Kind::Switch).ready || HooksOf(Kind::Tower).ready;
}

bool Idle(const Record& r) noexcept
{
    return !AnyOn(r.wish) && !AnyOn(r.inRange) && r.presser < 0 && r.lastLatch == 0;
}

bool AdmitsOf(Kind kind, const void* actor) noexcept
{
    return kind == Kind::Switch ? Admits(actor) : AdmitsTower(actor);
}

/// The record of `actor`: its entry (its key read again while it had none), else (with `make`, for an actor of `kind`
/// players 2 and 3 may use) a free, dead or idle one made its; null when none, the actor is not admitted or has no weak
/// value.
Record* RecordOf(const void* actor, bool make, Kind kind) noexcept
{
    const std::uint64_t weak = players::weak_object::WeakOf(const_cast<void*>(actor));
    if (weak == 0)
    {
        return nullptr;
    }
    for (Record& r : g_records)
    {
        if (r.actor == weak)
        {
            r.key = r.key != 0 ? r.key : FirstTag(actor);
            return &r;
        }
    }
    if (!make)
    {
        return nullptr;
    }
    for (const std::uint64_t refused : g_refused)
    {
        if (refused == weak)
        {
            return nullptr;
        }
    }
    if (!AdmitsOf(kind, actor))
    {
        g_refused[g_nextRefused] = weak;
        g_nextRefused            = (g_nextRefused + 1) % kRefusedRing;
        return nullptr;
    }
    for (Record& r : g_records)
    {
        if (r.actor == 0 || Idle(r) || players::weak_object::ObjectOf(r.actor) == nullptr)
        {
            r = {weak, FirstTag(actor), kind};
            return &r;
        }
    }
    if (!g_fullLogged)
    {
        g_fullLogged = true;
        try
        {
            platform::LogWarning(L"levers: no room to record the players' state of the {} {}: the game's own calls stand "
                                 L"for it (said once)",
                                 FactsOf(kind).word, players::NameOf(actor));
        }
        catch (...)
        {
        }
    }
    return nullptr;
}

/// A seat's call made without its copy of the slots (its first pass not run yet): its hold reads 0. Said once a seat.
void NoteNoCopy(int seat, const void* actor, Kind kind) noexcept
{
    if (g_noCopySaid[static_cast<std::size_t>(seat)])
    {
        return;
    }
    g_noCopySaid[static_cast<std::size_t>(seat)] = true;
    try
    {
        platform::LogDebug(L"levers: {} has no copy of the prompt slots for the {} {} yet: their hold reads 0 there (said "
                           L"once)",
                           players::SeatName(seat), FactsOf(kind).word, players::NameOf(actor));
    }
    catch (...)
    {
    }
}

/// The in-range field as the players in range have it: a switch's byte as their OR; a tower's registered character as
/// the first player in range's (cleared with nobody in range, as the game's leave leaves it).
void WriteInRange(void* actor, const Record& r) noexcept
{
    const KindFacts& f   = FactsOf(r.kind);
    auto*            dst = static_cast<std::uint8_t*>(actor) + f.inRange;
    if (!f.inRangeWeak)
    {
        const std::uint8_t any = AnyOn(r.inRange) ? 1 : 0;
        (void)platform::WriteGuarded(dst, &any, sizeof(any));
        return;
    }
    // The first player in range whose character still lives: a stale one would hide the point from everyone.
    std::uint64_t weak = 0;
    for (std::size_t player = 0; player < players::kMaxPlayers && weak == 0; ++player)
    {
        if (r.inRange[player] && players::weak_object::ObjectOf(r.rangeWeak[player]) != nullptr)
        {
            weak = r.rangeWeak[player];
        }
    }
    (void)platform::WriteGuarded(dst, &weak, sizeof(weak));
}

/// The in-range field read as "this player's register took": a switch's byte set, a tower's registered character
/// valid (its weak value in `weak`).
bool InRangeRead(const void* actor, Kind kind, std::uint64_t& weak) noexcept
{
    const KindFacts& f = FactsOf(kind);
    if (!f.inRangeWeak)
    {
        weak = 0;
        return ReadField<std::uint8_t>(actor, f.inRange) != 0;
    }
    weak = ReadField<std::uint64_t>(actor, f.inRange);
    return weak != 0 && players::weak_object::ObjectIn(actor, f.inRange) != nullptr;
}

/// A presser whose wish goes off while the point stays on for another player (they walked away, or left the game): the
/// game's off edge, which clears the press latch, is what the OR of the wishes holds back here, so its clear is made in
/// its stead; otherwise the pending press would hold the other player's notifies for as long as they stay.
void SpendPressAtLeave(void* actor, Record& r, int player) noexcept
{
    if (r.presser != player)
    {
        return;
    }
    constexpr std::uint8_t kOff = 0;
    (void)platform::WriteGuarded(static_cast<std::uint8_t*>(actor) + FactsOf(r.kind).latch, &kOff, sizeof(kOff));
    r.presser   = -1;
    r.lastLatch = 0;
    try
    {
        platform::LogDebug(L"levers: {}'s press on the {} {} is spent with their leave (the point stays on for another "
                           L"player: the latch cleared as the game's off would)",
                           players::SeatName(player), FactsOf(r.kind).word, players::NameOf(actor));
    }
    catch (...)
    {
    }
}

void LogWish(const void* actor, Kind kind, int player, bool wish, bool any, const wchar_t* done) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"levers: {}'s wish for the {} {} is {}; the point {}{}", players::SeatName(player),
                           FactsOf(kind).word, players::NameOf(actor), wish ? L"on" : L"off", any ? L"on" : L"off", done);
    }
    catch (...)
    {
    }
}

/// The game's point setter of `kind`, with a wishing player's arguments.
void CallSetter(Kind kind, void* actor, std::uint8_t show, SetterArgs args) noexcept
{
    const std::uintptr_t original = HooksOf(kind).setter.Original();
    if (kind == Kind::Switch)
    {
        reinterpret_cast<SwitchSetterFn>(original)(actor, show, args.check, args.ng);
    }
    else
    {
        reinterpret_cast<TowerSetterFn>(original)(actor, show, args.check);
    }
}

template <std::size_t N>
bool Among(const std::array<std::uintptr_t, N>& returns, std::uintptr_t caller) noexcept
{
    for (const std::uintptr_t at : returns)
    {
        if (at != 0 && at == caller)
        {
            return true;
        }
    }
    return false;
}

/// A setter call outside any notify that is no player's: with leave sites named, every caller but those (the
/// actor's state update's decide, on each actor tick).
bool ActorsOwnOutsideNotify(Kind kind, std::uintptr_t caller) noexcept
{
    return t_notified == kNotifiedNone && !FactsOf(kind).leaves.empty() && !Among(HooksOf(kind).leaveReturns, caller);
}

/// The point setter, from the notify's decide (on or off) and the unregister (off): the caller's wish recorded for the
/// player the notify runs for, else the player whose scopes are open (the unregister; player 1's outside any), the
/// game's setter called with the OR of every player's. A player's own on call runs with their own arguments, in their
/// own context (the setter reads the primary controller's character and the party leader for the point's "not usable"
/// bit), and the bits it leaves on the point are noted as that player's view. The actor's own off calls (its tick's
/// point-off request, its EndPlay) go to the game as they are: the point comes back with the next wishing notify.
void SetterOr(Kind kind, void* actor, std::uint8_t show, SetterArgs args, std::uintptr_t caller) noexcept
{
    if (actor == nullptr || !players::generation::OnGameThread() || Among(HooksOf(kind).ownOffReturns, caller))
    {
        CallSetter(kind, actor, show, args);
        return;
    }
    Record* r = RecordOf(actor, true, kind);  // player 1's wish recorded alone too: a seat joining later ORs with it
    if (r == nullptr)
    {
        CallSetter(kind, actor, show, args);  // an actor players 2 and 3 do not use: the game's alone
        return;
    }
    const bool actorsOwn = ActorsOwnOutsideNotify(kind, caller);
    if (actorsOwn && show == 0)
    {
        // The actor's own decide says off (in use, done, unavailable): the game's, as in single player. Its off edge
        // clears the press latch; a player still wishing puts the point back at their next notify.
        CallSetter(kind, actor, 0, args);
        r->presser   = -1;
        r->lastLatch = 0;
        return;
    }
    if (t_notified == kNotifiedOther || actorsOwn)
    {
        // A character no player plays, or the actor's own update: no player's wish. The players' setter stands; with
        // no player wishing, the game's.
        if (const int wisher = FirstOn(r->wish); wisher >= 0)
        {
            CallSetter(kind, actor, 1, r->args[wisher]);
        }
        else
        {
            CallSetter(kind, actor, show, args);
        }
        return;
    }
    const int  player  = t_notified >= 0 ? t_notified : scopes::primary_controller::OpenSeat();
    const bool wish    = show != 0;
    const bool changed = r->wish[player] != wish;
    r->wish[player]    = wish;
    if (wish)
    {
        r->args[player] = args;
    }
    if (!AnyOn(r->wish))
    {
        CallSetter(kind, actor, 0, args);  // the last player's off: the game's own call
        r->presser   = -1;                 // the off edge clears the game's latch: no press is pending
        r->lastLatch = 0;
        if (changed)
        {
            LogWish(actor, kind, player, wish, false, L" (the game's off)");
        }
        return;
    }
    if (changed && !wish)
    {
        SpendPressAtLeave(actor, *r, player);
    }
    if (wish)
    {
        CallSetter(kind, actor, 1, args);
        std::uint8_t flags = 0;
        if (field_pickup::PointFlagsOf(r->key, flags))
        {
            r->bits[player] = flags;
        }
    }
    else
    {
        CallSetter(kind, actor, 1, r->args[FirstOn(r->wish)]);
    }
    if (changed)
    {
        LogWish(actor, kind, player, wish, true, wish ? L"" : L" (another player holds it)");
    }
}

void SwitchSetter(void* actor, std::uint8_t show, std::uint8_t check, std::uint8_t ng) noexcept
{
    SetterOr(Kind::Switch, actor, show, {check, ng}, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

void TowerSetter(void* actor, std::uint8_t show, std::uint8_t notUsable) noexcept
{
    SetterOr(Kind::Tower, actor, show, {notUsable, 0}, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

/// Before the game's register: the actor's record, made if new, and its in-range field cleared; false when the actor is
/// not tracked (the game's register runs alone).
template <Kind K>
bool BeforeRegister(void* actor) noexcept
{
    if (actor == nullptr || !players::generation::OnGameThread() || RecordOf(actor, true, K) == nullptr)
    {
        return false;
    }
    const KindFacts&        f      = FactsOf(K);
    constexpr std::uint64_t kClear = 0;
    const std::size_t       width  = f.inRangeWeak ? sizeof(std::uint64_t) : sizeof(std::uint8_t);
    return platform::WriteGuarded(static_cast<std::uint8_t*>(actor) + f.inRange, &kClear, width);
}

/// After the game's register: whether this player's took, and the field as the players in range have it.
template <Kind K>
void AfterRegister(void* actor) noexcept
{
    if (Record* r = RecordOf(actor, false, K); r != nullptr)
    {
        const int     player = scopes::primary_controller::OpenSeat();
        std::uint64_t weak   = 0;
        r->inRange[player]   = InRangeRead(actor, K, weak);
        r->rangeWeak[player] = r->inRange[player] ? weak : 0;
        WriteInRange(actor, *r);
        if constexpr (K == Kind::Tower)
        {
            try
            {
                platform::LogDebug(L"levers: {} is at the tower {} (hub {}): their register {}", players::SeatName(player),
                                   players::NameOf(actor),
                                   players::unreal::NameText(ReadField<std::uint64_t>(actor, facts::kOneShotAccessHubId)),
                                   r->inRange[player] ? L"took" : L"did not take (the leader is no player character)");
            }
            catch (...)
            {
            }
        }
    }
}

/// What the class's register returns: the switch's a byte, the tower's nothing (its caller reads none).
template <Kind K>
using RegisterResult = std::conditional_t<K == Kind::Switch, std::uint8_t, void>;

/// The class's register (the trigger's enter): the game's, as the open scope's player (a seat's inside its delivery,
/// player 1 outside any), run with the in-range field cleared so what it leaves says whether this player's register
/// took (the game sets it only when the leader is a player character); then written as the players in range have it.
template <Kind K>
RegisterResult<K> Register(void* actor) noexcept
{
    const auto original = reinterpret_cast<RegisterResult<K> (*)(void*)>(HooksOf(K).registerSlot.Original());
    const bool tracked  = BeforeRegister<K>(actor);
    if constexpr (std::is_void_v<RegisterResult<K>>)
    {
        original(actor);
        if (tracked)
        {
            AfterRegister<K>(actor);
        }
    }
    else
    {
        const auto result = original(actor);
        if (tracked)
        {
            AfterRegister<K>(actor);
        }
        return result;
    }
}

/// The class's unregister (the trigger's leave; its point off goes through the setter's OR): the game's, then the
/// in-range field as the players still in range have it. The game turns the point off before it clears the registered
/// character, so a "another player holds it" setter call inside still finds the field valid; it is written back after.
template <Kind K>
void Unregister(void* actor) noexcept
{
    reinterpret_cast<UnregisterFn>(HooksOf(K).unregister.Original())(actor);
    if (actor == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    if (Record* r = RecordOf(actor, false, K); r != nullptr)
    {
        const int player     = scopes::primary_controller::OpenSeat();
        r->inRange[player]   = false;
        r->rangeWeak[player] = 0;
        WriteInRange(actor, *r);
    }
}

/// The game's notify for `notified`'s character (0 player 1, a seat, -1 no player's): a seat's with the seat's slot
/// block held in the live manager, inside the seat's primary scope and leader answer, the player named for the setter
/// meanwhile (kNotifiedOther for a character no player plays).
void CallNotify(NotifyFn original, void* actor, void* context, int notified, std::uint64_t key, Kind kind) noexcept
{
    const int saved = t_notified;
    t_notified      = notified >= 0 ? notified : kNotifiedOther;
    if (notified < 1)
    {
        original(actor, context);
        t_notified = saved;
        return;
    }
    const field_pickup::SlotHold slots(notified, key);
    if (!slots.Held())
    {
        NoteNoCopy(notified, actor, kind);
    }
    if (!input::member_movement::NotifyInPrimaryScope(notified, original, actor, context))
    {
        if (!g_scopeLogged)
        {
            g_scopeLogged = true;
            try
            {
                platform::LogWarning(L"levers: the {} {}'s notify for {}'s character ran outside their scope (it could "
                                     L"not be held): its answers are player 1's, booked to no player (said once)",
                                     FactsOf(kind).word, players::NameOf(actor), players::SeatName(notified));
            }
            catch (...)
            {
            }
        }
        t_notified = kNotifiedOther;  // player 1's answers: no player's wish
        original(actor, context);
    }
    t_notified = saved;
}

/// A latch seen up for the first time: `player` pressed (`how` ends the line).
void NoteRise(Record& r, int player, const void* actor, const wchar_t* how) noexcept
{
    r.presser   = player;
    r.lastLatch = 1;
    r.downSaid  = 0;
    try
    {
        platform::LogDebug(L"levers: {} pressed the {} {}{}", players::SeatName(player), FactsOf(r.kind).word,
                           players::NameOf(actor), how);
    }
    catch (...)
    {
    }
}

/// The class's notify: the game's (CallNotify), with the press latch down for a call whose character is not the
/// presser's (lever_rules::LatchDownFor). A latch up that no seat's pass named is player 1's (a seat's press runs
/// inside its pass, which names it at once; player 1's in the game's own input step, which may come before this
/// notify and the game's update). The latch goes back up after unless the point went off inside the call (its off edge
/// clears the latch; one put back then would outlive the point and fire a press nobody made at the next on).
template <Kind K>
void Notify(void* actor, void* context) noexcept
{
    const auto original = reinterpret_cast<NotifyFn>(HooksOf(K).notify.Original());
    if (actor == nullptr || !players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        original(actor, context);
        return;
    }
    const KindFacts& f        = FactsOf(K);
    const int        notified = PlayerOfCharacter(ReadPointer(context, facts::kNotifyContextCharacter));
    Record*          r        = RecordOf(actor, true, K);
    const auto       key      = r != nullptr ? r->key : FirstTag(actor);
    std::uint8_t*    latch    = static_cast<std::uint8_t*>(actor) + f.latch;
    if (r != nullptr && *latch != 0 && r->lastLatch == 0)
    {
        NoteRise(*r, 0, actor, L" (named at the first notify after the game's own input step)");
    }
    if (r != nullptr && r->presser >= 1 && !players::generation::Active(r->presser))
    {
        SpendPressAtLeave(actor, *r, r->presser);  // the presser left the game with their press pending
    }
    const int pressed = r != nullptr && *latch != 0 ? r->presser : -1;
    if (!lever_rules::LatchDownFor(pressed, notified))
    {
        CallNotify(original, actor, context, notified, key, K);
        return;
    }
    constexpr std::uint8_t kOff = 0, kOn = 1;
    if (!platform::WriteGuarded(latch, &kOff, sizeof(kOff)))
    {
        CallNotify(original, actor, context, notified, key, K);
        return;
    }
    CallNotify(original, actor, context, notified, key, K);
    const bool pointOn = ReadField<std::uint8_t>(actor, f.pointShown) != 0;
    r                  = RecordOf(actor, false, K);  // the call may have made the record idle and another taken it
    if (pointOn)
    {
        (void)platform::WriteGuarded(latch, &kOn, sizeof(kOn));
    }
    if (r == nullptr)
    {
        return;
    }
    r->lastLatch = pointOn ? 1 : 0;
    r->presser   = pointOn ? r->presser : -1;
    const auto bit = static_cast<std::uint8_t>(1u << (notified >= 0 ? notified : players::kMaxPlayers));
    if ((r->downSaid & bit) != 0 || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    r->downSaid |= bit;
    try
    {
        platform::LogDebug(L"levers: the {} {}'s press is {}'s: the notify for {}'s character runs with the latch down{}",
                           f.word, players::NameOf(actor), players::SeatName(pressed),
                           notified >= 0 ? players::SeatName(notified) : L"no player",
                           pointOn ? L"" : L" (the point went off inside: the latch stays down)");
    }
    catch (...)
    {
    }
}

/// The class's executer tick (its phase update polls the hold progress): a seat's member's with the seat's slot block
/// held in the live manager for the call, so the hold the seat's pass keeps is the one it polls. Each seat's executer
/// is logged as it starts.
template <Kind K>
void ExecTick(void* executer, float dt) noexcept
{
    const auto original = reinterpret_cast<ExecTickFn>(HooksOf(K).execTick.Original());
    if (executer == nullptr || !players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        original(executer, dt);
        return;
    }
    const void* character = players::weak_object::ObjectIn(executer, facts::kSwitchExecCharacter);
    void*       actor     = players::weak_object::ObjectIn(executer, facts::kSwitchExecActor);
    const int   player    = PlayerOfCharacter(character);
    if (player < 1 || actor == nullptr)
    {
        original(executer, dt);
        return;
    }
    if (g_execSeen[static_cast<std::size_t>(player)] != executer)
    {
        g_execSeen[static_cast<std::size_t>(player)] = executer;
        try
        {
            const auto start = ReadField<std::uint64_t>(actor, facts::kStateManagedDefaultStart);
            const auto end   = ReadField<std::uint64_t>(actor, facts::kStateManagedDefaultEnd);
            platform::LogDebug(L"levers: {}'s {} move on {} runs (their character {}; the default start trigger {}, end {}: "
                               L"sent for the leader's type only, so not for this move)",
                               players::SeatName(player), FactsOf(K).word, players::NameOf(actor),
                               players::NameOf(character),
                               start != 0 ? players::unreal::NameText(start) : std::wstring(L"none"),
                               end != 0 ? players::unreal::NameText(end) : std::wstring(L"none"));
        }
        catch (...)
        {
        }
    }
    const field_pickup::SlotHold slots(player, FirstTag(actor));
    if (!slots.Held())
    {
        NoteNoCopy(player, actor, K);
    }
    original(executer, dt);
}

void RemoveKind(Kind kind) noexcept
{
    KindHooks& h = HooksOf(kind);
    h.ready      = false;
    if (h.setter.IsInstalled())
    {
        (void)h.setter.Restore();
    }
    for (platform::VTableEntry* slot : {&h.notify, &h.registerSlot, &h.unregister, &h.execTick})
    {
        if (slot->Original() != 0)
        {
            (void)slot->Restore();
        }
    }
    h.ownOffReturns.fill(0);
    h.leaveReturns.fill(0);
}

/// One class's hooks, all or none: each own-off return address trusted only once its site is checked as a call of
/// the setter.
template <Kind K>
bool InstallKind(const platform::GameImage& image, std::uintptr_t setterHook) noexcept
{
    const KindFacts& f      = FactsOf(K);
    KindHooks&       h      = HooksOf(K);
    const auto       setter = platform::FindFunction(image, f.setter);
    bool ready = setter.has_value() && f.ownOffs.size() <= kMaxOwnOffs && f.leaves.size() <= kMaxLeaves;
    for (std::size_t i = 0; ready && i < f.ownOffs.size(); ++i)
    {
        const auto site    = image.Rebase(f.ownOffs[i], 5);
        ready              = site && players::call_site::Calls(*site, *setter);
        h.ownOffReturns[i] = ready ? *site + 5 : 0;
    }
    for (std::size_t i = 0; ready && i < f.leaves.size(); ++i)
    {
        const auto site   = image.Rebase(f.leaves[i], 5);
        ready             = site && players::call_site::Calls(*site, *setter);
        h.leaveReturns[i] = ready ? *site + 5 : 0;
    }
    ready = ready && platform::InstallAtEntryOrSecond(h.setter, image, f.setter, setterHook) &&
            platform::ReplaceSlot(image, f.registerSlot, reinterpret_cast<std::uintptr_t>(&Register<K>), h.registerSlot) &&
            platform::ReplaceSlot(image, f.unregister, reinterpret_cast<std::uintptr_t>(&Unregister<K>), h.unregister) &&
            platform::ReplaceSlot(image, f.notify, reinterpret_cast<std::uintptr_t>(&Notify<K>), h.notify) &&
            platform::ReplaceSlot(image, f.execTick, reinterpret_cast<std::uintptr_t>(&ExecTick<K>), h.execTick);
    if (!ready)
    {
        RemoveKind(K);
        return false;
    }
    h.ready = true;
    return true;
}

/// A delivered actor of `kind` (its first tag is its point's key): the seats' passes admit that point from here on.
void NoteDelivered(const void* actor, Kind kind) noexcept
{
    if (!HooksOf(kind).ready || actor == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    (void)RecordOf(actor, true, kind);
    const std::uint64_t key = FirstTag(actor);
    if (key == 0)
    {
        return;
    }
    for (const std::uint64_t noted : g_keys)
    {
        if (noted == key)
        {
            return;
        }
    }
    const std::uint64_t dropped = g_keys[g_nextKey];
    g_keys[g_nextKey]           = key;
    g_nextKey                   = (g_nextKey + 1) % kMaxKeys;
    try
    {
        platform::LogDebug(L"levers: the {} {} is delivered: its point is a player 2's or 3's press from here on{}",
                           FactsOf(kind).word, players::NameOf(actor),
                           dropped != 0 ? std::format(L" (the oldest noted point {} forgotten)", players::unreal::NameText(dropped))
                                        : std::wstring());
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    if (InstallKind<Kind::Switch>(image, reinterpret_cast<std::uintptr_t>(&SwitchSetter)))
    {
        platform::Log(L"levers: switches ready; players 2 and 3 work the listed switches ({} of them) on their own "
                      L"character, with their own hold (the point takes every player's wish, the press latch is the "
                      L"presser's, the in-range byte every player's)",
                      lever_rules::kSwitches.size());
    }
    else
    {
        platform::LogWarning(L"levers: switches NOT placed (a slot, the setter or one of its callers is not as the research "
                             L"read it): switches stay player 1's");
    }
    if (InstallKind<Kind::Tower>(image, reinterpret_cast<std::uintptr_t>(&TowerSetter)))
    {
        platform::Log(L"levers: towers ready; players 2 and 3 activate the communication towers' terminals on their own "
                      L"character (the point takes every player's wish, the press latch is the presser's, the registered "
                      L"character is a player's in range)");
    }
    else
    {
        platform::LogWarning(L"levers: towers NOT placed (a slot, the setter or one of its callers is not as the research "
                             L"read it): the towers stay player 1's");
    }
}

void Remove() noexcept
{
    RemoveKind(Kind::Switch);
    RemoveKind(Kind::Tower);
    g_records.fill({});
    g_keys.fill(0);
    g_refused.fill(0);
    g_execSeen.fill(nullptr);
    g_noCopySaid.fill(false);
    g_fullLogged = g_scopeLogged = false;
}

void Tick() noexcept
{
    if (!AnyReady() || !players::generation::OnGameThread())
    {
        return;
    }
    for (Record& r : g_records)
    {
        void* actor = r.actor != 0 ? players::weak_object::ObjectOf(r.actor) : nullptr;
        if (actor == nullptr)
        {
            r = {};
            continue;
        }
        bool wished = false, marked = false;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (players::generation::Active(seat))
            {
                continue;
            }
            wished             = wished || r.wish[seat];
            marked             = marked || r.inRange[seat];
            r.wish[seat]       = false;
            r.inRange[seat]    = false;
            r.rangeWeak[seat]  = 0;
            r.args[seat]       = {};
            r.bits[seat]       = 0;
            r.view[seat]       = PointView::NotTracked;
            r.viewLogged[seat] = PointView::NotTracked;
            if (r.presser == seat)
            {
                SpendPressAtLeave(actor, r, seat);
            }
        }
        if (marked)
        {
            WriteInRange(actor, r);
        }
        if (wished && !AnyOn(r.wish) && ReadField<std::uint8_t>(actor, FactsOf(r.kind).pointShown) != 0)
        {
            CallSetter(r.kind, actor, 0, {});
            try
            {
                platform::LogDebug(L"levers: the {} {}'s point is off: the player who wished it on left the game",
                                   FactsOf(r.kind).word, players::NameOf(actor));
            }
            catch (...)
            {
            }
        }
    }
}

bool Ready() noexcept
{
    return HooksOf(Kind::Switch).ready;
}

bool TowerReady() noexcept
{
    return HooksOf(Kind::Tower).ready;
}

bool Admits(const void* actor) noexcept
{
    if (actor == nullptr)
    {
        return false;
    }
    try
    {
        return lever_rules::Admitted(players::NameOf(actor)) || lever_rules::Admitted(players::unreal::NameText(FirstTag(actor)));
    }
    catch (...)
    {
    }
    return false;
}

bool AdmitsTower(const void* actor) noexcept
{
    return actor != nullptr && ReadField<std::uint8_t>(actor, facts::kOneShotAccessTower) != 0;
}

void NoteSwitch(const void* actor) noexcept
{
    NoteDelivered(actor, Kind::Switch);
}

void NoteTower(const void* actor) noexcept
{
    NoteDelivered(actor, Kind::Tower);
}

bool AdmitsPoint(std::uint64_t key) noexcept
{
    if (!AnyReady() || key == 0)
    {
        return false;
    }
    for (const std::uint64_t noted : g_keys)
    {
        if (noted == key)
        {
            return true;
        }
    }
    return false;
}

void NotePressesAfter(int player) noexcept
{
    if (!AnyReady() || !players::generation::OnGameThread())
    {
        return;
    }
    for (Record& r : g_records)
    {
        const void* actor = r.actor != 0 ? players::weak_object::ObjectOf(r.actor) : nullptr;
        if (actor == nullptr)
        {
            continue;
        }
        const std::uint8_t latch = ReadField<std::uint8_t>(actor, FactsOf(r.kind).latch);
        if (latch != 0 && r.lastLatch == 0)
        {
            NoteRise(r, player, actor, L"");
        }
        else if (latch == 0)
        {
            r.presser = -1;
        }
        r.lastLatch = latch;
    }
}

PointView PointViewFor(std::uint64_t key, int player) noexcept
{
    if (!AnyReady() || key == 0 || player < 0 || player >= static_cast<int>(players::kMaxPlayers) ||
        !players::generation::OnGameThread())
    {
        return PointView::NotTracked;
    }
    for (Record& r : g_records)
    {
        if (r.actor == 0 || r.key != key || !AnyOn(r.wish))
        {
            continue;
        }
        const PointView view = !r.wish[player]                                            ? PointView::Off
                               : (r.bits[player] & facts::kFieldActionPointNotUsable) != 0 ? PointView::NotUsable
                                                                                          : PointView::Usable;
        r.view[player] = view;
        return view;
    }
    return PointView::NotTracked;
}

void LogPointViews() noexcept
{
    if (!AnyReady() || !players::generation::OnGameThread() || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    for (Record& r : g_records)
    {
        for (int player = 0; player < static_cast<int>(players::kMaxPlayers); ++player)
        {
            const PointView view = r.view[player];
            if (view == PointView::NotTracked || view == r.viewLogged[player])
            {
                continue;
            }
            r.viewLogged[player] = view;
            try
            {
                platform::LogDebug(L"levers: {}'s selection sees the {} {} {}", players::SeatName(player),
                                   FactsOf(r.kind).word, players::NameOf(players::weak_object::ObjectOf(r.actor)),
                                   view == PointView::Off         ? L"off (their own notify wishes it off)"
                                   : view == PointView::NotUsable ? L"on, not usable from where they are (their own call's answer)"
                                                                  : L"on and usable");
            }
            catch (...)
            {
            }
        }
    }
}

}  // namespace lss::world::levers
