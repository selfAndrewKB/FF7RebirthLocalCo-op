#include "traversal/traversal_camera.h"

#include "facts/controller.h"
#include "input/climb_context.h"
#include "input/member_movement.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "traversal/traversal_shared.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// The executer classes of the seats' traversal cameras (traversal/traversal_camera.h): Start, Tick and End
// run with the seat's camera handle in the actor and in the seat's scopes; the ladder's exit blend, the squeeze's and
// crawl's exit, the one-shot actor's leave; the ladder's entry points for traversal_watch. The seat is the one whose
// member the executer's character is; each seat keeps its own handles, ladder exit and logged states (by seat, index
// 0 unused). The one-shot actor's leave finds its seat by the member or the actor recorded (OneShotLeave).

namespace lss::traversal::traversal_camera {

namespace {

struct ExecLive
{
    std::uintptr_t        actorVt = 0;
    platform::VTableEntry start, end, tick;
    bool                  startPlaced = false, endPlaced = false, tickPlaced = false;
};
std::array<ExecLive, kExecCount> g_exec;

// Each seat's camera handles per actor (by seat, then by the actor's weak value: keyed by the seat and the actor),
// moved in and out of the actor, never copied, so the reference counts stay the game's. An entry whose actor is gone
// keeps its weak reference (one small block); past kHandlePrune entries in a seat's map those are dropped from it.
using HandleBytes = std::array<std::uint64_t, kMaxHandle / 8>;
using HandleMap   = std::unordered_map<std::uint64_t, HandleBytes>;
constexpr std::size_t kHandlePrune = 32;
HandleMap             g_handles[players::kMaxPlayers];     // by seat; index 0 unused
const void*           g_exitLogged[players::kMaxPlayers]{};  // by seat: the executer whose exit was last logged

// A seat's ladder exit (game thread): the executer whose exit blend was asked for, and the Ticks waited since.
struct LadderExit
{
    const void* executer  = nullptr;
    bool        requested = false;
    bool        scoped    = false;  // the exit's camera off ran in the seat's scopes (logged once)
    unsigned    waited    = 0;
};
LadderExit g_ladderExit[players::kMaxPlayers];  // by seat; index 0 unused
RequestFn  g_requestCamExit = nullptr;

// The one-shot actor's leave slot, and the actor each seat's one-shot camera went on, as a weak value (game thread
// writes; the leave resolves it on any thread, so a freed actor whose memory is reused never matches).
platform::VTableEntry      g_oneShotLeave;
bool                       g_oneShotLeavePlaced = false;
std::atomic<std::uint64_t> g_oneShotWeak[players::kMaxPlayers]{};  // by seat; index 0 unused

// The squeeze's and crawl's exit calls, and the executer whose exit ran (per seat and class; reset at Start).
OneArgFn                                                  g_squeezeExit = nullptr;
OneArgFn                                                  g_cameraOff   = nullptr;
std::array<std::array<const void*, kExecCount>, players::kMaxPlayers> g_exitRan{};

HandleBytes ReadHandle(const void* actor, const ExecClass& cls) noexcept
{
    HandleBytes bytes{};
    for (std::uint32_t i = 0; i < cls.handleSize / 8; ++i)
    {
        bytes[i] = ReadField<std::uint64_t>(actor, cls.handle + 8 * i);
    }
    return bytes;
}

void WriteHandle(void* actor, const ExecClass& cls, const HandleBytes& bytes) noexcept
{
    for (std::uint32_t i = 0; i < cls.handleSize / 8; ++i)
    {
        WriteField<std::uint64_t>(actor, cls.handle + 8 * i, bytes[i]);
    }
}

/// "active", "inactive", "gone" or "none" for the first module of a handle. The weak reference keeps the reference
/// controller alive; the module only while its shared count (controller +0x8) is above 0, as the game's leaves check.
const wchar_t* ModuleText(const HandleBytes& handle) noexcept
{
    if (handle[0] == 0 || handle[1] == 0)
    {
        return L"none";
    }
    if (ReadField<std::int32_t>(reinterpret_cast<const void*>(handle[1]), 8) <= 0)
    {
        return L"gone";
    }
    return ReadField<std::uint8_t>(reinterpret_cast<const void*>(handle[0]), kModuleActive) != 0 ? L"active"
                                                                                                  : L"inactive";
}

/// The slide flag of a handle's module (a seat's own, or the ladder's), or -1 when it has no live module.
int SlideFlag(const HandleBytes& handle) noexcept
{
    const std::wstring_view state(ModuleText(handle));
    return state == L"none" || state == L"gone"
               ? -1
               : ReadField<std::uint8_t>(reinterpret_cast<const void*>(handle[0]), kLadderSlide);
}

std::array<int, players::kMaxPlayers> g_slideSeen{-1, -1, -1};  // game thread: by seat (0 player 1's), the last logged

/// A line when a player's ladder camera's slide flag changes (W4); `seat` 0 is player 1.
void NoteSlide(int seat, int flag) noexcept
{
    if (seat < 0 || seat >= players::kMaxPlayers || flag < 0 || flag == g_slideSeen[seat])
    {
        return;
    }
    g_slideSeen[seat] = flag;
    platform::LogDebug(L"traversal camera: player {}'s ladder camera: slide view {}", seat + 1,
                       flag != 0 ? L"on (the wider slide framing)" : L"off");
}

enum class Kind : std::uint8_t
{
    Start,
    Tick,
    End,
    Request,  // the ladder's exit-blend request (the climb move's state delegate body)
    OneArg    // a one-argument game call: the executer's capture slot, or `arg`
};

struct Call
{
    Kind           kind;
    ExecFn         exec     = nullptr;
    TickFn         tick     = nullptr;
    void*          executer = nullptr;
    double         dt       = 0;
    void*          third    = nullptr;
    std::int64_t   fourth   = 0;
    std::uintptr_t result   = 0;
    RequestFn      request  = nullptr;
    std::uint8_t   state    = 0;
    OneArgFn       one      = nullptr;
    void*          arg      = nullptr;  // nullptr: the capture slot holding the executer

