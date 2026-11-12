#include "traversal/traversal_shared.h"

#include "facts/move_state.h"
#include "input/climb_context.h"
#include "input/member_movement.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded_read.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "session/party_member.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

// The move classes of the seats' traversal cameras (traversal/traversal_camera.h): a seat's move Begin
// installs on the seat's own holder for the actor, the game's holder functions run on it, the holder check keeps it,
// the actor's leave and EndPlay release it; the per-frame rotation gate and the climbing wall's hip calls answer for
// the seat. Per seat: a holder pool, the installed move and the climbed wall (by seat, index 0 unused); the seat comes
// from the move's character, the Begin in progress, or climb_context's climber on the anim worker.

namespace lss::traversal::traversal_camera {

namespace {

struct MoveLive
{
    std::uintptr_t         actorVt = 0;
    platform::VTableEntry  begin;
    platform::CallRedirect gate;
    platform::VTableEntry  validate;
    platform::VTableEntry  leave;    // a seat's own holder: the leave and EndPlay slots
    platform::VTableEntry  endPlay;
    bool                   beginPlaced    = false;
    bool                   validatePlaced = false;
    bool                   leavePlaced    = false;
    bool                   endPlayPlaced  = false;
    // The validate's last logged outcome per seat (game thread; index 0 unused).
    const void*            seenActor[players::kMaxPlayers]{};
    bool                   seenKept[players::kMaxPlayers]{};
};
std::array<MoveLive, kMoveCount> g_move;

// Each seat's holders, a pool of kSeatHolders per seat (by seat; index 0, player 1's, unused: player 1 uses the
// actor's own). A slot is claimed and released on the game thread; `actor` is published last (release) and read on any
// thread (acquire), the holder's own bytes are the game's to read and write.
struct SeatHolder
{
    alignas(16) std::byte    bytes[kHolderSize]{};
    std::atomic<const void*> actor{nullptr};
    bool                     loggedActive = false;  // game thread: the last logged state
};
using HolderPool = std::array<SeatHolder, kSeatHolders>;
HolderPool                         g_seatHolders[players::kMaxPlayers];
platform::EntryRedirect            g_installEntry;
platform::EntryRedirect            g_getModuleEntry;
HolderLeaveFn                      g_holderLeave    = nullptr;
HolderValidateFn                   g_holderValidate = nullptr;
bool                               g_seatHoldersOn  = false;  // every hook placed (set at install)
thread_local int                   t_beginSeat      = -1;     // inside this seat's move Begin (game thread), else -1

/// Holds t_beginSeat for one Begin.
struct SeatBegin
{
    explicit SeatBegin(int seat) noexcept { t_beginSeat = seat; }
    ~SeatBegin() { t_beginSeat = -1; }
    SeatBegin(const SeatBegin&)            = delete;
    SeatBegin& operator=(const SeatBegin&) = delete;
};

// Each seat's move whose holder camera the seat installed, for the per-frame rotation gate (read on the anim worker).
std::atomic<const void*>   g_seatMove[players::kMaxPlayers]{};
std::atomic<std::uint64_t> g_seatMemberWeak[players::kMaxPlayers]{};

// The climbing wall each seat's Begin installed its camera on, and the seat's member then (written on the game thread,
// read by the hip calls on the anim worker).
std::array<platform::CallRedirect, std::size(kHipCalls)> g_hipCalls;
std::atomic<const void*>                                g_climbActor[players::kMaxPlayers]{};
std::atomic<const void*>                                g_climbMember[players::kMaxPlayers]{};

/// The built seat whose climb this thread ticks now (climb_context's context), or -1.
int SeatHere() noexcept
{
    const int seat = input::climb_context::SeatHere();
    return seat >= 1 && players::SeatBuilt(seat) ? seat : -1;
}

// --- moves ------------------------------------------------------------------------------------------------------------

struct BeginCall
{
    BeginFn        begin;
    void*          move;
    std::uintptr_t result = 0;
};

void RunBegin(void* context)
{
    auto* call   = static_cast<BeginCall*>(context);
    call->result = call->begin(call->move);
}

bool HeldBy(const void* actor, std::uint64_t weak) noexcept
{
    return actor != nullptr && weak != 0 && ReadField<std::uint8_t>(actor, kHolder) != 0 &&
           ReadField<std::uint64_t>(actor, kHolderOwner) == weak;
}

/// `seat`'s holder for `actor`, or null. Any thread.
std::byte* SeatHolderOf(int seat, const void* actor) noexcept
{
    if (actor == nullptr || !players::SeatIndex(seat))
    {
        return nullptr;
    }
    for (SeatHolder& holder : g_seatHolders[seat])
    {
        if (holder.actor.load(std::memory_order_acquire) == actor)
        {
            return holder.bytes;
        }
    }
    return nullptr;
}

/// A holder's active byte (+0x0).
bool HolderActive(const std::byte* holder) noexcept
{
    return holder != nullptr && std::to_integer<std::uint8_t>(holder[0]) != 0;
}

/// A holder's active byte and installer weak (a holder is read plainly, as the game reads it).
bool HolderNames(const std::byte* holder, std::uint64_t weak) noexcept
{
    if (weak == 0 || !HolderActive(holder))
    {
        return false;
    }
    std::uint64_t owner = 0;
    std::memcpy(&owner, holder + 4, sizeof(owner));
    return owner == weak;
}

/// `seat`'s member installed a camera on `actor`: on the seat's own holder, or on the actor's (the fallback).
bool HeldBySeat(int seat, const void* actor, std::uint64_t weak) noexcept
{
    return HolderNames(SeatHolderOf(seat, actor), weak) || HeldBy(actor, weak);
}

/// Claims `seat`'s holder for `actor` (game thread): the existing one, or a free slot of the seat's pool set up as the
/// game's BeginPlay sets the actor's (zeroed, the owning actor at +0x20). Null when all are in use.
std::byte* ClaimSeatHolder(int seat, void* actor) noexcept
{
    if (std::byte* held = SeatHolderOf(seat, actor))
    {
        return held;
    }
    for (SeatHolder& holder : g_seatHolders[seat])
    {
        if (holder.actor.load(std::memory_order_relaxed) == nullptr)
        {
            std::memset(holder.bytes, 0, sizeof(holder.bytes));
            std::memcpy(holder.bytes + kHolderActorAt, &actor, sizeof(actor));
            holder.loggedActive = false;
            holder.actor.store(actor, std::memory_order_release);
            return holder.bytes;
        }
    }
    return nullptr;
}

/// Releases a holder slot of `seat`'s (game thread), leaving its camera first with the game's forced leave (as EndPlay
/// leaves the actor's). The leave reaches the module only while its shared count is above zero, so it is safe after
/// the seat's camera manager is gone.
void ReleaseSeatHolder(int seat, SeatHolder& holder, const wchar_t* why) noexcept
{
    const void* actor = holder.actor.load(std::memory_order_relaxed);
    if (actor == nullptr)
    {
        return;
    }
    const bool active = HolderActive(holder.bytes);
    if (active && g_holderLeave != nullptr)
    {
        g_holderLeave(holder.bytes, nullptr, 1);
    }
    holder.actor.store(nullptr, std::memory_order_release);
    // The seat's move whose camera this was: the rotation gate stops naming it (its move may be freed and reused).
    g_seatMove[seat].store(nullptr, std::memory_order_release);
    g_seatMemberWeak[seat].store(0, std::memory_order_relaxed);
    platform::LogDebug(L"traversal camera: {}'s own holder for actor 0x{:X} released ({}){}", players::SeatName(seat),
                       reinterpret_cast<std::uintptr_t>(actor), why, active ? L"; its camera left first" : L"");
}

/// The holder install (game thread). A seat's Begin installs on the seat's own holder for the actor: player 1's camera
/// on the actor's holder stays, and player 1's later install never evicts the seat's.
std::uintptr_t HolderInstall(void* holder, void* module, void* args) noexcept
{
    const auto original = reinterpret_cast<InstallFn>(g_installEntry.Original());
    const int  seat     = t_beginSeat;
    if (seat < 1 || holder == nullptr || args == nullptr || ReadPointer(args, 0) != players::generation::MemberOf(seat))
    {
        return original(holder, module, args);
    }
    std::byte* own = SeatHolderOf(seat, static_cast<const std::byte*>(holder) - kHolder);
    return original(own != nullptr ? static_cast<void*>(own) : holder, module, args);
}

/// The holder's module getter (game thread in the installers, anim worker in the per-frame setters). Inside a seat's
/// Begin or a seat's climb Tick: the seat's own holder; with none, the actor's only if the seat's member installed it,
/// else no module (a seat's climb never moves player 1's camera). Everything else: the game's.
void* HolderModule(void* holder) noexcept
{
    const auto original = reinterpret_cast<GetModuleFn>(g_getModuleEntry.Original());
    const int  seat     = holder == nullptr ? -1 : t_beginSeat >= 1 ? t_beginSeat : SeatHere();
    if (seat < 1)
    {
        return original(holder);
    }
    if (std::byte* own = SeatHolderOf(seat, static_cast<const std::byte*>(holder) - kHolder))
    {
        return original(own);
    }
    return HolderNames(static_cast<const std::byte*>(holder), players::generation::MemberWeakOf(seat))
               ? original(holder)
               : nullptr;
}

/// The actor's holder leave (`vt+0x8F8`, from the camera off; anim worker on a normal finish, game thread on an abort):
/// a seat's member leaves the seat's own holder too. The game's leave on the actor's holder follows (its owner check
/// keeps player 1's camera).
template <std::size_t Index>
std::uintptr_t HolderLeave(void* actor, void* character) noexcept
{
    const auto original = reinterpret_cast<LeaveSlotFn>(g_move[Index].leave.Original());
    for (int seat = 1; character != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && character == players::generation::MemberOf(seat))
        {
            if (std::byte* own = SeatHolderOf(seat, actor))
            {
                g_holderLeave(own, character, 0);
            }
            break;
        }
    }
    return original(actor, character);
}

/// The actor's EndPlay (game thread): every seat's holder for it is left and released before the game's own.
template <std::size_t Index>
std::uintptr_t HolderEndPlay(void* actor, std::uintptr_t reason) noexcept
{
    const auto original = reinterpret_cast<EndPlayFn>(g_move[Index].endPlay.Original());
    if (players::generation::OnGameThread())
    {
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)  // only a built seat claims a holder
        {
            for (SeatHolder& holder : g_seatHolders[seat])
            {
                if (holder.actor.load(std::memory_order_relaxed) == actor)
                {
                    ReleaseSeatHolder(seat, holder, L"the actor ends play");
                }
            }
        }
    }
    return original(actor, reason);
}

