#include "traversal/hooks.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/traversal.h"
#include "feedback/traversal_rumble.h"
#include "input/climb_context.h"
#include "input/member_movement.h"
#include "platform/call_redirect.h"
#include "platform/entry_install.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"
#include "scopes/leader_answer.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "session/party_member.h"
#include "traversal/traversal_shared.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>

namespace lss::traversal::hooks {

namespace {

using traversal_camera::kMoveActor;
using traversal_camera::kMoveCharacter;
using traversal_camera::SeatOfCharacter;
using world::field_rules::AnyOn;
using world::field_rules::FirstOn;
using world::field_rules::PointView;

// The gun's functions, slots and call sites (research/RE-hooks-for-seats.md).
constexpr std::uint8_t kShotPointBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
                                            0x89, 0x78, 0x18, 0x4C, 0x89, 0x60, 0x20, 0x41, 0x55, 0x41, 0x56, 0x41};
constexpr platform::FunctionFact kShotPoint{L"HookShotChild_SetFieldActionMgrPoint_ReadsLeader", 0x143645008, kShotPointBytes};
// Its three callers: the notify's two (on and off) and the unregister's (off), each five bytes inside one block. The
// actor's EndPlay is a fourth caller, left alone: an unconditional off for an actor leaving play.
constexpr std::uintptr_t kShotPointCalls[] = {0x143642D07, 0x143642D88, 0x143642985};
// The pin's removal from the primary controller's leader-action component, by the point's key: what the setter's off
// branch calls.
constexpr std::uint8_t kRemovePinBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x1C, 0xE2, 0x98, 0xFE,
                                            0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kRemovePin{L"FieldActionPoint_RemoveLeaderActionInteractPoint1490_PrimaryPC", 0x143720D08,
                                            kRemovePinBytes};
constexpr std::uint8_t kShotInstallBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
                                              0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0xB8, 0x70, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kShotInstallSlot{L"the gun point's camera installer slot", 0x1466F1888,
                                              {L"HookShotChild_InstallCameraModule_PerActor", 0x14364610C, kShotInstallBytes}};
constexpr std::uint8_t kShotLeaveBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48,
                                            0x8B, 0xEC, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kShotLeaveSlot{L"the gun point's camera leave slot", 0x1466F1890,
                                            {L"HookShotChild_LeaveCameraModule", 0x143646E60, kShotLeaveBytes}};
constexpr std::uint8_t kShotBeginBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kShotBeginSlot{L"the hookshot move's Begin slot", 0x146694898,
                                            {L"HookShotMove_Begin_Vt98", 0x1432C3CA0, kShotBeginBytes}};
constexpr std::uint8_t kShotEndBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                          0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x60, 0x00, 0x00};
constexpr platform::SlotFact kShotEndSlot{L"the hookshot move's End slot", 0x146694920,
                                          {L"HookShotMove_End_Vt120_CameraOff_MarkFinished", 0x1432BE76C, kShotEndBytes}};
// The hit effect's entry crosses a block in both builds: the redirect goes at the second instruction (a store into the
// caller's home area that no branch lands on).
constexpr std::uint8_t kHitBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
                                      0x7C, 0x24, 0x18, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x50, 0x01};
constexpr platform::FunctionFact kHit{L"HookShotChild_HitFFB_Hookshot0", 0x143D6500C, kHitBytes};
// The press flag path (research/RE-hook-follow.md): the point's press listener sets the actor's one flag (its entry and
// its second instruction both cross a block, so the flag is watched after each selection update instead), and every
// holder's notify of that frame starts the hookshot on its own character while it is set.
constexpr std::uint8_t kShotNotifyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
                                             0x48, 0x8D, 0x6C, 0x24};
constexpr platform::SlotFact kShotNotifySlot{L"the gun point's notify slot", 0x1466F16E0,
                                             {L"HookShotChild_Vt748_NotifyNear", 0x143642BA4, kShotNotifyBytes}};
// The follower path: the AI world manager's queue reaches this slot for a follower the trail-follow step chose; the
// game's only notion of a follower there is a character an AI controller possessed, a seat's member included.
constexpr std::uint8_t kShotAIEnterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
                                              0x70, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kShotAIEnterSlot{L"the gun point's follower-enter slot", 0x1466F1648,
                                              {L"HookShotChild_Vt6B0_AIEnter_RequestState8", 0x143643E98, kShotAIEnterBytes}};

// The rope's (the same source). Its switch's entry crosses a block in one build only; the same second-instruction
// placement, checked the same way.
constexpr std::uint8_t kRopeActiveBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41,
                                             0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x00, 0x01, 0x00, 0x00};
constexpr platform::FunctionFact kRopeActive{L"HookRope_SetActive_PointAndAnim", 0x14368064C, kRopeActiveBytes};
constexpr std::uint8_t kRopeInstallBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48,
                                              0x8B, 0xEC, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kRopeInstallSlot{L"the hook rope's camera installer slot", 0x145884C50,
                                              {L"HookRope_InstallGlobalModule", 0x143681EE0, kRopeInstallBytes}};
constexpr std::uint8_t kRopeLeaveBytes[] = {0xB8, 0x38, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kRopeLeaveSlot{L"the hook rope's camera leave slot", 0x145884C58,
                                            {L"HookRope_LeaveGlobalModule", 0x143682514, kRopeLeaveBytes}};
constexpr std::uint8_t kRopeBeginBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                            0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr platform::SlotFact kRopeBeginSlot{L"the hook rope move's Begin slot", 0x146694B18,
                                            {L"HookRopeAction_CameraOnIfUCPC", 0x1432C3758, kRopeBeginBytes}};
constexpr std::uint8_t kRopeEndBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
                                          0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xB0, 0x00, 0x00};
constexpr platform::SlotFact kRopeEndSlot{L"the hook rope move's End slot", 0x146694BA0,
                                          {L"HookRopeMove_End_Vt120_CameraOff", 0x1432BE130, kRopeEndBytes}};