    std::uintptr_t Run() noexcept
    {
        switch (kind)
        {
        case Kind::Tick:
            return tick(executer, dt, third, fourth);
        case Kind::Request:
            return request(&executer, state);  // the delegate's capture: a slot holding the executer
        case Kind::OneArg:
            return one(arg != nullptr ? arg : static_cast<void*>(&executer));
        default:
            return exec(executer);
        }
    }
};

void RunCall(void* context)
{
    auto* call   = static_cast<Call*>(context);
    call->result = call->Run();
}

struct Outcome
{
    bool           scoped = false;
    bool           rail   = false;
    bool           swapped = false;
    const void*    actor  = nullptr;
    HandleBytes    after{};                          // the seat's handle
    const wchar_t* beforeText = L"(no handle)";      // its first module, read before the call
    const wchar_t* afterText  = L"(no handle)";      // the same after it
};

/// The camera text of a class without a handle: its actor's flag, or "(no handle)".
const wchar_t* FlagText(const void* actor, const ExecClass& cls) noexcept
{
    if (cls.cameraFlag == 0 || actor == nullptr)
    {
        return L"(no handle)";
    }
    return ReadField<std::uint8_t>(actor, cls.cameraFlag) != 0 ? L"on (the actor's flag)" : L"off (the actor's flag)";
}

/// Runs `call` for `seat`'s executer of class `index`: the seat's handle in the actor, the primary scope and the
/// leader answer, and a LeaderScope where the installer reads the leader. Unscoped (a scope refused), the game's own
/// answer stands, still with the seat's handle in place.
Outcome RunAsSeat(int seat, std::size_t index, Call& call) noexcept
{
    const ExecClass& cls = kExecClasses[index];
    Outcome          out;
    void*            member  = players::generation::MemberOf(seat);
    HandleMap&       handles = g_handles[seat];
    void*            actor  = ObjectIn(call.executer, call.kind == Kind::Start ? cls.actorAtStart : cls.actorLater);
    if (actor == nullptr)
    {
        actor = ObjectIn(call.executer, kExecFactoryActor);  // Start before the copy; Tick/End once a copy is cleared
    }
    if (actor != nullptr && VTableOf(actor) != g_exec[index].actorVt)
    {
        actor = nullptr;  // not the class's actor: no swap, no rail test
    }
    out.actor             = actor;
    const auto actorWeak  = actor != nullptr ? scopes::leader_scope::WeakOf(actor) : 0;
    HandleBytes* mine     = nullptr;
    HandleBytes  theirs{};
    if (actorWeak != 0 && cls.handle != 0)
    {
        try
        {
            if (handles.size() >= kHandlePrune && !handles.contains(actorWeak))
            {
                std::erase_if(handles,
                              [](const auto& entry) { return scopes::leader_scope::ObjectAt(&entry.first) == nullptr; });
            }
            mine = &handles[actorWeak];
        }
        catch (...)
        {
            mine = nullptr;
        }
    }
    if (mine != nullptr)
    {
        out.beforeText = ModuleText(*mine);
        theirs         = ReadHandle(actor, cls);
        WriteHandle(actor, cls, *mine);
        out.swapped = true;
    }
    if (mine == nullptr)
    {
        out.beforeText = FlagText(actor, cls);
    }
    out.rail = actor != nullptr && ReadField<std::uint8_t>(actor, kActorRail) != 0;
    const bool installs = cls.installInTick ? call.kind == Kind::Tick : call.kind == Kind::Start;
    {
        const Inside                             inside;
        std::optional<scopes::leader_scope::LeaderScope> leader;
        if ((call.kind == Kind::Start && cls.leaderAtStart) || (installs && out.rail))
        {
            leader.emplace(scopes::leader_scope::WeakOf(member));
        }
        out.scoped = input::member_movement::InPrimaryScope(seat, member, &RunCall, &call);
        if (!out.scoped)
        {
            call.result = call.Run();
        }
    }
    if (mine != nullptr)
    {
        if (scopes::leader_scope::StillNames(actorWeak, actor))
        {
            *mine = ReadHandle(actor, cls);
            WriteHandle(actor, cls, theirs);
        }
        else
        {
            *mine = {};  // the actor went inside the call: its memory is the game's now
        }
        out.after     = *mine;
        out.afterText = ModuleText(out.after);
    }
    else if (actor != nullptr && scopes::leader_scope::StillNames(actorWeak, actor))
    {
        out.afterText = FlagText(actor, cls);
    }
    return out;
}

void LogStep(int seat, std::size_t index, const wchar_t* step, const Outcome& out)
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    const ExecClass&     cls  = kExecClasses[index];
    const wchar_t* const name = players::SeatName(seat);
    platform::LogDebug(L"traversal camera: {}'s {} on {} {}: {}{}; {}'s {} camera {} -> {}", name, cls.name,
                       ActorName(out.actor), step,
                       out.scoped ? std::format(L"as {}", name)
                                  : std::wstring(L"a scope was refused, the game's answer stands"),
                       out.rail ? L" (rail camera)" : L"", name, cls.name, out.beforeText, out.afterText);
}

std::uintptr_t ExecStep(int seat, std::size_t index, Kind kind, Call& call)
{
    const Outcome out = RunAsSeat(seat, index, call);
    if (index == kOneShot && kind == Kind::Tick && out.scoped && out.actor != nullptr &&
        ReadField<std::uint8_t>(out.actor, kExecClasses[kOneShot].cameraFlag) != 0)
    {
        // The seat's camera is on there.
        g_oneShotWeak[seat].store(scopes::leader_scope::WeakOf(const_cast<void*>(out.actor)), std::memory_order_release);
    }
    try
    {
        if (kind == Kind::Start)
        {
            g_exitLogged[seat]     = nullptr;
            g_exitRan[seat][index] = nullptr;
            if (index == kLadder)
            {
                g_ladderExit[seat]   = LadderExit{};
                g_ladderModule[seat] = reinterpret_cast<const void*>(out.after[0]);
            }
            LogStep(seat, index, L"starts", out);
            if (index == kOneShot && out.rail)
            {
                platform::Log(L"traversal camera: {}'s one-shot move on {} uses a rail camera: its off at the end "
                              L"reaches the primary camera's rail operator (not covered yet)",
                              players::SeatName(seat), ActorName(out.actor));
            }
        }
        else if (kind == Kind::End)
        {
            LogStep(seat, index,
                    kExecClasses[index].tick == TickRule::MoveExit && g_exitRan[seat][index] != call.executer
                        ? L"ends (its move's exit was never seen at state 3: the End switched the camera off)"
                        : L"ends",
                    out);
        }
        else if (index == kLadder ? g_exitLogged[seat] != call.executer
                                  : std::wstring_view(out.beforeText) != out.afterText)
        {
            g_exitLogged[seat] = call.executer;
            LogStep(seat, index, index == kLadder ? L"reaches its exit" : L"changes its camera in the tick", out);
        }
    }
    catch (...)
    {
    }
    return call.result;
}

template <std::size_t Index>
std::uintptr_t ExecStart(void* executer) noexcept
{
    const auto original = reinterpret_cast<ExecFn>(g_exec[Index].start.Original());
    const int  seat     = g_inside ? -1 : SeatOfExecuter(executer);
    if (seat < 1)
    {
        return original(executer);
    }
    Call call{Kind::Start, original, nullptr, executer};
    return ExecStep(seat, Index, Kind::Start, call);
}

template <std::size_t Index>
std::uintptr_t ExecEnd(void* executer) noexcept
{
    const auto original = reinterpret_cast<ExecFn>(g_exec[Index].end.Original());
    const int  seat     = g_inside ? -1 : SeatOfExecuter(executer);
    if (seat < 1)
    {
        return original(executer);
    }
    Call call{Kind::End, original, nullptr, executer};
    return ExecStep(seat, Index, Kind::End, call);
}

/// A seat's ladder module's exit: its sub-state machine's current or pending entry is S2, or the module is off.
struct ModuleExit
{
    bool         blending = false;  // S2 current or pending
    bool         off      = false;  // no module, gone, or inactive
    std::int32_t current = -1, pending = -1;
};

ModuleExit LadderModuleExit(int seat, const void* executer) noexcept
{
    ModuleExit        out;
    const HandleMap&  handles = g_handles[seat];
    void*             actor   = ObjectIn(executer, kExecClasses[kLadder].actorLater);
    const auto        found   = actor != nullptr && VTableOf(actor) == g_exec[kLadder].actorVt
                                    ? handles.find(scopes::leader_scope::WeakOf(actor))
                                    : handles.end();
    if (found == handles.end() || std::wstring_view(ModuleText(found->second)) != L"active")
    {
        out.off = true;
        return out;
    }
    const void* module = reinterpret_cast<const void*>(found->second[0]);
    const void* cur    = ReadPointer(module, kModuleStateCurrent);
    const void* pend   = ReadPointer(module, kModuleStatePending);
    out.current        = cur != nullptr ? ReadField<std::int32_t>(cur, 0) : -1;
    out.pending        = pend != nullptr ? ReadField<std::int32_t>(pend, 0) : -1;
    out.blending       = out.current == kLadderExitBlend || out.pending == kLadderExitBlend;
    return out;
}

/// A seat's ladder Tick at an exit phase. Returns nothing to do (the caller runs the Tick unscoped) or runs it.
std::optional<std::uintptr_t> LadderExitTick(int seat, Call& tick, std::uint8_t state) noexcept
{
    LadderExit&          exit = g_ladderExit[seat];
    const wchar_t* const name = players::SeatName(seat);
    if (exit.executer != tick.executer)
    {
        exit = LadderExit{tick.executer};
    }
    if (!exit.requested && g_requestCamExit != nullptr)
    {
        exit.requested = true;
        Call request{Kind::Request};
        request.request        = g_requestCamExit;
        request.executer       = tick.executer;
        request.state          = state;
        const Outcome    out   = RunAsSeat(seat, kLadder, request);
        const ModuleExit after = LadderModuleExit(seat, tick.executer);
        try
        {
            if (platform::Enabled(platform::Level::Debug))
            {
                platform::LogDebug(L"traversal camera: {}'s ladder on {} reaches its exit (climb state {}): the game's "
                                   L"exit blend asked for {}; module {}, sub-state {} pending {} (the blend starts on the "
                                   L"game's task: this Tick runs unscoped)",
                                   name, ActorName(out.actor), state,
                                   out.scoped ? std::format(L"as {}", name)
                                              : std::wstring(L"with a scope refused (the game's answer stands)"),
                                   ModuleText(out.after), after.current, after.pending);
            }
        }
        catch (...)
        {
        }
        return std::nullopt;
    }
    if (state == 11 || state == 12)
    {
        return std::nullopt;  // these exits switch no camera in the Tick: the blend does it all
    }
    const ModuleExit now = LadderModuleExit(seat, tick.executer);
    if (world::world_rules::LadderExitWaits(g_requestCamExit != nullptr, now.blending, now.off, exit.waited, kExitWait))
    {
        ++exit.waited;
        return std::nullopt;
    }
    if (!exit.scoped)
    {
        exit.scoped = true;
        try
        {
            platform::LogDebug(L"traversal camera: {}'s ladder exit's camera off runs as {} {}", name, name,
                               now.blending ? L"during the exit blend (it does nothing: the blend ends the camera)"
                               : now.off    ? L"with the camera already off"
                                            : L"WITHOUT the exit blend (the game's gate refused it): a cut, as player "
                                              L"1's would be");
        }
        catch (...)
        {
        }
    }
    return ExecStep(seat, kLadder, Kind::Tick, tick);
}

/// A seat's squeeze or crawl reaches its move's exit: the camera off the worker's delegate refused, as the seat.
void MoveExitCall(int seat, std::size_t index, void* executer) noexcept
{
    const wchar_t* const name = players::SeatName(seat);
    const ExecClass& cls = kExecClasses[index];
    Call             call{Kind::OneArg};
    call.executer = executer;
    if (cls.exitCall == ExitCall::SqueezeDelegate)
    {
        call.one = g_squeezeExit;  // the body takes the delegate's capture: a slot holding the executer
    }
    else
    {
        call.one = g_cameraOff;
        call.arg = ObjectIn(executer, kCrawlExitActor);
    }
    if (call.one == nullptr || (cls.exitCall == ExitCall::CrawlCameraOff && call.arg == nullptr))
    {
        try
        {
            platform::LogDebug(L"traversal camera: {}'s {} reaches its exit: {}; its camera ends at the End", name,
                               cls.name, call.one == nullptr ? L"the exit call is unavailable" : L"no actor at the exit");
        }
        catch (...)
        {
        }
        return;
    }
    const Outcome out = RunAsSeat(seat, index, call);
    try
    {
        if (platform::Enabled(platform::Level::Debug))
        {
            platform::LogDebug(L"traversal camera: {}'s {} on {} reaches its exit: the game's exit camera off ran {}; "
                               L"{}'s {} camera {} -> {}",
                               name, cls.name, ActorName(out.actor),
                               out.scoped ? std::format(L"as {}", name)
                                          : std::wstring(L"with a scope refused (the game's answer stands)"),
                               name, cls.name, out.beforeText, out.afterText);
        }
    }
    catch (...)
    {
    }
}

template <std::size_t Index>
std::uintptr_t ExecTick(void* executer, double dt, void* third, std::int64_t fourth) noexcept
{
    const auto original = reinterpret_cast<TickFn>(g_exec[Index].tick.Original());
    const int  seat     = g_inside ? -1 : SeatOfExecuter(executer);
    if (seat < 1)
    {
        return original(executer, dt, third, fourth);
    }
    if (kExecClasses[Index].tick == TickRule::MoveExit)
    {
        const void* move = ReadPointer(executer, kExecClasses[Index].exitMove);
        if (g_exitRan[seat][Index] != executer && move != nullptr &&
            ReadField<std::uint8_t>(move, kMoveExitState) == kMoveAtExit)
        {
            g_exitRan[seat][Index] = executer;
            MoveExitCall(seat, Index, executer);
        }
        return original(executer, dt, third, fourth);
    }
    Call call{Kind::Tick, nullptr, original, executer, dt, third, fourth};
    if (kExecClasses[Index].tick == TickRule::LadderExit)
    {
        const void* move  = ReadPointer(executer, kLadderClimbMove);
        const auto  state = move != nullptr ? ReadField<std::uint8_t>(move, kClimbMoveState) : std::uint8_t{0};
        if (!ExitState(state))
        {
            return original(executer, dt, third, fourth);
        }
        const auto ran = LadderExitTick(seat, call, state);
        return ran ? *ran : original(executer, dt, third, fourth);
    }
    return ExecStep(seat, Index, Kind::Tick, call);
}

struct LeaveCall
{
    LeaveSlotFn    leave;
    void*          actor;
    void*          character;
    std::uintptr_t result = 0;
};

void RunLeave(void* context)
{
    auto* call   = static_cast<LeaveCall*>(context);
    call->result = call->leave(call->actor, call->character);
}

/// The seat whose one-shot move `actor` is leaving: by its member (`character`), or with no character by the actor the
/// seat's camera went on. -1 for everyone else's. Any thread.
int OneShotSeat(const void* actor, const void* character) noexcept
{
    for (int seat = 1; actor != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        const void* member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        if (member == nullptr)
        {
            continue;
        }
        const auto at = static_cast<std::size_t>(seat);
        if (character != nullptr ? character == member
                                 : scopes::leader_scope::StillNames(g_oneShotWeak[at].load(std::memory_order_acquire), actor))
        {
            return seat;
        }
    }
    return -1;
}

/// The one-shot actor's leave (any thread): for a seat's member, or with no character the actor a seat's camera went
/// on, the game's leave ends the camera on that seat's camera manager (climb_context's WithSeatCamera). Everyone
/// else's is the game's.
std::uintptr_t OneShotLeave(void* actor, void* character) noexcept
{
    const auto original = reinterpret_cast<LeaveSlotFn>(g_oneShotLeave.Original());
    const int  seat     = OneShotSeat(actor, character);
    if (seat < 0)
    {
        return original(actor, character);
    }
    std::atomic<std::uint64_t>& weak = g_oneShotWeak[static_cast<std::size_t>(seat)];
    const wchar_t* const        name = players::SeatName(seat);
    const auto flag = kExecClasses[kOneShot].cameraFlag;
    const bool was  = ReadField<std::uint8_t>(actor, flag) != 0;
    LeaveCall  call{original, actor, character};
    const bool ran  = input::climb_context::WithSeatCamera(seat, &RunLeave, &call);
    if (!ran)
    {
        // The seat's camera manager is gone (its player left): no camera of theirs to end. The leave's own last step
        // only, so a later leave of this actor cannot reach player 1's slot 15.
        WriteField<std::uint8_t>(actor, flag, 0);
    }
    std::uint64_t recorded = weak.load(std::memory_order_acquire);
    if (scopes::leader_scope::StillNames(recorded, actor))
    {
        weak.compare_exchange_strong(recorded, 0, std::memory_order_acq_rel);
    }
    try
    {
        platform::LogDebug(L"traversal camera: {}'s one-shot move ends ({} thread{}): {}", name,
                           players::generation::OnGameThread() ? L"game" : L"animation",
                           character == nullptr ? L", the actor's EndPlay" : L"",
                           !was  ? L"its camera flag was already off (no camera, or ended before)"
                           : ran ? L"the game's leave ran on the seat's own camera; player 1's untouched"
                                 : L"the seat's camera manager is gone: the flag cleared, player 1's camera untouched");
    }
    catch (...)
    {
    }
    return call.result;
}

// --- placement ------------------------------------------------------------------------------------------------------

template <std::size_t Index>
std::wstring PlaceExec(const platform::GameImage& image)
{
    const ExecClass& cls  = kExecClasses[Index];
    ExecLive&        live = g_exec[Index];
    if (cls.handleSize > kMaxHandle || cls.handleSize % 8 != 0)
    {
        return std::format(L"{} NO (its camera handle does not fit the seats' copy)", cls.name);
    }
    live.actorVt          = Live(image, cls.actorVt, 0x740);
    if (cls.start.slot != 0)
    {
        live.startPlaced =
            platform::ReplaceSlot(image, cls.start, reinterpret_cast<std::uintptr_t>(&ExecStart<Index>), live.start);
    }
    if (cls.end.slot != 0)
    {
        live.endPlaced =
            platform::ReplaceSlot(image, cls.end, reinterpret_cast<std::uintptr_t>(&ExecEnd<Index>), live.end);
    }
    if (cls.tickSlot.slot != 0)
    {
        live.tickPlaced =
            platform::ReplaceSlot(image, cls.tickSlot, reinterpret_cast<std::uintptr_t>(&ExecTick<Index>), live.tick);
    }
    const bool ok = live.actorVt != 0 && (cls.start.slot == 0 || live.startPlaced) &&
                    (cls.end.slot == 0 || live.endPlaced) && (cls.tickSlot.slot == 0 || live.tickPlaced);
    return std::format(L"{} {}", cls.name, ok ? L"yes" : L"NO");
}

template <std::size_t... I>
std::wstring PlaceAllExec(const platform::GameImage& image, std::index_sequence<I...>)
{
    std::wstring text;
    ((text += (text.empty() ? L"" : L", ") + PlaceExec<I>(image)), ...);
    return text;
}

}  // namespace