template <std::size_t Index>
std::uintptr_t MoveBegin(void* move) noexcept
{
    const auto original = reinterpret_cast<BeginFn>(g_move[Index].begin.Original());
    if (g_inside || !players::generation::AnyActive() || !players::generation::OnGameThread())
    {
        return original(move);
    }
    void* actor = ObjectIn(move, kMoveActor);
    if (actor != nullptr && VTableOf(actor) != g_move[Index].actorVt)
    {
        actor = nullptr;
    }
    const MoveClass& cls  = kMoveClasses[Index];
    const int        seat = SeatOfCharacter(ObjectIn(move, kMoveCharacter));
    if (seat < 1)
    {
        // Anyone else's Begin: with the seats' own holders it never touches a seat's camera. Without (the fallback, on
        // the actor's holder), if it takes over a holder a seat's camera held, that seat's rotation stops with it.
        if (g_seatHoldersOn)
        {
            return original(move);
        }
        std::uint64_t weaks[players::kMaxPlayers]{};
        bool          heldBySeat[players::kMaxPlayers]{};
        for (int each = 1; each <= players::kSeatsBuilt; ++each)
        {
            void* member     = players::generation::Active(each) ? players::generation::MemberOf(each) : nullptr;
            weaks[each]      = member != nullptr ? scopes::leader_scope::WeakOf(member) : 0;
            heldBySeat[each] = HeldBy(actor, weaks[each]);
        }
        const auto result = original(move);
        for (int each = 1; each <= players::kSeatsBuilt; ++each)
        {
            if (heldBySeat[each] && !HeldBy(actor, weaks[each]))
            {
                g_seatMove[each].store(nullptr, std::memory_order_release);
                try
                {
                    platform::LogDebug(L"traversal camera: another character's {} on {} took the camera holder from {}",
                                       cls.name, ActorName(actor), players::SeatName(each));
                }
                catch (...)
                {
                }
            }
        }
        return result;
    }
    void*                member     = players::generation::MemberOf(seat);
    const auto           memberWeak = scopes::leader_scope::WeakOf(member);
    const wchar_t* const name       = players::SeatName(seat);
    // The seat's new move: the hip calls follow only a climbing wall this Begin installs the seat's camera on.
    g_climbActor[seat].store(nullptr, std::memory_order_release);
    // The seat's own holder for this actor; without one (every slot in use, or the hooks are not placed), the actor's.
    const std::byte* own = g_seatHoldersOn && actor != nullptr ? ClaimSeatHolder(seat, actor) : nullptr;
    if (g_seatHoldersOn && actor != nullptr && own == nullptr)
    {
        try
        {
            platform::Log(L"traversal camera: {}'s own camera holders are all in use; {}'s {} on {} uses the "
                          L"wall's own",
                          name, name, cls.name, ActorName(actor));
        }
        catch (...)
        {
        }
    }
    // Player 1 holds this actor's camera: a second install would evict it (the holder deactivates the previous module).
    if (own == nullptr && actor != nullptr && ReadField<std::uint8_t>(actor, kHolder) != 0 && !HeldBy(actor, memberWeak))
    {
        try
        {
            platform::LogDebug(L"traversal camera: {}'s {} on {} begins without a camera: another character's "
                               L"camera holds it",
                               name, cls.name, ActorName(actor));
        }
        catch (...)
        {
        }
        return original(move);
    }
    const bool rail = actor != nullptr && ReadField<std::uint8_t>(actor, kActorRail) != 0;
    BeginCall  call{original, move};
    bool       scoped = false;
    {
        const Inside                             inside;
        const SeatBegin                          seatBegin(seat);
        std::optional<scopes::leader_scope::LeaderScope> leader;
        if (rail)
        {
            leader.emplace(memberWeak);
        }
        scoped = input::member_movement::InPrimaryScope(seat, member, &RunBegin, &call);
        if (!scoped)
        {
            call.result = original(move);
        }
    }
    const bool installed = HeldBySeat(seat, actor, memberWeak);
    if (installed)
    {
        g_seatMemberWeak[seat].store(memberWeak, std::memory_order_relaxed);
        g_seatMove[seat].store(move, std::memory_order_release);
        if (Index == kClimbingWall)
        {
            g_climbMember[seat].store(member, std::memory_order_relaxed);
            g_climbActor[seat].store(actor, std::memory_order_release);
        }
    }
    try
    {
        if (platform::Enabled(platform::Level::Debug))
        {
            platform::LogDebug(L"traversal camera: {}'s {} on {} begins: {}{}; camera holder {}", name, cls.name,
                               ActorName(actor),
                               scoped ? std::format(L"as {}", name)
                                      : std::wstring(L"a scope was refused, the game's answer stands"),
                               rail ? L" (rail camera)" : L"",
                               HolderNames(own, memberWeak) ? std::format(L"{}'s own (the wall's stays the game's)", name)
                               : installed                  ? std::format(L"the wall's, {}'s", name)
                                                            : std::format(L"not {}'s", name));
        }
    }
    catch (...)
    {
    }
    return call.result;
}