constexpr std::size_t kHandleSize = 16;  // a camera-module handle: the module, then its weak reference controller
constexpr std::size_t kMaxActors  = 8;   // gun points and ropes with recorded wishes or handles at once
constexpr float       kOffRadius  = -1.0f;  // the game's own radius arguments of an off call

using PointFn     = void (*)(void* actor, std::uint8_t show, std::uint8_t allAngle, std::uint8_t ng, float r1, float r2);
using RemovePinFn = void (*)(std::uint64_t key);
using ActiveFn    = void (*)(void* actor, std::uint8_t on, void* character, std::uint8_t ngAllowed);
using MoveFn      = std::uintptr_t (*)(void* move);
using EndFn       = std::uintptr_t (*)(void* move, std::uint8_t abort);
using HandleFn    = std::uintptr_t (*)(void* actor);
using HitFn       = std::uintptr_t (*)(void* gun, std::uint8_t kind);
using NotifyFn    = void (*)(void* actor, void* context);
using AIEnterFn   = std::uint8_t (*)(void* actor, void* character);
using Handle      = std::array<std::uint8_t, kHandleSize>;

/// Who pressed a gun point last: 0 player 1, a seat, -1 unknown; and the press flag as last looked at, so a flag that
/// came up since names the update that dispatched the press.
struct Presser
{
    std::uint64_t actor    = 0;  // the actor's weak value
    int           player   = -1;
    std::uint8_t  lastFlag = 0;
};

/// The gun point setter's arguments after `show`, as a wishing player's call last passed them (the game's switch
/// takes a wishing player's, not a leaving one's).
struct PointArgs
{
    std::uint8_t allAngle = 0;
    std::uint8_t ng       = 0;
    float        r1       = kOffRadius;
    float        r2       = kOffRadius;
};
/// The players' wishes for an actor's point: on or off, as each player's own call last passed it.
struct Wishes
{
    std::uint64_t actor = 0;  // the actor's weak value
    bool          wish[players::kMaxPlayers] = {};
    PointArgs     args[players::kMaxPlayers] = {};
    PointView  view[players::kMaxPlayers] = {};        // each player's view as their selection last asked it
    PointView  viewLogged[players::kMaxPlayers] = {};  // as last said
};
/// A seat's copy of a gun point's camera handle, per actor.
struct ActorHandle
{
    std::uint64_t actor = 0;
    Handle        bytes{};
};

std::array<platform::CallRedirect, std::size(kShotPointCalls)> g_shotPointCalls;
PointFn                 g_shotPoint = nullptr;
RemovePinFn             g_removePin = nullptr;
platform::VTableEntry   g_shotInstall, g_shotLeave, g_shotBegin, g_shotEnd, g_shotNotify, g_shotAIEnter;
platform::EntryRedirect g_hit;
platform::EntryRedirect g_ropeActive;
platform::VTableEntry   g_ropeInstall, g_ropeLeave, g_ropeBegin, g_ropeEnd;
std::uintptr_t          g_ropeCamera = 0;  // the rope's global camera handle, live
bool                    g_gun = false, g_rope = false;

// Game thread.
std::array<Wishes, kMaxActors>        g_shotWishes{}, g_ropeWishes{};
std::array<Presser, kMaxActors>       g_pressers{};  // each gun point's last presser
bool                                  g_pressFullLogged = false;
bool                                  g_notifyScopeLogged = false;
constexpr int                         kNotifiedNone    = -1;  // no notify runs (the unregister: the open scope's player)
constexpr int                         kNotifiedOther   = -2;  // a notify for a character no player plays
thread_local int                      t_notified       = kNotifiedNone;  // else the player whose character's notify runs
std::array<std::uint64_t, kMaxActors> g_ropeKeys{};  // delivered ropes' point keys
std::size_t                           g_nextRopeKey    = 0;
const std::uint64_t*                  g_shotPreset     = nullptr;  // the FName globals of the gun point's presets, live
const std::uint64_t*                  g_shotPlayerPreset = nullptr;
thread_local int                      t_seat           = 0;  // the seat whose move's Begin or End this thread runs (0: none)

// Any thread: each seat's copies of the gun points' camera handles, and of the rope's global one, under one lock (the
// installer and leave run on the game thread or the animation worker; a leave reached from inside an installer takes
// it again).
std::recursive_mutex                                                   g_handles;
std::array<std::array<ActorHandle, kMaxActors>, players::kMaxPlayers> g_shotHandles{};
std::array<Handle, players::kMaxPlayers>                              g_ropeHandles{};

/// The seat this thread works for: the move wrapper's, else the open scope's on the game thread, else the climb
/// context's on the worker; 0 for player 1 or none.
int SeatNow() noexcept
{
    if (players::generation::OnGameThread())
    {
        return t_seat != 0 ? t_seat : scopes::primary_controller::OpenSeat();
    }
    const int seat = input::climb_context::SeatHere();
    return players::SeatBuilt(seat) ? seat : 0;
}

/// The wishes of `actor`, with a seat that left the game wishing nothing (its wish was never taken back by a call):
/// its entry, else a free, dead or wishless one made its (an actor gone from play, or one nobody wishes on, gives
/// its entry up; an actor's EndPlay switches its point off outside the redirects and leaves its wishes to this),
/// else null when the table is full or the actor has no weak value (the game's own call then, logged once).
Wishes* WishesOf(std::array<Wishes, kMaxActors>& table, void* actor) noexcept
{
    const std::uint64_t weak = players::weak_object::WeakOf(actor);
    if (weak == 0)
    {
        return nullptr;
    }
    for (Wishes& wishes : table)
    {
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            wishes.wish[seat] = wishes.wish[seat] && players::generation::Active(seat);
        }
    }
    for (Wishes& wishes : table)
    {
        if (wishes.actor == weak)
        {
            return &wishes;
        }
    }
    for (Wishes& wishes : table)
    {
        if (wishes.actor == 0 || !AnyOn(wishes.wish) || players::weak_object::ObjectOf(wishes.actor) == nullptr)
        {
            wishes = {weak};
            return &wishes;
        }
    }
    return nullptr;
}