namespace executers {

Placement Place(const platform::GameImage& image)
{
    Placement out;
    out.classes          = PlaceAllExec(image, std::make_index_sequence<kExecCount>{});
    g_oneShotLeavePlaced = g_exec[kOneShot].tickPlaced &&
                           platform::ReplaceSlot(image, kOneShotLeaveSlot,
                                                 reinterpret_cast<std::uintptr_t>(&OneShotLeave), g_oneShotLeave);
    const auto request = platform::FindFunction(image, kRequestCamExit);
    g_requestCamExit   = request ? reinterpret_cast<RequestFn>(*request) : nullptr;
    const auto squeeze = platform::FindFunction(image, kSqueezeExit);
    g_squeezeExit      = squeeze ? reinterpret_cast<OneArgFn>(*squeeze) : nullptr;
    const auto off     = platform::FindFunction(image, kCameraOff);
    g_cameraOff        = off ? reinterpret_cast<OneArgFn>(*off) : nullptr;
    out.oneShotLeave   = g_oneShotLeavePlaced;
    out.ladderExit     = g_requestCamExit != nullptr;
    out.squeezeExit    = g_squeezeExit != nullptr;
    out.crawlOff       = g_cameraOff != nullptr;
    return out;
}

bool Remove() noexcept
{
    bool kept = false;
    for (ExecLive& live : g_exec)
    {
        kept |= live.startPlaced && !live.start.Restore();
        kept |= live.endPlaced && !live.end.Restore();
        kept |= live.tickPlaced && !live.tick.Restore();
        live.startPlaced = live.endPlaced = live.tickPlaced = false;
    }
    kept |= g_oneShotLeavePlaced && !g_oneShotLeave.Restore();
    g_oneShotLeavePlaced = false;
    for (auto& weak : g_oneShotWeak)
    {
        weak.store(0, std::memory_order_release);
    }
    return kept;
}

void Tick(int seat, bool active) noexcept
{
    if (!active && players::SeatIndex(seat))
    {
        g_ladderModule[seat] = nullptr;
        g_oneShotWeak[seat].store(0, std::memory_order_release);
    }
}

}  // namespace executers