/// The per-frame rotation's move gate (anim worker): the game's answer, or yes for a seat's installed move.
template <std::size_t Index>
std::uint8_t MoveGateCall(const void* move) noexcept
{
    const auto answer = reinterpret_cast<GateFn>(g_move[Index].gate.Original())(move);
    if (answer != 0 || move == nullptr)
    {
        return answer;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (move == g_seatMove[seat].load(std::memory_order_acquire))
        {
            const auto weak = g_seatMemberWeak[seat].load(std::memory_order_relaxed);
            return weak != 0 && ReadField<std::uint64_t>(move, kMoveCharacter) == weak ? std::uint8_t{1} : answer;
        }
    }
    return answer;
}

/// The actor's holder check (field-actor manager tick, game thread): a holder a seat's member installed is checked
/// with the party leader's type answering the member's, so the game keeps the seat's camera as it keeps player 1's.
/// Every other holder, and anything off the game thread, gets the game's own check. Logged per seat when the outcome
/// changes. The game's check of the actor's holder runs once, with the type of the seat whose camera is on it (the
/// fallback); each seat's own holder is checked after it, in seat order.
template <std::size_t Index>
std::uintptr_t ValidateHolder(void* actor) noexcept
{
    const auto original = reinterpret_cast<ValidateFn>(g_move[Index].validate.Original());
    if (!players::generation::AnyActive() || !players::generation::OnGameThread())
    {
        return original(actor);
    }
    struct SeatCheck
    {
        void*                       member = nullptr;
        std::uint64_t               weak   = 0;
        std::byte*                  own    = nullptr;
        bool                        ownOn  = false;
        bool                        wall   = false;  // the fallback: the seat's camera on the actor's holder
    };
    SeatCheck checks[players::kMaxPlayers]{};
    int       wallSeat = -1;
    bool      any      = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat))
        {
            continue;
        }
        SeatCheck& check = checks[seat];
        check.member     = players::generation::MemberOf(seat);
        check.weak       = check.member != nullptr ? scopes::leader_scope::WeakOf(check.member) : 0;
        check.own        = SeatHolderOf(seat, actor);
        check.ownOn      = HolderActive(check.own);
        check.wall       = HeldBy(actor, check.weak);
        wallSeat         = check.wall && wallSeat < 0 ? seat : wallSeat;
        any              = any || check.ownOn || check.wall;
    }
    if (!any)
    {
        return original(actor);
    }
    bool           wallTyped = false;
    std::uintptr_t result;
    {
        std::optional<scopes::leader_scope::LeaderTypeScope> scope;
        const auto type = wallSeat >= 1 ? session::party_member::PlayerTypeOf(checks[wallSeat].member) : std::nullopt;
        if (type)
        {
            scope.emplace(*type);
            wallTyped = scope->Held();
        }
        result = original(actor);
    }
    MoveLive& live = g_move[Index];
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const SeatCheck& check = checks[seat];
        if (!check.ownOn && !check.wall)
        {
            continue;
        }
        bool typed = seat == wallSeat && wallTyped;
        bool kept  = check.wall && HeldBy(actor, check.weak);
        if (check.ownOn)
        {
            // The game's validate on the seat's own holder, with the leader type answering the member's (W1).
            {
                const auto                                   type = session::party_member::PlayerTypeOf(check.member);
                std::optional<scopes::leader_scope::LeaderTypeScope> scope;
                if (type)
                {
                    scope.emplace(*type);
                    typed = scope->Held();
                }
                g_holderValidate(check.own);
            }
            kept = HolderActive(check.own);
            if (kept)
            {
                result &= ~std::uintptr_t{0xFF};  // "can deactivate": not while a seat's camera is on
            }
        }
        if (live.seenActor[seat] != actor || live.seenKept[seat] != kept)
        {
            live.seenActor[seat] = actor;
            live.seenKept[seat]  = kept;
            try
            {
                const wchar_t* const name  = players::SeatName(seat);
                const auto           level = kept ? platform::Level::Debug : platform::Level::Info;
                platform::LogAt(level, L"traversal camera: the game's holder check {} {}'s {} camera on {} ({})",
                                kept ? L"keeps" : L"still REMOVED", name, kMoveClasses[Index].name, ActorName(actor),
                                typed ? std::format(L"{}'s member counted as the leader's type", name)
                                      : std::wstring(L"the leader type could not be set: the game's own check"));
            }
            catch (...)
            {
            }
        }
    }
    return result;
}