/// A wishing player's character: player 1's pawn, or the seat's member.
void* CharacterOf(int wisher) noexcept
{
    return wisher == 0 ? players::actor_fields::Pawn(players::generation::PlayerOneController())
                       : players::generation::MemberOf(wisher);
}

/// The presser's entry for `actor`, made (in a free or dead entry) when `make`; null when none, or the table is full.
Presser* PresserOf(void* actor, bool make) noexcept
{
    const std::uint64_t weak = players::weak_object::WeakOf(actor);
    if (weak == 0)
    {
        return nullptr;
    }
    for (Presser& presser : g_pressers)
    {
        if (presser.actor == weak)
        {
            return &presser;
        }
    }
    if (!make)
    {
        return nullptr;
    }
    for (Presser& presser : g_pressers)
    {
        if (presser.actor == 0 || (presser.player < 0 && presser.lastFlag == 0) ||
            players::weak_object::ObjectOf(presser.actor) == nullptr)
        {
            presser = {weak, -1, 0};
            return &presser;
        }
    }
    return nullptr;
}

/// A press flag seen up for the first time: `player` pressed (`how` ends the line).
void NoteRise(Presser& presser, int player, const void* actor, const wchar_t* how) noexcept
{
    presser.player   = player;
    presser.lastFlag = 1;
    try
    {
        platform::LogDebug(L"hooks: {} pressed the gun point {}{}", players::SeatName(player), players::NameOf(actor), how);
    }
    catch (...)
    {
    }
}

/// A presser whose wish goes off while the point stays on for another player (their character flew, or they walked
/// away, or they left the game): the game's off path, which clears the press flag, is what the OR of the wishes holds
/// back here, so its clear is made in its stead; otherwise the pending press would hold the other player's notifies
/// for as long as they stay.
void SpendPressAtLeave(void* actor, int player) noexcept
{
    Presser* presser = PresserOf(actor, false);
    if (presser == nullptr || presser->player != player)
    {
        return;
    }
    constexpr std::uint8_t kOff = 0;
    (void)platform::WriteGuarded(static_cast<std::uint8_t*>(actor) + facts::kHookShotPressFlag, &kOff, sizeof(kOff));
    *presser = {presser->actor, -1, 0};
    try
    {
        platform::LogDebug(L"hooks: {}'s press on the gun point {} is spent with their leave (the point stays on for another "
                           L"player: the flag cleared as the game's off would)",
                           players::SeatName(player), players::NameOf(actor));
    }
    catch (...)
    {
    }
}

/// The game's notify for `notified`'s character (0 player 1, a seat, -1 no player's): a seat's inside the seat's
/// primary scope and leader answer, the player named for the switch meanwhile (kNotifiedOther for a character no
/// player plays: its answer is no player's wish). The pass that calls it runs in the character's own tick, outside
/// every scope; without them the pin would go to player 1's component and the wish be booked to player 1.
void CallNotify(NotifyFn original, void* actor, void* context, int notified) noexcept
{
    const int saved = t_notified;
    t_notified      = notified >= 0 ? notified : kNotifiedOther;
    if (notified < 1)
    {
        original(actor, context);
    }
    else if (!input::member_movement::NotifyInPrimaryScope(notified, original, actor, context))
    {
        if (!g_notifyScopeLogged)
        {
            g_notifyScopeLogged = true;
            try
            {
                platform::LogWarning(L"hooks: the gun point {}'s notify for {}'s character ran outside their scope (it could "
                                     L"not be held): its compass pin goes to player 1's (said once)",
                                     players::NameOf(actor), players::SeatName(notified));
            }
            catch (...)
            {
            }
        }
        original(actor, context);
    }
    t_notified = saved;
}

/// The gun point's notify: the game's (CallNotify), with the press flag at 0 for a call whose character is another
/// player's than the presser's. The flag has no owner in the game and every holder's notify of the press frame would
/// start the hookshot on its own character; at 0 for that one call, only the presser's starts. A flag up that no
/// seat's pass named is player 1's (a seat's press runs inside its pass, which names it at once; player 1's in the
/// game's own input step, which may come before this notify and the game's update). The flag goes back after unless
/// the game took the point off inside the call (its off path clears the flag with the listener handle; a flag put
/// back then would outlive the point and fire a press nobody made at the next on).
void ShotNotify(void* actor, void* context) noexcept
{
    const auto original = reinterpret_cast<NotifyFn>(g_shotNotify.Original());
    if (!players::generation::OnGameThread())
    {
        original(actor, context);
        return;
    }
    const int notified = players::generation::PlayerOfCharacter(ReadPointer(context, facts::kNotifyContextCharacter));
    Presser*  presser  = nullptr;
    if (ReadField<std::uint8_t>(actor, facts::kHookShotPressFlag) != 0)
    {
        presser = PresserOf(actor, true);
        if (presser != nullptr && presser->lastFlag == 0)
        {
            NoteRise(*presser, 0, actor, L" (named at the first notify after the game's own input step)");
        }
    }
    if (presser != nullptr && presser->player >= 1 && !players::generation::Active(presser->player))
    {
        SpendPressAtLeave(actor, presser->player);  // the presser left the game with their press pending
    }
    const bool known   = presser != nullptr && presser->player >= 0;
    const int  pressed = known ? presser->player : -1;  // kept for the line below: the entry may be reset inside
    if (!known || notified < 0 || notified == pressed)
    {
        CallNotify(original, actor, context, notified);
        return;
    }
    std::uint8_t*          flag = static_cast<std::uint8_t*>(actor) + facts::kHookShotPressFlag;
    constexpr std::uint8_t kOff = 0, kOn = 1;
    if (!platform::WriteGuarded(flag, &kOff, sizeof(kOff)))
    {
        CallNotify(original, actor, context, notified);
        return;
    }
    CallNotify(original, actor, context, notified);
    const bool pointOn = ReadField<std::uint64_t>(actor, facts::kHookShotListener) != 0;
    if (pointOn)
    {
        (void)platform::WriteGuarded(flag, &kOn, sizeof(kOn));
        presser->lastFlag = 1;
    }
    else
    {
        *presser = {presser->actor, -1, 0};
    }
    try
    {
        platform::LogDebug(L"hooks: the gun point {}'s press is {}'s: the notify for {}'s character ran with the flag off{}",
                           players::NameOf(actor), players::SeatName(pressed), players::SeatName(notified),
                           pointOn ? L"" : L" (the point went off inside: the flag stays off)");
    }
    catch (...)
    {
    }
}

