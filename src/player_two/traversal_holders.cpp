#include "player_two/traversal_shared.h"

#include "facts/move_state.h"
#include "player_two/climb_context.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded_read.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

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

// The move classes of player 2's traversal cameras (player_two/traversal_camera.h): the move's Begin installs on
// player 2's own holder for the actor, the game's holder functions run on it, the holder check keeps it, the actor's
// leave and EndPlay release it; the per-frame rotation gate and the climbing wall's hip calls answer for player 2.

namespace lss::player_two::traversal_camera {

namespace {

struct MoveLive
{
    std::uintptr_t         actorVt = 0;
    platform::VTableEntry  begin;
    platform::CallRedirect gate;
    platform::VTableEntry  validate;
    platform::VTableEntry  leave;    // player 2's own holder: the leave and EndPlay slots
    platform::VTableEntry  endPlay;
    bool                   beginPlaced    = false;
    bool                   validatePlaced = false;
    bool                   leavePlaced    = false;
    bool                   endPlayPlaced  = false;
    const void*            seenActor      = nullptr;  // the validate's last logged outcome (game thread)
    bool                   seenKept       = false;
};
std::array<MoveLive, kMoveCount> g_move;

// Player 2's holders. A slot is claimed and released on the game thread; `actor` is published last (release) and read
// on any thread (acquire), the holder's own bytes are the game's to read and write.
struct TwoHolder
{
    alignas(16) std::byte    bytes[kHolderSize]{};
    std::atomic<const void*> actor{nullptr};
    bool                     loggedActive = false;  // game thread: the last logged state
};
std::array<TwoHolder, kTwoHolders> g_twoHolders;
platform::EntryRedirect            g_installEntry;
platform::EntryRedirect            g_getModuleEntry;
HolderLeaveFn                      g_holderLeave    = nullptr;
HolderValidateFn                   g_holderValidate = nullptr;
bool                               g_twoHoldersOn   = false;  // every hook placed (set at install)
thread_local bool                  t_twoBegin = false;        // inside player 2's move Begin (game thread)

/// Holds t_twoBegin for one Begin.
struct TwoBegin
{
    TwoBegin() noexcept { t_twoBegin = true; }
    ~TwoBegin() { t_twoBegin = false; }
    TwoBegin(const TwoBegin&)            = delete;
    TwoBegin& operator=(const TwoBegin&) = delete;
};

// Player 2's move whose holder camera player 2 installed, for the per-frame rotation gate (read on the anim worker).
std::atomic<const void*>   g_playerTwoMove{nullptr};
std::atomic<std::uint64_t> g_playerTwoMemberWeak{0};

// The climbing wall player 2's Begin installed its camera on, and player 2's member then (written on the game thread,
// read by the hip calls on the anim worker).
std::array<platform::CallRedirect, std::size(kHipCalls)> g_hipCalls;
std::atomic<const void*>                                g_climbActor{nullptr};
std::atomic<const void*>                                g_climbMember{nullptr};

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

/// Player 2's holder for `actor`, or null. Any thread.
std::byte* TwoHolderOf(const void* actor) noexcept
{
    if (actor == nullptr)
    {
        return nullptr;
    }
    for (TwoHolder& two : g_twoHolders)
    {
        if (two.actor.load(std::memory_order_acquire) == actor)
        {
            return two.bytes;
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

/// Player 2's member installed a camera on `actor`: on player 2's own holder, or on the actor's (the fallback).
bool HeldByTwo(const void* actor, std::uint64_t weak) noexcept
{
    return HolderNames(TwoHolderOf(actor), weak) || HeldBy(actor, weak);
}

/// Claims player 2's holder for `actor` (game thread): the existing one, or a free slot set up as the game's BeginPlay
/// sets the actor's (zeroed, the owning actor at +0x20). Null when all are in use.
std::byte* ClaimTwoHolder(void* actor) noexcept
{
    if (std::byte* held = TwoHolderOf(actor))
    {
        return held;
    }
    for (TwoHolder& two : g_twoHolders)
    {
        if (two.actor.load(std::memory_order_relaxed) == nullptr)
        {
            std::memset(two.bytes, 0, sizeof(two.bytes));
            std::memcpy(two.bytes + kHolderActorAt, &actor, sizeof(actor));
            two.loggedActive = false;
            two.actor.store(actor, std::memory_order_release);
            return two.bytes;
        }
    }
    return nullptr;
}

/// Releases a holder slot (game thread), leaving its camera first with the game's forced leave (as EndPlay leaves the
/// actor's). The leave reaches the module only while its shared count is above zero, so it is safe after player 2's
/// camera manager is gone.
void ReleaseTwoHolder(TwoHolder& two, const wchar_t* why) noexcept
{
    const void* actor = two.actor.load(std::memory_order_relaxed);
    if (actor == nullptr)
    {
        return;
    }
    const bool active = HolderActive(two.bytes);
    if (active && g_holderLeave != nullptr)
    {
        g_holderLeave(two.bytes, nullptr, 1);
    }
    two.actor.store(nullptr, std::memory_order_release);
    // Player 2's move whose camera this was: the rotation gate stops naming it (its move may be freed and reused).
    g_playerTwoMove.store(nullptr, std::memory_order_release);
    g_playerTwoMemberWeak.store(0, std::memory_order_relaxed);
    platform::LogDebug(L"traversal camera: player 2's own holder for actor 0x{:X} released ({}){}",
                       reinterpret_cast<std::uintptr_t>(actor), why, active ? L"; its camera left first" : L"");
}

/// The holder install (game thread). Player 2's Begin installs on player 2's own holder for the actor: player 1's camera
/// on the actor's holder stays, and player 1's later install never evicts player 2's.
std::uintptr_t HolderInstall(void* holder, void* module, void* args) noexcept
{
    const auto original = reinterpret_cast<InstallFn>(g_installEntry.Original());
    if (!t_twoBegin || holder == nullptr || args == nullptr || ReadPointer(args, 0) != generation::Member())
    {
        return original(holder, module, args);
    }
    std::byte* two = TwoHolderOf(static_cast<const std::byte*>(holder) - kHolder);
    return original(two != nullptr ? static_cast<void*>(two) : holder, module, args);
}

/// The holder's module getter (game thread in the installers, anim worker in the per-frame setters). Inside player 2's
/// Begin or player 2's climb Tick: player 2's own holder; with none, the actor's only if player 2's member installed it,
/// else no module (player 2's climb never moves player 1's camera). Everything else: the game's.
void* HolderModule(void* holder) noexcept
{
    const auto original = reinterpret_cast<GetModuleFn>(g_getModuleEntry.Original());
    if (holder == nullptr || (!t_twoBegin && climb_context::PlayerHere() != 2))
    {
        return original(holder);
    }
    if (std::byte* two = TwoHolderOf(static_cast<const std::byte*>(holder) - kHolder))
    {
        return original(two);
    }
    return HolderNames(static_cast<const std::byte*>(holder), generation::MemberWeak())
               ? original(holder)
               : nullptr;
}

/// The actor's holder leave (`vt+0x8F8`, from the camera off; anim worker on a normal finish, game thread on an abort):
/// player 2's member leaves player 2's own holder too. The game's leave on the actor's holder follows (its owner check
/// keeps player 1's camera).
template <std::size_t Index>
std::uintptr_t HolderLeave(void* actor, void* character) noexcept
{
    const auto original = reinterpret_cast<LeaveSlotFn>(g_move[Index].leave.Original());
    if (character != nullptr && generation::Active() && character == generation::Member())
    {
        if (std::byte* two = TwoHolderOf(actor))
        {
            g_holderLeave(two, character, 0);
        }
    }
    return original(actor, character);
}

/// The actor's EndPlay (game thread): player 2's holder for it is left and released before the game's own.
template <std::size_t Index>
std::uintptr_t HolderEndPlay(void* actor, std::uintptr_t reason) noexcept
{
    const auto original = reinterpret_cast<EndPlayFn>(g_move[Index].endPlay.Original());
    if (generation::OnGameThread())
    {
        for (TwoHolder& two : g_twoHolders)
        {
            if (two.actor.load(std::memory_order_relaxed) == actor)
            {
                ReleaseTwoHolder(two, L"the actor ends play");
            }
        }
    }
    return original(actor, reason);
}

template <std::size_t Index>
std::uintptr_t MoveBegin(void* move) noexcept
{
    const auto original = reinterpret_cast<BeginFn>(g_move[Index].begin.Original());
    if (g_inside || !generation::Active() || !generation::OnGameThread())
    {
        return original(move);
    }
    void*      member     = generation::Member();
    const auto memberWeak = member != nullptr ? leader_scope::WeakOf(member) : 0;
    void*      actor      = ObjectIn(move, kMoveActor);
    if (actor != nullptr && VTableOf(actor) != g_move[Index].actorVt)
    {
        actor = nullptr;
    }
    const MoveClass& cls = kMoveClasses[Index];
    if (member == nullptr || ObjectIn(move, kMoveCharacter) != member)
    {
        // Anyone else's Begin: with player 2's own holder it never touches player 2's camera. Without (the fallback, on
        // the actor's holder), if it takes over a holder player 2's camera held, player 2's rotation stops with it.
        if (g_twoHoldersOn)
        {
            return original(move);
        }
        const bool heldByTwo = HeldBy(actor, memberWeak);
        const auto result    = original(move);
        if (heldByTwo && !HeldBy(actor, memberWeak))
        {
            g_playerTwoMove.store(nullptr, std::memory_order_release);
            try
            {
                platform::LogDebug(L"traversal camera: another character's {} on {} took the camera holder from player 2",
                                   cls.name, ActorName(actor));
            }
            catch (...)
            {
            }
        }
        return result;
    }
    // Player 2's new move: the hip calls follow only a climbing wall this Begin installs player 2's camera on.
    g_climbActor.store(nullptr, std::memory_order_release);
    // Player 2's own holder for this actor; without one (every slot in use, or the hooks are not placed), the actor's.
    const std::byte* own = g_twoHoldersOn && actor != nullptr ? ClaimTwoHolder(actor) : nullptr;
    if (g_twoHoldersOn && actor != nullptr && own == nullptr)
    {
        try
        {
            platform::Log(L"traversal camera: player 2's own camera holders are all in use; player 2's {} on {} uses the "
                          L"wall's own",
                          cls.name, ActorName(actor));
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
            platform::LogDebug(L"traversal camera: player 2's {} on {} begins without a camera: another character's "
                               L"camera holds it",
                               cls.name, ActorName(actor));
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
        const TwoBegin                           twoBegin;
        std::optional<leader_scope::LeaderScope> leader;
        if (rail)
        {
            leader.emplace(memberWeak);
        }
        scoped = member_movement::InPrimaryScope(member, &RunBegin, &call);
        if (!scoped)
        {
            call.result = original(move);
        }
    }
    const bool installed = HeldByTwo(actor, memberWeak);
    if (installed)
    {
        g_playerTwoMemberWeak.store(memberWeak, std::memory_order_relaxed);
        g_playerTwoMove.store(move, std::memory_order_release);
        if (Index == kClimbingWall)
        {
            g_climbMember.store(member, std::memory_order_relaxed);
            g_climbActor.store(actor, std::memory_order_release);
        }
    }
    try
    {
        platform::LogDebug(L"traversal camera: player 2's {} on {} begins: {}{}; camera holder {}", cls.name,
                           ActorName(actor), scoped ? L"as player 2" : L"a scope was refused, the game's answer stands",
                           rail ? L" (rail camera)" : L"",
                           HolderNames(own, memberWeak) ? L"player 2's own (the wall's stays the game's)"
                           : installed                  ? L"the wall's, player 2's"
                                                        : L"not player 2's");
    }
    catch (...)
    {
    }
    return call.result;
}

/// The per-frame rotation's move gate (anim worker): the game's answer, or yes for player 2's installed move.
template <std::size_t Index>
std::uint8_t MoveGateCall(const void* move) noexcept
{
    const auto answer = reinterpret_cast<GateFn>(g_move[Index].gate.Original())(move);
    if (answer != 0 || move == nullptr || move != g_playerTwoMove.load(std::memory_order_acquire))
    {
        return answer;
    }
    const auto weak = g_playerTwoMemberWeak.load(std::memory_order_relaxed);
    return weak != 0 && ReadField<std::uint64_t>(move, kMoveCharacter) == weak ? std::uint8_t{1} : answer;
}

/// The actor's holder check (field-actor manager tick, game thread): a holder player 2's member installed is checked
/// with the party leader's type answering the member's, so the game keeps player 2's camera as it keeps player 1's.
/// Every other holder, and anything off the game thread, gets the game's own check. Logged when the outcome changes.
template <std::size_t Index>
std::uintptr_t ValidateHolder(void* actor) noexcept
{
    const auto original = reinterpret_cast<ValidateFn>(g_move[Index].validate.Original());
    if (!generation::Active() || !generation::OnGameThread())
    {
        return original(actor);
    }
    void*      member     = generation::Member();
    const auto memberWeak = member != nullptr ? leader_scope::WeakOf(member) : 0;
    std::byte* own        = TwoHolderOf(actor);
    const bool ownOn      = HolderActive(own);
    const bool wallTwo    = HeldBy(actor, memberWeak);  // the fallback: player 2's camera on the actor's holder
    if (!ownOn && !wallTwo)
    {
        return original(actor);
    }
    const auto     type  = party_member::PlayerTypeOf(member);
    bool           typed = false;
    std::uintptr_t result;
    {
        std::optional<leader_scope::LeaderTypeScope> scope;
        if (wallTwo && type)
        {
            scope.emplace(*type);
            typed = scope->Held();
        }
        result = original(actor);
    }
    bool kept = wallTwo && HeldBy(actor, memberWeak);
    if (ownOn)
    {
        // The game's validate on player 2's own holder, with the leader type answering the member's (W1).
        {
            std::optional<leader_scope::LeaderTypeScope> scope;
            if (type)
            {
                scope.emplace(*type);
                typed = scope->Held();
            }
            g_holderValidate(own);
        }
        kept = HolderActive(own);
        if (kept)
        {
            result &= ~std::uintptr_t{0xFF};  // "can deactivate": not while player 2's camera is on
        }
    }
    MoveLive& live = g_move[Index];
    if (live.seenActor != actor || live.seenKept != kept)
    {
        live.seenActor = actor;
        live.seenKept  = kept;
        try
        {
            const auto level = kept ? platform::Level::Debug : platform::Level::Info;
            platform::LogAt(level, L"traversal camera: the game's holder check {} player 2's {} camera on {} ({})",
                            kept ? L"keeps" : L"still REMOVED", kMoveClasses[Index].name, ActorName(actor),
                            typed ? L"player 2's member counted as the leader's type"
                                  : L"the leader type could not be set: the game's own check");
        }
        catch (...)
        {
        }
    }
    return result;
}

/// The climbing move's phase-3 player test before the hip-framing setter (anim worker; game thread on an abort): the
/// game's answer, or yes for player 2's member on the wall whose holder player 2's Begin filled. The holder is read
/// guarded (the wall is cached raw), and must still name player 2's member: a later install by anyone else ends it.
template <std::size_t Site>
std::uint8_t HipTrackCall(const void* character) noexcept
{
    const auto answer = reinterpret_cast<CoreFn>(g_hipCalls[Site].Original())(character);
    if (answer != 0 || character == nullptr || character != g_climbMember.load(std::memory_order_relaxed))
    {
        return answer;
    }
    const void* actor = g_climbActor.load(std::memory_order_acquire);
    const auto  weak  = g_playerTwoMemberWeak.load(std::memory_order_relaxed);
    struct
    {
        std::uint8_t  active;
        std::uint8_t  pad[3];
        std::uint32_t ownerLow, ownerHigh;  // the installer's FWeakObjectPtr at +0x4 (unaligned for a u64)
    } holder{};
    const std::byte* own  = TwoHolderOf(actor);  // player 2's own holder, else the wall's
    const bool       held = actor != nullptr && weak != 0 && generation::Active() &&
                      platform::CopyGuarded(&holder, own != nullptr ? own : static_cast<const std::byte*>(actor) + kHolder,
                                            sizeof(holder)) &&
                      holder.active != 0 &&
                      ((static_cast<std::uint64_t>(holder.ownerHigh) << 32) | holder.ownerLow) == weak;
    try
    {
        platform::LogDebug(L"traversal camera: player 2's climbing move {} the wall's hold phase ({} thread): hip framing "
                           L"{}",
                           kHipCallNames[Site], generation::OnGameThread() ? L"game" : L"animation",
                           held ? (Site == 0 ? L"on, as player 1's" : L"off, as player 1's")
                                : L"left as the game decides (player 2's camera does not hold this wall)");
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
                                                                  L"one tick after player 2's install)");
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

/// Player 2's own holders: the two entry redirects and the game's leave and validate, on only with every move class's
/// slots and climb_context's Ticks placed (without the Tick context the module getter cannot tell player 2's climb).
bool PlaceTwoHolders(const platform::GameImage& image) noexcept
{
    bool slots = climb_context::TicksPlaced();
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
        // into the caller's home area, and no branch lands on the second; RE-p2-traversal-climb-holder.md §4).
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
    g_twoHoldersOn  = PlaceTwoHolders(image);
    out.twoHolders  = g_twoHoldersOn;
    return out;
}

bool Remove() noexcept
{
    bool kept = false;
    for (TwoHolder& two : g_twoHolders)
    {
        ReleaseTwoHolder(two, L"the mod is removed");
    }
    g_twoHoldersOn = false;
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
    g_climbActor.store(nullptr, std::memory_order_release);
    return kept;
}

void Tick(bool active) noexcept
{
    if (!active)
    {
        g_climbActor.store(nullptr, std::memory_order_release);
        for (TwoHolder& two : g_twoHolders)
        {
            ReleaseTwoHolder(two, L"player 2 is out");
        }
        return;
    }
    // Player 2's holders: a line when one's camera goes on or off; an idle one is released (a new Begin claims again).
    for (TwoHolder& two : g_twoHolders)
    {
        const void* actor = two.actor.load(std::memory_order_relaxed);
        if (actor == nullptr)
        {
            continue;
        }
        const bool active = HolderActive(two.bytes);
        if (active != two.loggedActive)
        {
            two.loggedActive = active;
            try
            {
                platform::LogDebug(L"traversal camera: player 2's own holder on {}: camera {}", ActorName(actor),
                                   active ? L"on (player 2's camera manager)" : L"off");
            }
            catch (...)
            {
            }
        }
        if (!active)
        {
            ReleaseTwoHolder(two, L"idle");
        }
    }
}

}  // namespace holders

}  // namespace lss::player_two::traversal_camera