/// The climbing move's phase-3 player test before the hip-framing setter (anim worker; game thread on an abort): the
/// game's answer, or yes for a seat's member on the wall whose holder the seat's Begin filled. The holder is read
/// guarded (the wall is cached raw), and must still name the seat's member: a later install by anyone else ends it.
template <std::size_t Site>
std::uint8_t HipTrackCall(const void* character) noexcept
{
    const auto answer = reinterpret_cast<CoreFn>(g_hipCalls[Site].Original())(character);
    if (answer != 0 || character == nullptr)
    {
        return answer;
    }
    int seat = -1;
    for (int each = 1; each <= players::kSeatsBuilt && seat < 0; ++each)
    {
        seat = character == g_climbMember[each].load(std::memory_order_relaxed) ? each : -1;
    }
    if (seat < 1)
    {
        return answer;
    }
    const void* actor = g_climbActor[seat].load(std::memory_order_acquire);
    const auto  weak  = g_seatMemberWeak[seat].load(std::memory_order_relaxed);
    struct
    {
        std::uint8_t  active;
        std::uint8_t  pad[3];
        std::uint32_t ownerLow, ownerHigh;  // the installer's FWeakObjectPtr at +0x4 (unaligned for a u64)
    } holder{};
    const std::byte* own  = SeatHolderOf(seat, actor);  // the seat's own holder, else the wall's
    const bool       held = actor != nullptr && weak != 0 && players::generation::Active(seat) &&
                      platform::CopyGuarded(&holder, own != nullptr ? own : static_cast<const std::byte*>(actor) + kHolder,
                                            sizeof(holder)) &&
                      holder.active != 0 &&
                      ((static_cast<std::uint64_t>(holder.ownerHigh) << 32) | holder.ownerLow) == weak;
    try
    {
        const wchar_t* const name = players::SeatName(seat);
        if (platform::Enabled(platform::Level::Debug))
        {
            platform::LogDebug(L"traversal camera: {}'s climbing move {} the wall's hold phase ({} thread): hip framing "
                               L"{}",
                               name, kHipCallNames[Site], players::generation::OnGameThread() ? L"game" : L"animation",
                               held ? std::wstring(Site == 0 ? L"on, as player 1's" : L"off, as player 1's")
                                    : std::format(L"left as the game decides ({}'s camera does not hold this wall)", name));
        }
    }
    catch (...)
    {
    }
    return held ? std::uint8_t{1} : answer;
}