/// The gun point's follower enter: the game's for an AI member, refused for a player's member (a player's character
/// follows nobody; 0 is the game's own answer for a character with no controller holder).
std::uint8_t ShotAIEnter(void* actor, void* character) noexcept
{
    const int player = players::generation::OnGameThread() ? players::generation::PlayerOfCharacter(character) : -1;
    if (player < 1)
    {
        return reinterpret_cast<AIEnterFn>(g_shotAIEnter.Original())(actor, character);
    }
    try
    {
        platform::LogDebug(L"hooks: the game's follower path asked {}'s member to take the gun point {}: refused (a player's "
                           L"member follows nobody)",
                           players::SeatName(player), players::NameOf(actor));
    }
    catch (...)
    {
    }
    return 0;
}

void LogWish(const wchar_t* what, const void* actor, int player, bool wish, bool any, const wchar_t* done) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"hooks: {}'s wish for the {} {} is {}; the point {}{}", players::SeatName(player), what,
                           players::NameOf(actor), wish ? L"on" : L"off", any ? L"on" : L"off", done);
    }
    catch (...)
    {
    }
}

void LogFull(const wchar_t* what, const void* actor, bool& logged) noexcept
{
    if (logged)
    {
        return;
    }
    logged = true;
    try
    {
        platform::LogWarning(L"hooks: no room to record a wish for the {} {}: the game's own call stands for it (said once)",
                             what, players::NameOf(actor));
    }
    catch (...)
    {
    }
}
bool g_shotFullLogged = false, g_ropeFullLogged = false;

/// The gun point's switch, from the notify (on or off) and the unregister (off): the caller's wish recorded for the
/// player the notify runs for, else the player whose scopes are open (the unregister; player 1's outside any), the
/// game's switch called with the OR of every player's. An on call passes the first wishing player's answer (player
/// 1's while they wish): the setter writes the point's "not usable from here" bit from it, and each player's
/// selection sees their own (GunPointViewFor). The game adds the caller's compass pin only as the point goes from
/// down to up and takes it down with the point, so at a player's own change while the point stays up their pin is
/// made or removed in their own context: a player joining a point another holds gets a down and an up of the game's
/// setter (their pin made; the other's untouched, it hangs on the other's controller), a player leaving one gets the
/// game's own removal of their pin.
void ShotPointOr(void* actor, std::uint8_t show, std::uint8_t allAngle, std::uint8_t ng, float r1, float r2) noexcept
{
    Wishes* wishes = actor != nullptr && players::generation::OnGameThread() ? WishesOf(g_shotWishes, actor) : nullptr;
    if (wishes == nullptr)
    {
        if (actor != nullptr && players::generation::OnGameThread())
        {
            LogFull(L"gun point", actor, g_shotFullLogged);
        }
        g_shotPoint(actor, show, allAngle, ng, r1, r2);
        return;
    }
    if (t_notified == kNotifiedOther)
    {
        // A character no player plays: no player's wish. The players' switch stands (the first wisher's answer); with
        // no player wishing, the game's own call.
        if (AnyOn(wishes->wish))
        {
            const PointArgs& args = wishes->args[FirstOn(wishes->wish)];
            g_shotPoint(actor, 1, args.allAngle, args.ng, args.r1, args.r2);
        }
        else
        {
            g_shotPoint(actor, show, allAngle, ng, r1, r2);
        }
        return;
    }
    const int  player  = t_notified >= 0 ? t_notified : scopes::primary_controller::OpenSeat();
    const bool wish    = show != 0;
    const bool wasAny  = AnyOn(wishes->wish);
    const bool changed = wishes->wish[player] != wish;
    wishes->wish[player] = wish;
    if (wish)
    {
        wishes->args[player] = {allAngle, ng, r1, r2};
    }
    const bool any = AnyOn(wishes->wish);
    if (!any)
    {
        g_shotPoint(actor, 0, allAngle, ng, r1, r2);  // the last player's off: the game's own call
        if (Presser* presser = PresserOf(actor, false); presser != nullptr)
        {
            *presser = {presser->actor, -1, 0};  // the off clears the game's press flag: no press is pending
        }
        LogWish(L"gun point", actor, player, wish, any, changed ? L" (the game's off)" : L"");
        return;
    }
    if (changed && !wish)
    {
        SpendPressAtLeave(actor, player);
    }
    const PointArgs& args = wishes->args[FirstOn(wishes->wish)];
    if (changed && wish && wasAny)
    {
        if (ReadField<std::uint8_t>(actor, facts::kHookShotPressFlag) != 0)
        {
            try
            {
                platform::LogDebug(L"hooks: {} joins the gun point {} with its press flag up: the game's down clears it",
                                   players::SeatName(player), players::NameOf(actor));
            }
            catch (...)
            {
            }
        }
        g_shotPoint(actor, 0, 0, 0, kOffRadius, kOffRadius);
        g_shotPoint(actor, 1, args.allAngle, args.ng, args.r1, args.r2);
        LogWish(L"gun point", actor, player, wish, any, L" (down and up for their own pin: another player held it)");
        return;
    }
    g_shotPoint(actor, 1, args.allAngle, args.ng, args.r1, args.r2);
    if (changed && !wish)
    {
        g_removePin(players::actor_fields::FirstTag(actor));
        LogWish(L"gun point", actor, player, wish, any, L" (their own pin removed: another player holds it)");
        return;
    }
    LogWish(L"gun point", actor, player, wish, any, L"");
}