std::uintptr_t LadderInput(LadderInputFn original, void* executer, float dt) noexcept
{
    const int seat = g_exec[kLadder].actorVt == 0 || g_inside ? -1 : SeatOfExecuter(executer);
    if (seat < 1)
    {
        const auto result = original(executer, dt);
        // Player 1's slide flag, for the log (a seat on the same ladder: the seat's input must not change it).
        void* one = players::generation::AnyActive() && players::generation::OnGameThread() ? players::generation::PlayerOneController() : nullptr;
        if (one != nullptr && g_exec[kLadder].actorVt != 0 &&
            ObjectIn(executer, kExecCharacter) == ReadPointer(one, facts::kControllerPawn))
        {
            void* actor = ObjectIn(executer, kExecClasses[kLadder].actorLater);
            if (actor != nullptr && VTableOf(actor) == g_exec[kLadder].actorVt)
            {
                NoteSlide(0, SlideFlag(ReadHandle(actor, kExecClasses[kLadder])));
            }
        }
        return result;
    }
    // RE-p2-traversal-worker-gates.md W4: the input step sets the ladder module's slide flag (+0x434, a wider FOV while
    // sliding) through the handle on the ladder. Window B already holds the seat's scopes (traversal_watch opens
    // player 2's); only the seat's handle is put in, so the write reaches its camera and leaves player 1's flag alone.
    const ExecClass& cls     = kExecClasses[kLadder];
    HandleMap&       handles = g_handles[seat];
    void*            actor   = ObjectIn(executer, cls.actorLater);
    const auto       found   = actor != nullptr && VTableOf(actor) == g_exec[kLadder].actorVt
                                   ? handles.find(scopes::leader_scope::WeakOf(actor))
                                   : handles.end();
    if (found == handles.end())
    {
        return original(executer, dt);
    }
    const auto        actorWeak = found->first;
    const HandleBytes theirs    = ReadHandle(actor, cls);
    WriteHandle(actor, cls, found->second);
    std::uintptr_t result = 0;
    {
        const Inside inside;
        result = original(executer, dt);
    }
    if (scopes::leader_scope::StillNames(actorWeak, actor))
    {
        found->second = ReadHandle(actor, cls);
        WriteHandle(actor, cls, theirs);
    }
    else
    {
        found->second = {};
    }
    NoteSlide(seat, SlideFlag(found->second));
    return result;
}

std::uintptr_t LadderStart(ExecFn original, void* executer) noexcept
{
    const int seat = g_exec[kLadder].actorVt == 0 || g_inside ? -1 : SeatOfExecuter(executer);
    if (seat < 1)
    {
        return original(executer);
    }
    Call call{Kind::Start, original, nullptr, executer};
    return ExecStep(seat, kLadder, Kind::Start, call);
}

std::uintptr_t LadderEnd(ExecFn original, void* executer) noexcept
{
    const int seat = g_exec[kLadder].actorVt == 0 || g_inside ? -1 : SeatOfExecuter(executer);
    if (seat < 1)
    {
        return original(executer);
    }
    Call call{Kind::End, original, nullptr, executer};
    return ExecStep(seat, kLadder, Kind::End, call);
}

}  // namespace lss::traversal::traversal_camera