// --- placement ------------------------------------------------------------------------------------------------------

template <std::size_t Index>
std::wstring PlaceMove(const platform::GameImage& image)
{
    const MoveClass& cls  = kMoveClasses[Index];
    MoveLive&        live = g_move[Index];
    live.actorVt          = Live(image, cls.actorVt, 0x740);
    live.beginPlaced =
        live.actorVt != 0 &&
        platform::ReplaceSlot(image, cls.begin, reinterpret_cast<std::uintptr_t>(&MoveBegin<Index>), live.begin);
    bool gate = true;
    if (cls.gateCall != 0)
    {
        const auto site   = image.Rebase(cls.gateCall, 5);
        const auto target = platform::FindFunction(image, kMoveGate);
        gate = site && target &&
               live.gate.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&MoveGateCall<Index>));
    }
    live.validatePlaced = live.beginPlaced &&
                          platform::ReplaceSlot(image, kValidateSlots[Index],
                                                reinterpret_cast<std::uintptr_t>(&ValidateHolder<Index>), live.validate);
    live.leavePlaced    = live.beginPlaced &&
                       platform::ReplaceSlot(image, kLeaveSlots[Index],
                                             reinterpret_cast<std::uintptr_t>(&HolderLeave<Index>), live.leave);
    live.endPlayPlaced  = live.beginPlaced &&
                         platform::ReplaceSlot(image, kEndPlaySlots[Index],
                                               reinterpret_cast<std::uintptr_t>(&HolderEndPlay<Index>), live.endPlay);
    return std::format(L"{} {}{}{}", cls.name, live.beginPlaced ? L"yes" : L"NO",
                       gate ? L"" : L" (its per-frame rotation stays player 1's only)",
                       !live.beginPlaced || live.validatePlaced ? L""
                                                                : L" (the holder check differs: the game removes it "
                                                                  L"one tick after player 2's or 3's install)");
}