/// The rope's switch (the notify, the unregister's tail jump, a third caller): the player is the character's seat,
/// or the open scope's for a call without one (the leave); the game's switch gets the OR, with a wishing player's
/// character when the caller passed none. The rope's point carries no compass pin.
void RopeActiveOr(void* actor, std::uint8_t on, void* character, std::uint8_t ngAllowed) noexcept
{
    const auto original = reinterpret_cast<ActiveFn>(g_ropeActive.Original());
    Wishes*    wishes   = actor != nullptr && players::generation::OnGameThread() ? WishesOf(g_ropeWishes, actor) : nullptr;
    if (wishes == nullptr)
    {
        if (actor != nullptr && players::generation::OnGameThread())
        {
            LogFull(L"rope", actor, g_ropeFullLogged);
        }
        original(actor, on, character, ngAllowed);
        return;
    }
    int player = SeatOfCharacter(character);
    if (player < 0)
    {
        player = character == nullptr ? scopes::primary_controller::OpenSeat() : 0;  // no character (the leave): the open scope's; else player 1's
    }
    const bool wish    = on != 0;
    const bool changed = wishes->wish[player] != wish;
    wishes->wish[player] = wish;
    const bool any    = AnyOn(wishes->wish);
    const int  wisher = FirstOn(wishes->wish);
    // An on forwarded for another player's wish carries that player's character, not the leaving caller's.
    void* who = wish && character != nullptr ? character : wisher >= 0 ? CharacterOf(wisher) : character;
    original(actor, any ? 1 : 0, who, ngAllowed);
    LogWish(L"rope", actor, player, wish, any, changed ? L"" : L" (unchanged)");
}

// ---- the moves' Begin and End ----

struct MoveCall
{
    MoveFn         fn;
    void*          move;
    std::uintptr_t result = 0;
};

void RunMove(void* context) noexcept
{
    auto& call  = *static_cast<MoveCall*>(context);
    call.result = call.fn(call.move);
}

struct EndCall
{
    EndFn          fn;
    void*          move;
    std::uint8_t   abort;
    std::uintptr_t result = 0;
};

void RunEnd(void* context) noexcept
{
    auto& call  = *static_cast<EndCall*>(context);
    call.result = call.fn(call.move, call.abort);
}

void LogMove(const wchar_t* what, const wchar_t* step, int seat, const void* move, bool scoped, bool leaderHeld) noexcept
{
    if (!scoped || !leaderHeld)
    {
        platform::LogWarning(L"hooks: {}'s {} {} with {} refused: the game's answer (player 1's) stands there",
                             players::SeatName(seat), what, step, !scoped ? L"their primary scope" : L"the party leader's scope");
        return;
    }
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"hooks: {}'s {} {} on {} as {}", players::SeatName(seat), what, step,
                           players::NameOf(players::weak_object::ObjectIn(move, kMoveActor)), players::SeatName(seat));
    }
    catch (...)
    {
    }
}

/// A move's Begin: a seat's member's runs inside the seat's scopes (the rope's with the member as the party leader
/// too), the seat named for the camera handle swaps meanwhile.
std::uintptr_t Begin(const wchar_t* what, const platform::VTableEntry& slot, void* move, bool leader) noexcept
{
    const auto original = reinterpret_cast<MoveFn>(slot.Original());
    const int  seat     = players::generation::OnGameThread()
                              ? SeatOfCharacter(players::weak_object::ObjectIn(move, kMoveCharacter))
                              : -1;
    if (seat < 1)
    {
        return original(move);
    }
    void*    member = players::generation::MemberOf(seat);
    MoveCall call{original, move};
    bool     scoped     = false;
    bool     leaderHeld = true;
    {
        const int saved = t_seat;
        t_seat          = seat;
        std::optional<scopes::leader_scope::LeaderScope> leaderScope;
        if (leader)
        {
            leaderScope.emplace(scopes::leader_scope::WeakOf(member));
            leaderHeld = leaderScope->Held();
        }
        scoped = input::member_movement::InPrimaryScope(seat, member, &RunMove, &call);
        leaderScope.reset();
        if (!scoped)
        {
            call.result = original(move);  // the game's own call, in neither scope
        }
        t_seat = saved;
    }
    LogMove(what, L"begins", seat, move, scoped, leaderHeld);
    return call.result;
}

/// A move's End: on the game thread (an abort) a seat's runs inside the seat's scopes; on the worker (the finish,
/// inside the climb context) the game's call with the seat named for the handle swap.
std::uintptr_t End(const wchar_t* what, const platform::VTableEntry& slot, void* move, std::uint8_t abort) noexcept
{
    const auto original = reinterpret_cast<EndFn>(slot.Original());
    if (!players::generation::OnGameThread())
    {
        return original(move, abort);
    }
    const int seat = SeatOfCharacter(players::weak_object::ObjectIn(move, kMoveCharacter));
    if (seat < 1)
    {
        return original(move, abort);
    }
    EndCall call{original, move, abort};
    bool    scoped = false;
    {
        const int saved = t_seat;
        t_seat          = seat;
        scoped          = input::member_movement::InPrimaryScope(seat, players::generation::MemberOf(seat), &RunEnd, &call);
        if (!scoped)
        {
            call.result = original(move, abort);
        }
        t_seat = saved;
    }
    LogMove(what, abort != 0 ? L"ends (aborted)" : L"ends (finished)", seat, move, scoped, true);
    return call.result;
}

std::uintptr_t ShotBegin(void* move) noexcept
{
    return Begin(L"hookshot", g_shotBegin, move, false);
}

std::uintptr_t ShotEnd(void* move, std::uint8_t abort) noexcept
{
    return End(L"hookshot", g_shotEnd, move, abort);
}

