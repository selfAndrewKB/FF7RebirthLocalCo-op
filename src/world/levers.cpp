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
#include "world/field_pickup.h"
#include "world/lever_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#include <intrin.h>

namespace lss::world::levers {

namespace {

using field_rules::PointView;

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
// The setter's two callers whose off is the actor's own, never a player's wish: the actor tick's (its point-off request)
// and EndPlay's (an unconditional off for an actor leaving play). Known by the return address.
constexpr std::uintptr_t kSetterCallTick    = 0x140E8302D;
constexpr std::uintptr_t kSetterCallEndPlay = 0x141444DE0;

// Every listed switch can hold a record at once; the refused ring remembers switches not on the list, so the name test
// runs once per actor.
constexpr std::size_t kMaxSwitches = lever_rules::kSwitches.size();
constexpr std::size_t kRefusedRing = 16;

using NotifyFn     = void (*)(void* actor, void* context);
using RegisterFn   = std::uint8_t (*)(void* actor);
using UnregisterFn = void (*)(void* actor);
using ExecTickFn   = void (*)(void* executer, float dt);
using SetterFn     = void (*)(void* actor, std::uint8_t show, std::uint8_t check, std::uint8_t ng);

/// The setter's arguments after `show`, as a wishing player's own call last passed them.
struct SetterArgs
{
    std::uint8_t check = 0;
    std::uint8_t ng    = 0;
};

/// One listed switch's shared state, per player: the point wished on, the arguments and the point's bits their own call
/// left, in range, the view their selection last asked; and its press: who pressed, the latch as last looked at, and
/// which players' notifies were said to run with it down.
struct Switch
{
    std::uint64_t actor = 0;  // the actor's weak value
    std::uint64_t key   = 0;  // its point's key (its first tag)
    bool          wish[players::kMaxPlayers]    = {};
    SetterArgs    args[players::kMaxPlayers]    = {};
    std::uint8_t  bits[players::kMaxPlayers]    = {};
    bool          inRange[players::kMaxPlayers] = {};
    PointView     view[players::kMaxPlayers]       = {};
    PointView     viewLogged[players::kMaxPlayers] = {};
    int           presser   = -1;
    std::uint8_t  lastLatch = 0;
    std::uint8_t  downSaid  = 0;  // by player bit, for the current press
};

platform::VTableEntry   g_notify, g_register, g_unregister, g_execTick;
platform::EntryRedirect g_setter;
std::uintptr_t          g_tickReturn = 0, g_endPlayReturn = 0;  // live return addresses of the actor's own off calls
bool                    g_ready = false;

// Game thread.
std::array<Switch, kMaxSwitches>        g_switches{};
std::array<std::uint64_t, kMaxSwitches> g_keys{};  // delivered switches' point keys
std::size_t                             g_nextKey = 0;
std::array<std::uint64_t, kRefusedRing> g_refused{};  // weak values of switch actors not on the list
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

bool Idle(const Switch& s) noexcept
{
    return !AnyOn(s.wish) && !AnyOn(s.inRange) && s.presser < 0 && s.lastLatch == 0;
}

/// The record of `actor`: its entry (its key read again while it had none), else (with `make`, for a listed switch) a
/// free, dead or idle one made its; null when none, the actor is not listed or has no weak value.
Switch* SwitchOf(const void* actor, bool make) noexcept
{
    const std::uint64_t weak = players::weak_object::WeakOf(const_cast<void*>(actor));
    if (weak == 0)
    {
        return nullptr;
    }
    for (Switch& s : g_switches)
    {
        if (s.actor == weak)
        {
            s.key = s.key != 0 ? s.key : FirstTag(actor);
            return &s;
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
    if (!Admits(actor))
    {
        g_refused[g_nextRefused] = weak;
        g_nextRefused            = (g_nextRefused + 1) % kRefusedRing;
        return nullptr;
    }
    for (Switch& s : g_switches)
    {
        if (s.actor == 0 || Idle(s) || players::weak_object::ObjectOf(s.actor) == nullptr)
        {
            s = {weak, FirstTag(actor)};
            return &s;
        }
    }
    if (!g_fullLogged)
    {
        g_fullLogged = true;
        try
        {
            platform::LogWarning(L"levers: no room to record the players' state of the switch {}: the game's own calls stand "
                                 L"for it (said once)",
                                 players::NameOf(actor));
        }
        catch (...)
        {
        }
    }
    return nullptr;
}

/// A seat's call made without its copy of the slots (its first pass not run yet): its hold reads 0. Said once a seat.
void NoteNoCopy(int seat, const void* actor) noexcept
{
    if (g_noCopySaid[static_cast<std::size_t>(seat)])
    {
        return;
    }
    g_noCopySaid[static_cast<std::size_t>(seat)] = true;
    try
    {
        platform::LogDebug(L"levers: {} has no copy of the prompt slots for the switch {} yet: their hold reads 0 there (said "
                           L"once)",
                           players::SeatName(seat), players::NameOf(actor));
    }
    catch (...)
    {
    }
}
/// The in-range byte as the OR of the players in range.
void WriteInRange(void* actor, const Switch& s) noexcept
{
    const std::uint8_t any = AnyOn(s.inRange) ? 1 : 0;
    (void)platform::WriteGuarded(static_cast<std::uint8_t*>(actor) + facts::kSwitchInRange, &any, sizeof(any));
}

/// A presser whose wish goes off while the point stays on for another player (they walked away, or left the game): the
/// game's off edge, which clears the press latch, is what the OR of the wishes holds back here, so its clear is made in
/// its stead; otherwise the pending press would hold the other player's notifies for as long as they stay.
void SpendPressAtLeave(void* actor, Switch& s, int player) noexcept
{
    if (s.presser != player)
    {
        return;
    }
    constexpr std::uint8_t kOff = 0;
    (void)platform::WriteGuarded(static_cast<std::uint8_t*>(actor) + facts::kSwitchPressLatch, &kOff, sizeof(kOff));
    s.presser   = -1;
    s.lastLatch = 0;
    try
    {
        platform::LogDebug(L"levers: {}'s press on {} is spent with their leave (the point stays on for another player: the "
                           L"latch cleared as the game's off would)",
                           players::SeatName(player), players::NameOf(actor));
    }
    catch (...)
    {
    }
}

void LogWish(const void* actor, int player, bool wish, bool any, const wchar_t* done) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"levers: {}'s wish for the switch {} is {}; the point {}{}", players::SeatName(player),
                           players::NameOf(actor), wish ? L"on" : L"off", any ? L"on" : L"off", done);
    }
    catch (...)
    {
    }
}

/// The point setter, from the notify's decide (on or off) and the unregister (off): the caller's wish recorded for the
/// player the notify runs for, else the player whose scopes are open (the unregister; player 1's outside any), the
/// game's setter called with the OR of every player's. A player's own on call runs with their own arguments, in their
/// own context (the setter reads the primary controller's character and the party leader for the point's "not usable"
/// bit), and the bits it leaves on the point are noted as that player's view. The actor's own off calls (its tick's
/// point-off request, its EndPlay) go to the game as they are: the point comes back with the next wishing notify.
void SetterOr(void* actor, std::uint8_t show, std::uint8_t check, std::uint8_t ng) noexcept
{
    const auto original = reinterpret_cast<SetterFn>(g_setter.Original());
    const auto caller   = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (actor == nullptr || !players::generation::OnGameThread() || caller == g_tickReturn || caller == g_endPlayReturn)
    {
        original(actor, show, check, ng);
        return;
    }
    Switch* s = SwitchOf(actor, true);  // player 1's wish recorded alone too: a seat joining later ORs with it
    if (s == nullptr)
    {
        original(actor, show, check, ng);  // a switch not on the list: the game's alone
        return;
    }
    if (t_notified == kNotifiedOther)
    {
        // A character no player plays: no player's wish. The players' setter stands; with no player wishing, the game's.
        if (const int wisher = FirstOn(s->wish); wisher >= 0)
        {
            original(actor, 1, s->args[wisher].check, s->args[wisher].ng);
        }
        else
        {
            original(actor, show, check, ng);
        }
        return;
    }
    const int  player  = t_notified >= 0 ? t_notified : scopes::primary_controller::OpenSeat();
    const bool wish    = show != 0;
    const bool changed = s->wish[player] != wish;
    s->wish[player]    = wish;
    if (wish)
    {
        s->args[player] = {check, ng};
    }
    if (!AnyOn(s->wish))
    {
        original(actor, 0, check, ng);  // the last player's off: the game's own call
        s->presser   = -1;              // the off edge clears the game's latch: no press is pending
        s->lastLatch = 0;
        if (changed)
        {
            LogWish(actor, player, wish, false, L" (the game's off)");
        }
        return;
    }
    if (changed && !wish)
    {
        SpendPressAtLeave(actor, *s, player);
    }
    if (wish)
    {
        original(actor, 1, check, ng);
        std::uint8_t flags = 0;
        if (field_pickup::PointFlagsOf(s->key, flags))
        {
            s->bits[player] = flags;
        }
    }
    else
    {
        const SetterArgs& args = s->args[FirstOn(s->wish)];
        original(actor, 1, args.check, args.ng);
    }
    if (changed)
    {
        LogWish(actor, player, wish, true, wish ? L"" : L" (another player holds it)");
    }
}

/// The class's register (the trigger's enter): the game's, as the open scope's player (a seat's inside its delivery,
/// player 1 outside any), run with the in-range byte down so the byte it leaves says whether this player's register
/// took (the game sets it only when the leader is a player character); then written as the OR of the players in range.
std::uint8_t Register(void* actor) noexcept
{
    const auto original = reinterpret_cast<RegisterFn>(g_register.Original());
    Switch*    s        = actor != nullptr && players::generation::OnGameThread() ? SwitchOf(actor, true) : nullptr;
    if (s == nullptr)
    {
        return original(actor);
    }
    std::uint8_t* inRange = static_cast<std::uint8_t*>(actor) + facts::kSwitchInRange;
    constexpr std::uint8_t kDown = 0;
    if (!platform::WriteGuarded(inRange, &kDown, sizeof(kDown)))
    {
        return original(actor);
    }
    const auto result = original(actor);
    if ((s = SwitchOf(actor, false)) != nullptr)
    {
        s->inRange[scopes::primary_controller::OpenSeat()] = ReadField<std::uint8_t>(actor, facts::kSwitchInRange) != 0;
        WriteInRange(actor, *s);
    }
    return result;
}

/// The class's unregister (the trigger's leave; its point off goes through SetterOr): the game's, then the in-range
/// byte as the OR of the players still in range.
void Unregister(void* actor) noexcept
{
    reinterpret_cast<UnregisterFn>(g_unregister.Original())(actor);
    if (actor == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    if (Switch* s = SwitchOf(actor, false); s != nullptr)
    {
        s->inRange[scopes::primary_controller::OpenSeat()] = false;
        WriteInRange(actor, *s);
    }
}

/// The game's notify for `notified`'s character (0 player 1, a seat, -1 no player's): a seat's with the seat's slot
/// block held in the live manager, inside the seat's primary scope and leader answer, the player named for the setter
/// meanwhile (kNotifiedOther for a character no player plays).
void CallNotify(NotifyFn original, void* actor, void* context, int notified, std::uint64_t key) noexcept
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
        NoteNoCopy(notified, actor);
    }
    if (!input::member_movement::NotifyInPrimaryScope(notified, original, actor, context))
    {
        if (!g_scopeLogged)
        {
            g_scopeLogged = true;
            try
            {
                platform::LogWarning(L"levers: the switch {}'s notify for {}'s character ran outside their scope (it could "
                                     L"not be held): its answers are player 1's, booked to no player (said once)",
                                     players::NameOf(actor), players::SeatName(notified));
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
void NoteRise(Switch& s, int player, const void* actor, const wchar_t* how) noexcept
{
    s.presser   = player;
    s.lastLatch = 1;
    s.downSaid  = 0;
    try
    {
        platform::LogDebug(L"levers: {} pressed the switch {}{}", players::SeatName(player), players::NameOf(actor), how);
    }
    catch (...)
    {
    }
}

/// The switch's notify: the game's (CallNotify), with the press latch down for a call whose character is not the
/// presser's (lever_rules::LatchDownFor). A latch up that no seat's pass named is player 1's (a seat's press runs
/// inside its pass, which names it at once; player 1's in the game's own input step, which may come before this
/// notify and the game's update). The latch goes back up after unless the point went off inside the call (its off edge
/// clears the latch; one put back then would outlive the point and fire a press nobody made at the next on).
void Notify(void* actor, void* context) noexcept
{
    const auto original = reinterpret_cast<NotifyFn>(g_notify.Original());
    if (actor == nullptr || !players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        original(actor, context);
        return;
    }
    const int      notified = PlayerOfCharacter(ReadPointer(context, facts::kNotifyContextCharacter));
    Switch*        s        = SwitchOf(actor, true);
    const auto     key      = s != nullptr ? s->key : FirstTag(actor);
    std::uint8_t*  latch    = static_cast<std::uint8_t*>(actor) + facts::kSwitchPressLatch;
    if (s != nullptr && *latch != 0 && s->lastLatch == 0)
    {
        NoteRise(*s, 0, actor, L" (named at the first notify after the game's own input step)");
    }
    if (s != nullptr && s->presser >= 1 && !players::generation::Active(s->presser))
    {
        SpendPressAtLeave(actor, *s, s->presser);  // the presser left the game with their press pending
    }
    const int pressed = s != nullptr && *latch != 0 ? s->presser : -1;
    if (!lever_rules::LatchDownFor(pressed, notified))
    {
        CallNotify(original, actor, context, notified, key);
        return;
    }
    constexpr std::uint8_t kOff = 0, kOn = 1;
    if (!platform::WriteGuarded(latch, &kOff, sizeof(kOff)))
    {
        CallNotify(original, actor, context, notified, key);
        return;
    }
    CallNotify(original, actor, context, notified, key);
    const bool pointOn = ReadField<std::uint8_t>(actor, facts::kSwitchPointShown) != 0;
    s                  = SwitchOf(actor, false);  // the call may have made the record idle and another taken it
    if (pointOn)
    {
        (void)platform::WriteGuarded(latch, &kOn, sizeof(kOn));
    }
    if (s == nullptr)
    {
        return;
    }
    s->lastLatch = pointOn ? 1 : 0;
    s->presser   = pointOn ? s->presser : -1;
    const auto bit = static_cast<std::uint8_t>(1u << (notified >= 0 ? notified : players::kMaxPlayers));
    if ((s->downSaid & bit) != 0 || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    s->downSaid |= bit;
    try
    {
        platform::LogDebug(L"levers: the switch {}'s press is {}'s: the notify for {}'s character runs with the latch down{}",
                           players::NameOf(actor), players::SeatName(pressed),
                           notified >= 0 ? players::SeatName(notified) : L"no player",
                           pointOn ? L"" : L" (the point went off inside: the latch stays down)");
    }
    catch (...)
    {
    }
}

/// The switch executer's tick (its phase update polls the hold progress): a seat's member's with the seat's slot block
/// held in the live manager for the call, so the hold the seat's pass keeps is the one it polls. Each seat's executer
/// is logged as it starts and as its switch completes.
void ExecTick(void* executer, float dt) noexcept
{
    const auto original = reinterpret_cast<ExecTickFn>(g_execTick.Original());
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
            platform::LogDebug(L"levers: {}'s switch move on {} runs (their character {}; the switch's default start trigger {}, "
                               L"end {}: sent for the leader's type only, so not for this move)",
                               players::SeatName(player), players::NameOf(actor), players::NameOf(character),
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
        NoteNoCopy(player, actor);
    }
    original(executer, dt);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto tickSite    = image.Rebase(kSetterCallTick, 5);
    const auto endPlaySite = image.Rebase(kSetterCallEndPlay, 5);
    const auto setter      = platform::FindFunction(image, kSetter);
    // Each return address is trusted only once its site is checked as a call of the setter.
    bool ready = setter && tickSite && endPlaySite && players::call_site::Calls(*tickSite, *setter) &&
                 players::call_site::Calls(*endPlaySite, *setter);
    if (ready)
    {
        g_tickReturn    = *tickSite + 5;
        g_endPlayReturn = *endPlaySite + 5;
    }
    ready = ready && platform::InstallAtEntryOrSecond(g_setter, image, kSetter, reinterpret_cast<std::uintptr_t>(&SetterOr)) &&
            platform::ReplaceSlot(image, kRegisterSlot, reinterpret_cast<std::uintptr_t>(&Register), g_register) &&
            platform::ReplaceSlot(image, kUnregisterSlot, reinterpret_cast<std::uintptr_t>(&Unregister), g_unregister) &&
            platform::ReplaceSlot(image, kNotifySlot, reinterpret_cast<std::uintptr_t>(&Notify), g_notify) &&
            platform::ReplaceSlot(image, kExecTickSlot, reinterpret_cast<std::uintptr_t>(&ExecTick), g_execTick);
    if (!ready)
    {
        Remove();
        platform::LogWarning(L"levers: NOT placed (a slot, the setter or one of its callers is not as the research read it): "
                             L"switches stay player 1's");
        return;
    }
    g_ready = true;
    platform::Log(L"levers: ready; players 2 and 3 work the listed switches ({} of them) on their own character, with their "
                  L"own hold (the point takes every player's wish, the press latch is the presser's, the in-range byte "
                  L"every player's)",
                  lever_rules::kSwitches.size());
}

void Remove() noexcept
{
    g_ready = false;
    if (g_setter.IsInstalled())
    {
        (void)g_setter.Restore();
    }
    for (platform::VTableEntry* slot : {&g_notify, &g_register, &g_unregister, &g_execTick})
    {
        if (slot->Original() != 0)
        {
            (void)slot->Restore();
        }
    }
    g_switches.fill({});
    g_keys.fill(0);
    g_refused.fill(0);
    g_execSeen.fill(nullptr);
    g_noCopySaid.fill(false);
    g_fullLogged = g_scopeLogged = false;
}

void Tick() noexcept
{
    if (!g_ready || !players::generation::OnGameThread())
    {
        return;
    }
    for (Switch& s : g_switches)
    {
        void* actor = s.actor != 0 ? players::weak_object::ObjectOf(s.actor) : nullptr;
        if (actor == nullptr)
        {
            s = {};
            continue;
        }
        bool wished = false, marked = false;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if (players::generation::Active(seat))
            {
                continue;
            }
            wished             = wished || s.wish[seat];
            marked             = marked || s.inRange[seat];
            s.wish[seat]       = false;
            s.inRange[seat]    = false;
            s.args[seat]       = {};
            s.bits[seat]       = 0;
            s.view[seat]       = PointView::NotTracked;
            s.viewLogged[seat] = PointView::NotTracked;
            if (s.presser == seat)
            {
                SpendPressAtLeave(actor, s, seat);
            }
        }
        if (marked)
        {
            WriteInRange(actor, s);
        }
        if (wished && !AnyOn(s.wish) && ReadField<std::uint8_t>(actor, facts::kSwitchPointShown) != 0)
        {
            reinterpret_cast<SetterFn>(g_setter.Original())(actor, 0, 0, 0);
            try
            {
                platform::LogDebug(L"levers: the switch {}'s point is off: the player who wished it on left the game",
                                   players::NameOf(actor));
            }
            catch (...)
            {
            }
        }
    }
}

bool Ready() noexcept
{
    return g_ready;
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

void NoteSwitch(const void* actor) noexcept
{
    if (!g_ready || actor == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    (void)SwitchOf(actor, true);
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
    g_nextKey                   = (g_nextKey + 1) % kMaxSwitches;
    try
    {
        platform::LogDebug(L"levers: the switch {} is delivered: its point is a player 2's or 3's press from here on{}",
                           players::NameOf(actor),
                           dropped != 0 ? std::format(L" (the oldest noted switch {} forgotten)", players::unreal::NameText(dropped))
                                        : std::wstring());
    }
    catch (...)
    {
    }
}

bool AdmitsPoint(std::uint64_t key) noexcept
{
    if (!g_ready || key == 0)
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
    if (!g_ready || !players::generation::OnGameThread())
    {
        return;
    }
    for (Switch& s : g_switches)
    {
        const void* actor = s.actor != 0 ? players::weak_object::ObjectOf(s.actor) : nullptr;
        if (actor == nullptr)
        {
            continue;
        }
        const std::uint8_t latch = ReadField<std::uint8_t>(actor, facts::kSwitchPressLatch);
        if (latch != 0 && s.lastLatch == 0)
        {
            NoteRise(s, player, actor, L"");
        }
        else if (latch == 0)
        {
            s.presser = -1;
        }
        s.lastLatch = latch;
    }
}

PointView PointViewFor(std::uint64_t key, int player) noexcept
{
    if (!g_ready || key == 0 || player < 0 || player >= static_cast<int>(players::kMaxPlayers) ||
        !players::generation::OnGameThread())
    {
        return PointView::NotTracked;
    }
    for (Switch& s : g_switches)
    {
        if (s.actor == 0 || s.key != key || !AnyOn(s.wish))
        {
            continue;
        }
        const PointView view = !s.wish[player]                      ? PointView::Off
                               : (s.bits[player] & facts::kFieldActionPointNotUsable) != 0 ? PointView::NotUsable
                                                                     : PointView::Usable;
        s.view[player] = view;
        return view;
    }
    return PointView::NotTracked;
}

void LogPointViews() noexcept
{
    if (!g_ready || !players::generation::OnGameThread() || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    for (Switch& s : g_switches)
    {
        for (int player = 0; player < static_cast<int>(players::kMaxPlayers); ++player)
        {
            const PointView view = s.view[player];
            if (view == PointView::NotTracked || view == s.viewLogged[player])
            {
                continue;
            }
            s.viewLogged[player] = view;
            try
            {
                platform::LogDebug(L"levers: {}'s selection sees the switch {} {}", players::SeatName(player),
                                   players::NameOf(players::weak_object::ObjectOf(s.actor)),
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