template <std::size_t... I>
std::wstring PlaceAllMoves(const platform::GameImage& image, std::index_sequence<I...>)
{
    std::wstring text;
    ((text += (text.empty() ? L"" : L", ") + PlaceMove<I>(image)), ...);
    return text;
}

template <std::size_t... I>
bool PlaceHipCalls(const platform::GameImage& image, std::uintptr_t core, std::index_sequence<I...>)
{
    const auto place = [&](std::size_t index, std::uintptr_t replacement) {
        const auto site = image.Rebase(kHipCalls[index], 5);
        return site && g_hipCalls[index].Install(*site, core, replacement);
    };
    return (place(I, reinterpret_cast<std::uintptr_t>(&HipTrackCall<I>)) & ...);
}

/// The seats' own holders: the two entry redirects and the game's leave and validate, on only with every move class's
/// slots and climb_context's Ticks placed (without the Tick context the module getter cannot tell a seat's climb).
bool PlaceSeatHolders(const platform::GameImage& image) noexcept
{
    bool slots = input::climb_context::TicksPlaced();
    for (const MoveLive& live : g_move)
    {
        slots = slots && live.beginPlaced && live.validatePlaced && live.leavePlaced && live.endPlayPlaced;
    }
    const auto leave    = platform::FindFunction(image, kHolderLeave);
    const auto validate = platform::FindFunction(image, kHolderValidate);
    if (!slots || !leave || !validate)
    {
        return false;
    }
    g_holderLeave    = reinterpret_cast<HolderLeaveFn>(*leave);
    g_holderValidate = reinterpret_cast<HolderValidateFn>(*validate);
    const auto install   = kInstall.entryBytes;
    const auto installAt = platform::FindFunction(image, kInstall);
    bool       installed = false;
    if (installAt)
    {
        // 1.005's entry instruction crosses a 16-byte block: the redirect goes on the second (the first is a store
        // into the caller's home area, and no branch lands on the second; research/RE-p2-traversal-climb-holder.md).
        installed = platform::RunningBuild() == platform::BuildIndex::V1005
                        ? g_installEntry.InstallAtSecondInstruction(*installAt, install.first(kInstallFirst),
                                                                    install.subspan(kInstallFirst, kInstallSecond),
                                                                    reinterpret_cast<std::uintptr_t>(&HolderInstall))
                        : g_installEntry.Install(*installAt,
                                                 image.EntryInstruction(kInstall.address, install.first(kInstallFirst)),
                                                 reinterpret_cast<std::uintptr_t>(&HolderInstall));
    }
    const auto getFirst = kGetModule.entryBytes.first(kGetModuleFirst);
    const auto getAt    = platform::FindFunction(image, kGetModule);
    const bool got      = installed && getAt &&
                          g_getModuleEntry.Install(*getAt, image.EntryInstruction(kGetModule.address, getFirst),
                                                   reinterpret_cast<std::uintptr_t>(&HolderModule));
    if (installed && !got)
    {
        g_installEntry.Restore();
    }
    return got;
}

}  // namespace