std::uintptr_t RopeBegin(void* move) noexcept
{
    return Begin(L"rope swing", g_ropeBegin, move, true);
}

std::uintptr_t RopeEnd(void* move, std::uint8_t abort) noexcept
{
    return End(L"rope swing", g_ropeEnd, move, abort);
}

// ---- the camera handles ----

bool HoldsModule(const Handle& handle) noexcept
{
    std::uint64_t module = 0;
    std::memcpy(&module, handle.data(), sizeof(module));
    return module != 0;
}

/// The seat's copy of `actor`'s gun handle: its entry, else a free, dead or empty one made its (a handle holding no
/// module is as good as new); null when the table is full or the actor has no weak value. Under g_handles.
Handle* ShotHandleOf(int seat, void* actor) noexcept
{
    auto&               mine = g_shotHandles[static_cast<std::size_t>(seat)];
    const std::uint64_t weak = players::weak_object::WeakOf(actor);
    if (weak == 0)
    {
        return nullptr;
    }
    for (ActorHandle& handle : mine)
    {
        if (handle.actor == weak)
        {
            return &handle.bytes;
        }
    }
    for (ActorHandle& handle : mine)
    {
        if (handle.actor == 0 || !HoldsModule(handle.bytes) || players::weak_object::ObjectOf(handle.actor) == nullptr)
        {
            handle = {weak, {}};
            return &handle.bytes;
        }
    }
    return nullptr;
}

/// The handle at `at` (an actor's field or the rope's global) swapped with the seat's copy around the game's call.
std::uintptr_t WithHandle(std::uint8_t* at, Handle& mine, HandleFn fn, void* actor) noexcept
{
    Handle theirs{};
    std::memcpy(theirs.data(), at, kHandleSize);
    std::memcpy(at, mine.data(), kHandleSize);
    const auto result = fn(actor);
    std::memcpy(mine.data(), at, kHandleSize);
    std::memcpy(at, theirs.data(), kHandleSize);
    return result;
}

const wchar_t* HandleText(const Handle& handle) noexcept
{
    return HoldsModule(handle) ? L"a module" : L"none";
}

void LogHandle(const wchar_t* what, const wchar_t* step, int seat, const void* actor, const Handle& before,
               const Handle& after) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"hooks: {}'s {} camera {} on {} ({} thread): their own handle {} -> {}", players::SeatName(seat),
                           what, step, players::NameOf(actor),
                           players::generation::OnGameThread() ? L"game" : L"animation", HandleText(before), HandleText(after));
    }
    catch (...)
    {
    }
}

void LogNoHandle(const wchar_t* what, int seat, const void* actor) noexcept
{
    try
    {
        platform::LogWarning(L"hooks: no room for {}'s own {} camera handle on {}: the game's call runs on the actor's",
                             players::SeatName(seat), what, players::NameOf(actor));
    }
    catch (...)
    {
    }
}

/// A gun point's installer or leave: every call under the lock (player 1's reads the actor's field a seat's call
/// swaps); a seat's with the seat's own copy of the actor's handle in place.
std::uintptr_t ShotHandleCall(const wchar_t* step, const platform::VTableEntry& slot, void* actor) noexcept
{
    const auto original = reinterpret_cast<HandleFn>(slot.Original());
    if (actor == nullptr)
    {
        return original(actor);
    }
    const std::lock_guard<std::recursive_mutex> lock(g_handles);
    const int seat = SeatNow();
    Handle*   mine = seat >= 1 ? ShotHandleOf(seat, actor) : nullptr;
    if (seat >= 1 && mine == nullptr)
    {
        LogNoHandle(L"hookshot", seat, actor);
    }
    if (mine == nullptr)
    {
        return original(actor);
    }
    const Handle before = *mine;
    const auto   result = WithHandle(static_cast<std::uint8_t*>(actor) + facts::kHookShotCameraHandle, *mine, original, actor);
    LogHandle(L"hookshot", step, seat, actor, before, *mine);
    return result;
}

std::uintptr_t ShotInstall(void* actor) noexcept
{
    return ShotHandleCall(L"installs", g_shotInstall, actor);
}

std::uintptr_t ShotLeave(void* actor) noexcept
{
    return ShotHandleCall(L"leaves", g_shotLeave, actor);
}

/// A rope's installer or leave: every call under the lock; a seat's with the seat's copy of the global handle in
/// place.
std::uintptr_t RopeHandleCall(const wchar_t* step, const platform::VTableEntry& slot, void* actor) noexcept
{
    const auto original = reinterpret_cast<HandleFn>(slot.Original());
    const std::lock_guard<std::recursive_mutex> lock(g_handles);
    const int seat = g_ropeCamera != 0 ? SeatNow() : 0;
    if (seat < 1)
    {
        return original(actor);
    }
    Handle&      mine   = g_ropeHandles[static_cast<std::size_t>(seat)];
    const Handle before = mine;
    const auto   result = WithHandle(reinterpret_cast<std::uint8_t*>(g_ropeCamera), mine, original, actor);
    LogHandle(L"rope swing", step, seat, actor, before, mine);
    return result;
}

std::uintptr_t RopeInstall(void* actor) noexcept
{
    return RopeHandleCall(L"installs", g_ropeInstall, actor);
}

std::uintptr_t RopeLeave(void* actor) noexcept
{
    return RopeHandleCall(L"leaves", g_ropeLeave, actor);
}

// ---- the hit rumble ----