namespace holders {

Placement Place(const platform::GameImage& image)
{
    Placement out;
    out.classes     = PlaceAllMoves(image, std::make_index_sequence<kMoveCount>{});
    const auto core = platform::FindFunction(image, facts::kIsPlayerCore);
    out.hip         = core && g_move[kClimbingWall].beginPlaced &&
              PlaceHipCalls(image, *core, std::make_index_sequence<std::size(kHipCalls)>{});
    g_seatHoldersOn = PlaceSeatHolders(image);
    out.seatHolders = g_seatHoldersOn;
    return out;
}

bool Remove() noexcept
{
    bool kept = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)  // only a built seat claims a holder
    {
        for (SeatHolder& holder : g_seatHolders[seat])
        {
            ReleaseSeatHolder(seat, holder, L"the mod is removed");
        }
    }
    g_seatHoldersOn = false;
    kept |= g_getModuleEntry.IsInstalled() && !g_getModuleEntry.Restore();
    kept |= g_installEntry.IsInstalled() && !g_installEntry.Restore();
    for (MoveLive& live : g_move)
    {
        kept |= live.beginPlaced && !live.begin.Restore();
        kept |= live.gate.IsInstalled() && !live.gate.Restore();
        kept |= live.validatePlaced && !live.validate.Restore();
        kept |= live.leavePlaced && !live.leave.Restore();
        kept |= live.endPlayPlaced && !live.endPlay.Restore();
        live.beginPlaced = live.validatePlaced = live.leavePlaced = live.endPlayPlaced = false;
    }
    for (platform::CallRedirect& call : g_hipCalls)
    {
        kept |= call.IsInstalled() && !call.Restore();
    }
    for (auto& actor : g_climbActor)
    {
        actor.store(nullptr, std::memory_order_release);
    }
    return kept;
}