/// The gun's hit effect (a montage notify, game thread): for a gun attached to a seat's member, the game's player
/// test answers the member's type on this thread and the seat owns the add.
std::uintptr_t Hit(void* gun, std::uint8_t kind) noexcept
{
    const auto original = reinterpret_cast<HitFn>(g_hit.Original());
    if (gun == nullptr || !players::generation::OnGameThread())
    {
        return original(gun, kind);
    }
    const void* character = ReadPointer(ReadPointer(ReadPointer(gun, facts::kActorRootComponent), facts::kSceneComponentAttachParent),
                                        facts::kActorComponentOwner);
    const int   seat      = SeatOfCharacter(character);
    const auto  type      = seat >= 1 ? session::party_member::PlayerTypeOf(players::generation::MemberOf(seat)) : std::nullopt;
    if (!type)
    {
        return original(gun, kind);
    }
    const scopes::leader_answer::ThreadScope     answer(*type);
    const feedback::traversal_rumble::OwnerScope owner(seat);
    platform::LogDebug(L"hooks: {}'s gun hit effect {} with their type answering the player test", players::SeatName(seat), kind);
    return original(gun, kind);
}

void RestoreGun() noexcept
{
    for (platform::CallRedirect& call : g_shotPointCalls)
    {
        if (call.IsInstalled())
        {
            (void)call.Restore();
        }
    }
    for (platform::VTableEntry* slot : {&g_shotInstall, &g_shotLeave, &g_shotBegin, &g_shotEnd, &g_shotNotify, &g_shotAIEnter})
    {
        if (slot->Original() != 0)
        {
            (void)slot->Restore();
        }
    }
}