void Tick(int seat, bool active) noexcept
{
    if (!players::SeatIndex(seat))
    {
        return;
    }
    if (!active)
    {
        g_climbActor[seat].store(nullptr, std::memory_order_release);
        for (SeatHolder& holder : g_seatHolders[seat])
        {
            ReleaseSeatHolder(seat, holder,
                              players::BySeat(seat, L"player 2 is out", L"player 3 is out"));
        }
        return;
    }
    // The seat's holders: a line when one's camera goes on or off; an idle one is released (a new Begin claims again).
    for (SeatHolder& holder : g_seatHolders[seat])
    {
        const void* actor = holder.actor.load(std::memory_order_relaxed);
        if (actor == nullptr)
        {
            continue;
        }
        const bool active = HolderActive(holder.bytes);
        if (active != holder.loggedActive)
        {
            holder.loggedActive = active;
            try
            {
                const wchar_t* const name = players::SeatName(seat);
                if (platform::Enabled(platform::Level::Debug))
                {
                    platform::LogDebug(L"traversal camera: {}'s own holder on {}: camera {}", name, ActorName(actor),
                                       active ? std::format(L"on ({}'s camera manager)", name) : std::wstring(L"off"));
                }
            }
            catch (...)
            {
            }
        }
        if (!active)
        {
            ReleaseSeatHolder(seat, holder, L"idle");
        }
    }
}

}  // namespace holders

}  // namespace lss::traversal::traversal_camera