void RestoreRope() noexcept
{
    if (g_ropeActive.IsInstalled())
    {
        (void)g_ropeActive.Restore();
    }
    for (platform::VTableEntry* slot : {&g_ropeInstall, &g_ropeLeave, &g_ropeBegin, &g_ropeEnd})
    {
        if (slot->Original() != 0)
        {
            (void)slot->Restore();
        }
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    // The gun: the point's three callers and the pin's removal, the move's Begin and End, the camera handle slots, and
    // the move's Tick in the climb context; all or none (a point switched by the OR without the scoped Begin would
    // start a flight the camera never follows, and the reverse).
    const auto point  = platform::FindFunction(image, kShotPoint);
    const auto remove = platform::FindFunction(image, kRemovePin);
    g_shotPoint       = point ? reinterpret_cast<PointFn>(*point) : nullptr;
    g_removePin       = remove ? reinterpret_cast<RemovePinFn>(*remove) : nullptr;
    const auto preset = image.Rebase(facts::kHookShotPresetName, sizeof(std::uint64_t));
    const auto player = image.Rebase(facts::kHookShotPlayerPresetName, sizeof(std::uint64_t));
    g_shotPreset       = preset ? reinterpret_cast<const std::uint64_t*>(*preset) : nullptr;
    g_shotPlayerPreset = player ? reinterpret_cast<const std::uint64_t*>(*player) : nullptr;
    bool gun = g_shotPoint != nullptr && g_removePin != nullptr && g_shotPreset != nullptr && g_shotPlayerPreset != nullptr &&
               input::climb_context::MovePlaced(L"hookshot");
    for (std::size_t i = 0; gun && i < std::size(kShotPointCalls); ++i)
    {
        const auto site = image.Rebase(kShotPointCalls[i], 5);
        gun = site && g_shotPointCalls[i].Install(*site, reinterpret_cast<std::uintptr_t>(g_shotPoint),
                                                   reinterpret_cast<std::uintptr_t>(&ShotPointOr));
    }
    gun = gun && platform::ReplaceSlot(image, kShotBeginSlot, reinterpret_cast<std::uintptr_t>(&ShotBegin), g_shotBegin) &&
          platform::ReplaceSlot(image, kShotEndSlot, reinterpret_cast<std::uintptr_t>(&ShotEnd), g_shotEnd) &&
          platform::ReplaceSlot(image, kShotInstallSlot, reinterpret_cast<std::uintptr_t>(&ShotInstall), g_shotInstall) &&
          platform::ReplaceSlot(image, kShotLeaveSlot, reinterpret_cast<std::uintptr_t>(&ShotLeave), g_shotLeave) &&
          platform::ReplaceSlot(image, kShotNotifySlot, reinterpret_cast<std::uintptr_t>(&ShotNotify), g_shotNotify) &&
          platform::ReplaceSlot(image, kShotAIEnterSlot, reinterpret_cast<std::uintptr_t>(&ShotAIEnter), g_shotAIEnter);
    if (!gun)
    {
        RestoreGun();
    }
    g_gun = gun;
    // The rope: its switch, the move's Begin and End, the global handle's slots, the move's Tick in the climb context;
    // all or none.
    const auto camera = image.Rebase(facts::kHookRopeCameraModuleGlobal, kHandleSize);
    g_ropeCamera      = camera ? *camera : 0;
    bool rope = g_ropeCamera != 0 && input::climb_context::MovePlaced(L"hook rope") &&
                platform::InstallAtEntryOrSecond(g_ropeActive, image, kRopeActive, reinterpret_cast<std::uintptr_t>(&RopeActiveOr)) &&
                platform::ReplaceSlot(image, kRopeBeginSlot, reinterpret_cast<std::uintptr_t>(&RopeBegin), g_ropeBegin) &&
                platform::ReplaceSlot(image, kRopeEndSlot, reinterpret_cast<std::uintptr_t>(&RopeEnd), g_ropeEnd) &&
                platform::ReplaceSlot(image, kRopeInstallSlot, reinterpret_cast<std::uintptr_t>(&RopeInstall), g_ropeInstall) &&
                platform::ReplaceSlot(image, kRopeLeaveSlot, reinterpret_cast<std::uintptr_t>(&RopeLeave), g_ropeLeave);
    if (!rope)
    {
        RestoreRope();
    }
    g_rope = rope;
    const bool hit = gun && platform::InstallAtEntryOrSecond(g_hit, image, kHit, reinterpret_cast<std::uintptr_t>(&Hit));
    if (gun && rope && hit)
    {
        platform::Log(L"hooks: the grappling gun and the hook rope for players 2 and 3 ready (the point's switch takes every "
                      L"player's wish, a press on the gun point starts the hookshot on the presser's character alone, a "
                      L"player's member follows nobody through it, the move's begin and end and the camera handle are the "
                      L"player's own, the gun's hit rumble on their pad)");
        return;
    }
    platform::LogWarning(L"hooks: the grappling gun for players 2 and 3 {}; the hook rope {}; the gun's hit rumble on their pad {}",
                         gun ? L"ready" : L"NOT placed (a check failed): the gun stays player 1's",
                         rope ? L"ready" : L"NOT placed (a check failed): ropes stay player 1's", hit ? L"yes" : L"no");
}

void NotePressesAfter(int player) noexcept
{
    if (!g_gun || !players::generation::OnGameThread())
    {
        return;
    }
    for (const Wishes& wishes : g_shotWishes)
    {
        void* actor = wishes.actor != 0 ? players::weak_object::ObjectOf(wishes.actor) : nullptr;
        if (actor == nullptr)
        {
            continue;
        }
        const std::uint8_t flag    = ReadField<std::uint8_t>(actor, facts::kHookShotPressFlag);
        Presser*           presser = PresserOf(actor, flag != 0);
        if (presser == nullptr)
        {
            if (flag != 0 && !g_pressFullLogged)
            {
                g_pressFullLogged = true;
                try
                {
                    platform::LogWarning(L"hooks: no room to note who pressed the gun point {}: the game's own notifies stand "
                                         L"for it (said once)",
                                         players::NameOf(actor));
                }
                catch (...)
                {
                }
            }
            continue;
        }
        if (flag != 0 && presser->lastFlag == 0)
        {
            NoteRise(*presser, player, actor, L"");
        }
        else if (flag == 0)
        {
            presser->player = -1;
        }
        presser->lastFlag = flag;
    }
}

PointView GunPointViewFor(std::uint64_t key, int player) noexcept
{
    if (!g_gun || key == 0 || player < 0 || player >= static_cast<int>(players::kMaxPlayers) ||
        !players::generation::OnGameThread())
    {
        return PointView::NotTracked;
    }
    for (Wishes& wishes : g_shotWishes)
    {
        if (wishes.actor == 0 || !AnyOn(wishes.wish))
        {
            continue;
        }
        const void* actor = players::weak_object::ObjectOf(wishes.actor);
        if (actor == nullptr || players::actor_fields::FirstTag(actor) != key)
        {
            continue;
        }
        const PointArgs&   args = wishes.args[player];
        const PointView view = !wishes.wish[player]                      ? PointView::Off
                                  : args.allAngle != 0 || args.ng != 0      ? PointView::NotUsable
                                                                            : PointView::Usable;
        wishes.view[player] = view;
        return view;
    }
    return PointView::NotTracked;
}

void LogGunPointViews() noexcept
{
    if (!g_gun || !players::generation::OnGameThread() || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    for (Wishes& wishes : g_shotWishes)
    {
        for (int player = 0; player < static_cast<int>(players::kMaxPlayers); ++player)
        {
            const PointView view = wishes.view[player];
            if (view == PointView::NotTracked || view == wishes.viewLogged[player])
            {
                continue;
            }
            wishes.viewLogged[player] = view;
            try
            {
                platform::LogDebug(L"hooks: {}'s selection sees the gun point {} {}", players::SeatName(player),
                                   players::NameOf(players::weak_object::ObjectOf(wishes.actor)),
                                   view == PointView::Off         ? L"off (their own notify wishes it off)"
                                   : view == PointView::NotUsable ? L"on, not usable from where they are (their own notify's answer)"
                                                                     : L"on and usable");
            }
            catch (...)
            {
            }
        }
    }
}

void Remove() noexcept
{
    RestoreGun();
    RestoreRope();
    if (g_hit.IsInstalled())
    {
        (void)g_hit.Restore();
    }
    g_gun = g_rope = false;
    g_shotWishes.fill({});
    g_ropeWishes.fill({});
    g_pressers.fill({});
    g_ropeKeys.fill(0);
    g_shotFullLogged = g_ropeFullLogged = g_pressFullLogged = g_notifyScopeLogged = false;
    const std::lock_guard<std::recursive_mutex> lock(g_handles);
    g_shotHandles.fill({});
    g_ropeHandles.fill({});
}

bool GunReady() noexcept
{
    return g_gun;
}

bool RopeReady() noexcept
{
    return g_rope;
}

void NoteRope(const void* actor) noexcept
{
    if (!g_rope || actor == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    const std::uint64_t key = players::actor_fields::FirstTag(actor);
    if (key == 0)
    {
        return;
    }
    for (const std::uint64_t noted : g_ropeKeys)
    {
        if (noted == key)
        {
            return;
        }
    }
    const std::uint64_t dropped = g_ropeKeys[g_nextRopeKey];
    g_ropeKeys[g_nextRopeKey]   = key;
    g_nextRopeKey               = (g_nextRopeKey + 1) % kMaxActors;
    try
    {
        platform::LogDebug(L"hooks: the rope {} is delivered: its point is a player 2's or 3's press from here on{}",
                           players::NameOf(actor),
                           dropped != 0 ? std::format(L" (the oldest noted rope {} forgotten)", players::unreal::NameText(dropped))
                                        : std::wstring());
    }
    catch (...)
    {
    }
}

bool AdmitsPoint(std::uint64_t key, std::uint64_t preset) noexcept
{
    if (g_gun && preset != 0)
    {
        std::uint64_t shot = 0, shotPlayer = 0;
        if ((platform::CopyGuarded(&shot, g_shotPreset, sizeof(shot)) && preset == shot) ||
            (platform::CopyGuarded(&shotPlayer, g_shotPlayerPreset, sizeof(shotPlayer)) && preset == shotPlayer))
        {
            return true;
        }
    }
    if (g_rope && key != 0)
    {
        for (const std::uint64_t noted : g_ropeKeys)
        {
            if (noted == key)
            {
                return true;
            }
        }
    }
    return false;
}

}  // namespace lss::traversal::hooks
