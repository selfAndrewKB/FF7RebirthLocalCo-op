#include "feedback/combat_rumble.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/dev_switch.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/frame_watch.h"
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
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace lss::feedback::combat_rumble {

namespace seat_ladder = scopes::seat_ladder;

namespace {

using Clock = std::chrono::steady_clock;

// research/RE-p2-rumble.md (this build).
// BattleHit_ApplyReaction_CamFxAndFFB: the leader gate (cl kind, rdx attacker, r8 target), the camera effect
// on the primary camera manager, and the primary-controller fallback when the attacker has no player controller.
constexpr std::uintptr_t kGateCall     = 0x14131FE21;
constexpr std::uintptr_t kGate         = 0x14169AE88;  // BattleHitFx_ShouldPlay_LeaderInvolved
constexpr std::uintptr_t kCameraCall   = 0x14131FE78;
constexpr std::uintptr_t kCamera       = 0x1416FAE10;  // PrimaryPCM_CameraEffect2DF0_Play
constexpr std::uintptr_t kFallbackCall = 0x14131FEA2;
using facts::kGetPrimaryEndPlayerController;
// 1.005 (research/RE-1005-field-2.md): the fallback call sits at %16 = 12, so it crosses a 16-byte block. There the
// three calls are changed together from the game thread's first tick (InstallFromOwnThread), outside every hit: the
// hit reaction runs on the game thread [ASSUMPTION: battle damage is applied in the world tick; the gate's leader
// scopes already rely on it, and Gate warns if a hit ever arrives off it].

// The force-feedback notifies (SE End classes; research/RE-p2-rumble.md): each class's vtable
// slot, the function it holds, its GetFirstPlayerController call site.
enum class Phase : std::uint8_t
{
    Notify,
    Begin,
    Tick,
    End,
};
constexpr const wchar_t* kPhaseText[] = {L"Notify", L"Begin", L"Tick", L"End"};

struct NotifyFact
{
    const wchar_t* what;
    std::uintptr_t slot;
    std::uintptr_t function;
    std::uintptr_t firstControllerCall;
    Phase          phase;
};
constexpr std::array kNotifies = {
    NotifyFact{L"EndForceFeedback", 0x14668D6C0 + 0x278, 0x14325F71C, 0x14325F752, Phase::Notify},
    NotifyFact{L"EndForceFeedbackAdvanced", 0x1458898E8 + 0x278, 0x14184709C, 0x1418470D2, Phase::Notify},
    NotifyFact{L"EndForceFeedbackRange", 0x1457FA8B0 + 0x278, 0x1411F6B30, 0x1411F6B6B, Phase::Begin},
    NotifyFact{L"EndForceFeedbackRange", 0x1457FA8B0 + 0x280, 0x1411F7B64, 0x1411F7BB9, Phase::Tick},
    NotifyFact{L"EndForceFeedbackRange", 0x1457FA8B0 + 0x288, 0x1411F6EF4, 0x1411F6F41, Phase::End},
    NotifyFact{L"EndForceFeedbackRangeAdvanced", 0x14589EE58 + 0x278, 0x141846EEC, 0x141846F3C, Phase::Begin},
    NotifyFact{L"EndForceFeedbackRangeAdvanced", 0x14589EE58 + 0x280, 0x14181AD44, 0x14181AD87, Phase::Tick},
    NotifyFact{L"EndForceFeedbackRangeAdvanced", 0x14589EE58 + 0x288, 0x14181ACC0, 0x14181AD0C, Phase::End},
};
constexpr std::uint32_t kComponentOwner = 0xA0;  // UActorComponent::OwnerPrivate

#if LSS_DEV
// The plays, for the log (dev tools): EndFFBComp_PlayChannel (the dispatcher) and its classic path (also called
// directly).
constexpr std::uint8_t   kPlayBytes[]        = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr platform::FunctionFact kPlay{L"EndFFBComp_PlayChannel", 0x1411F71E8, kPlayBytes};
constexpr std::uint8_t   kPlayClassicBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20};
constexpr platform::FunctionFact kPlayClassic{L"EndFFBComp_PlayChannel's classic path", 0x1411F7244, kPlayClassicBytes};
constexpr std::uintptr_t kFromDispatcher     = 0x1411F7234;  // the dispatcher's own call of the classic path, +5
constexpr std::uint32_t  kComponentController = 0x10;        // the End force-feedback component's controller

struct Source
{
    std::uintptr_t returnAddress;
    const wchar_t* what;
};
constexpr Source kSources[] = {
    {0x14131FF12, L"battle hit"},
    {0x1416FADE5, L"guard"},
    {0x140CB3815, L"ATB gain on hit"},
    {0x1413243E9, L"Materia ATB effect"},
    {0x142242DD1, L"Materia ATB effect (task)"},
    {0x1412F376F, L"field hit"},
    {0x140C07E34, L"item get"},
    {0x1408A18D1, L"rumble list (traversal)"},
    {0x14325F7A5, L"montage notify"},
    {0x141847117, L"montage notify (advanced)"},
    {0x1411F7122, L"montage range notify"},
    {0x1408A264E, L"cutscene clip"},
    {0x1434973C6, L"cutscene track"},
    {0x1434AFE70, L"Niagara effect"},
    {0x1411F6EE1, L"minigame"},
};
// Six arguments, two on the stack: rcx component, dl channel, r8 FName*, r9b, [rsp+0x28] u8,
// [rsp+0x30] u8*. Returns a byte.
using PlayFn = std::uint64_t (*)(void* component, std::uint64_t channel, const void* name, std::uint64_t r9,
                                 std::uint64_t fifth, void* sixth);
#endif

constexpr auto kStatusEvery = std::chrono::seconds(10);

using GateFn      = std::uint64_t (*)(std::uint64_t kind, void* attacker, void* target, void* functor);
using CameraFn    = void* (*)(void* handle, const void* location, const void* name);
using FallbackFn  = void* (*)();
using FirstFn     = void* (*)(void* world);
using Notify3Fn   = void (*)(void* notify, void* mesh, void* animation);
using Notify4Fn   = void (*)(void* notify, void* mesh, void* animation, float value);

// Set once at install.
platform::CallRedirect                                   g_gateCall, g_cameraCall, g_fallbackCall;
std::array<platform::CallRedirect, kNotifies.size()>     g_firstCalls;
std::array<std::uintptr_t, kNotifies.size()>             g_firstSites{};
std::array<platform::VTableEntry, kNotifies.size()>      g_notifySlots;
#if LSS_DEV
platform::EntryRedirect                                  g_play, g_playClassic;
#endif
std::uintptr_t                                           g_firstController = 0;
std::optional<platform::GameImage>                       g_image;      // live addresses as research IDA addresses
std::atomic<bool>                                        g_sitesTried{false};  // the first-controller calls placed
// 1.005: the battle-hit calls, installed by the first BeforeTick (sites and targets live; all three or none).
struct HitSite
{
    platform::CallRedirect* redirect;
    std::uintptr_t          site, target, replacement;
};
std::array<HitSite, 3> g_hitSites{};
std::atomic<bool>       g_hitsPending   {false};
std::atomic<bool>      g_hitsOwnThread{false};  // installed with InstallFromOwnThread (the fallback crosses a block)
unsigned                                                 g_notifiesWrapped = 0;

// Game thread.
// The seat whose member the last gate ran for, when it passed (0: none). Read by the same hit's camera and fallback
// calls, a few instructions later; between the gate and them run only the target AI component's effect-name listeners
// (an effect-name lookup before the camera, IsEndPlayerController before the fallback), which replace an effect name
// and are not known to start another hit [ASSUMPTION]. A nested hit there would overwrite it.
int   g_hitSeat          = 0;
void* g_notifyController = nullptr;  // a seat's controller while a notify for the seat runs
platform::Every g_status{kStatusEvery};
// Per seat; index 0 holds player 1's counts (the One fields) and the plays on other controllers.
struct Counts
{
    std::uint32_t hitsOne = 0, seatHits = 0, seatGates = 0, seatCameras = 0, seatFallbacks = 0, hitSkips = 0;
    std::uint32_t notifiesOne = 0, seatNotifies = 0, notifySkips = 0, answered = 0;
    std::uint32_t playsOne = 0, seatPlays = 0, playsOther = 0;
};
std::array<Counts, players::kMaxPlayers> g_counts{};

bool Ready() noexcept
{
    return players::generation::OnGameThread() && players::generation::AnyActive();
}

/// The built active seat whose member, or (with `pawns`) whose controller's pawn, is `actor`, or 0.
int SeatOfCharacter(const void* actor, bool pawns) noexcept
{
    for (int seat = 1; actor != nullptr && seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat))
        {
            continue;
        }
        if (actor == players::generation::MemberOf(seat) ||
            (pawns && actor == players::actor_fields::Pawn(players::generation::ControllerOf(seat))))
        {
            return seat;
        }
    }
    return 0;
}

/// A Debug line may be written (the level is on).
bool Line() noexcept
{
    return platform::Enabled(platform::Level::Debug);
}

void* PlayerOnePawn() noexcept
{
    return players::actor_fields::Pawn(players::generation::PlayerOneController());
}

/// `seat`'s leader view (leader, leader type, primary controller) for one call; nothing held if any part refuses (the
/// ladder's all-or-nothing rule).
class SeatView
{
public:
    SeatView(int seat, bool primary) noexcept
    {
        seat_ladder::Args args;
        args.seat = seat;
        if (seat_ladder::FillMember(args, players::generation::MemberOf(seat)))
        {
            scopes_.emplace((primary ? seat_ladder::Primary : 0u) | seat_ladder::Leader | seat_ladder::LeaderType,
                            args);
        }
    }

    bool Held() const noexcept { return scopes_ && scopes_->Held(); }

private:
    std::optional<seat_ladder::SeatScopes> scopes_;
};

// ---------------------------------------------------------------------------------------------
// Battle hits
// ---------------------------------------------------------------------------------------------

std::uint64_t Gate(std::uint64_t kind, void* attacker, void* target, void* functor) noexcept
{
    const auto original = reinterpret_cast<GateFn>(g_gateCall.Original());
    g_hitSeat           = 0;
    // On 1.005 the fallback call was changed on the understanding that only the game thread runs a hit (call_redirect.h).
    static std::atomic<unsigned> offThread{0};
    if (g_hitsOwnThread.load(std::memory_order_relaxed) && !players::generation::OnGameThread() &&
        offThread.fetch_add(1, std::memory_order_relaxed) < 5)
    {
        platform::Log(L"WARNING: combat rumble: a battle hit ran off the game thread; its fallback call was changed on the "
                      L"understanding that only the game thread runs it");
    }
    if (!Ready())
    {
        return original(kind, attacker, target, functor);
    }
    void*      one    = PlayerOnePawn();
    const bool isOne  = one != nullptr && (attacker == one || target == one);
    // Player 1's character on either side keeps the game's own answer (party hits are enemy against party). Else the
    // seat whose member attacks, or is hit.
    int seat = 0;
    if (!isOne)
    {
        seat = SeatOfCharacter(attacker, false);
        seat = seat != 0 ? seat : SeatOfCharacter(target, false);
    }
    if (seat == 0)
    {
        const auto result = original(kind, attacker, target, functor);
        if (isOne)
        {
            ++g_counts[0].hitsOne;
            if (Line())
            {
                platform::LogDebug(L"combat rumble: frame {}: battle hit (gate kind {}), attacker {}, target {}: player 1's, gate {}",
                              platform::frame_watch::FrameIndex(), kind & 0xFF, players::Who(attacker), players::Who(target),
                              (result & 0xFF) != 0 ? L"passed" : L"closed");
            }
        }
        return result;
    }
    Counts& counts = g_counts[seat];
    ++counts.seatHits;
    std::uint64_t result = 0;
    bool          held   = false;
    {
        const SeatView view(seat, false);  // the gate reads the leader and its type only
        held   = view.Held();
        result = original(kind, attacker, target, functor);
    }
    g_hitSeat = held && (result & 0xFF) != 0 ? seat : 0;
    counts.seatGates += g_hitSeat != 0 ? 1 : 0;
    counts.hitSkips += held ? 0 : 1;
    if (Line())
    {
        // The seat's name as its own argument: nothing extra is built for the line.
        const wchar_t* const name   = players::SeatName(seat);
        const wchar_t* const passed = (result & 0xFF) != 0 ? L"passed" : L"closed";
        if (held)
        {
            platform::LogDebug(L"combat rumble: frame {}: battle hit (gate kind {}), attacker {}, target {}: {}'s (the "
                               L"gate asked with {}'s member as the leader), gate {}",
                               platform::frame_watch::FrameIndex(), kind & 0xFF, players::Who(attacker), players::Who(target), name, name,
                               passed);
        }
        else
        {
            platform::LogDebug(L"combat rumble: frame {}: battle hit (gate kind {}), attacker {}, target {}: {}'s ({}'s "
                               L"leader view REFUSED: the game's own answer), gate {}",
                               platform::frame_watch::FrameIndex(), kind & 0xFF, players::Who(attacker), players::Who(target), name, name,
                               passed);
        }
    }
    return result;
}

/// The hit's camera effect (PrimaryPCM_CameraEffect2DF0_Play: rcx the out handle, returned; rdx the location; r8 the
/// effect's FName*), which finds "the" camera manager through the primary controller: the seat's for the seat's hit.
void* Camera(void* handle, const void* location, const void* name) noexcept
{
    const auto original = reinterpret_cast<CameraFn>(g_cameraCall.Original());
    const int  seat     = g_hitSeat;
    if (seat == 0 || !Ready())
    {
        return original(handle, location, name);
    }
    const scopes::primary_controller::Scope primary(seat);
    if (!primary.Held())
    {
        ++g_counts[seat].hitSkips;
        return original(handle, location, name);
    }
    void* result = original(handle, location, name);
    ++g_counts[seat].seatCameras;
    if (Line())
    {
        const bool played = handle != nullptr && ReadField<std::int32_t>(handle, 0) != -1;  // {-1, 0} when nothing plays
        const wchar_t* const who = players::SeatName(seat);
        platform::LogDebug(L"combat rumble: frame {}: the hit's camera effect {} on {}'s camera manager{}{}{}",
                           platform::frame_watch::FrameIndex(),
                           players::unreal::NameText(name != nullptr ? ReadField<std::uint64_t>(name, 0) : 0), who,
                           played ? L"" : L": NOTHING PLAYED (", played ? L"" : who,
                           played ? L"" : L"'s camera manager has no effect component?)");
    }
    return result;
}

/// The hit's controller when the attacker has none (GetPrimaryEndPlayerController_FromPartyManager, no arguments):
/// the seat's for the seat's hit.
void* Fallback() noexcept
{
    const auto original = reinterpret_cast<FallbackFn>(g_fallbackCall.Original());
    const int  seat     = g_hitSeat;
    if (seat == 0 || !Ready())
    {
        return original();
    }
    const scopes::primary_controller::Scope primary(seat);
    if (!primary.Held())
    {
        ++g_counts[seat].hitSkips;
        return original();
    }
    ++g_counts[seat].seatFallbacks;
    return original();
}

// ---------------------------------------------------------------------------------------------
// Notifies
// ---------------------------------------------------------------------------------------------

/// Opens a seat's view around a notify on the seat's mesh (its member's, or its controller's pawn's: the seat from the
/// mesh's owner), and makes its first-controller calls answer the seat's controller.
class NotifyScope
{
public:
    NotifyScope(std::size_t index, void* mesh) noexcept : index_(index)
    {
        // Without its first-controller call answering the seat's, an open gate would play the seat's notify on player
        // 1's pad.
        if (!Ready() || !g_firstCalls[index].IsInstalled())
        {
            return;
        }
        void*       owner = ReadPointer(mesh, kComponentOwner);
        const int   seat  = SeatOfCharacter(owner, true);
        const Phase phase = kNotifies[index].phase;
        if (seat == 0)
        {
            if (owner != nullptr && owner == PlayerOnePawn())
            {
                ++g_counts[0].notifiesOne;
                if (phase != Phase::Tick && Line())
                {
                    platform::LogDebug(L"combat rumble: frame {}: {} {} on player 1's character's mesh: the game's own (PC1)",
                                  platform::frame_watch::FrameIndex(), kNotifies[index].what,
                                  kPhaseText[static_cast<int>(phase)]);
                }
            }
            return;
        }
        Counts& counts = g_counts[seat];
        ++counts.seatNotifies;
        view_.emplace(seat, true);
        if (!view_->Held())
        {
            view_.reset();
            ++counts.notifySkips;
            if (phase != Phase::Tick && Line())
            {
                const wchar_t* const name = players::SeatName(seat);
                platform::LogDebug(L"combat rumble: frame {}: {} {} on {}'s mesh: {}'s leader view REFUSED (the "
                                   L"game's own: player 1's controller or none)",
                                   platform::frame_watch::FrameIndex(), kNotifies[index].what,
                                   kPhaseText[static_cast<int>(phase)], name, name);
            }
            return;
        }
        saved_             = g_notifyController;
        g_notifyController = players::generation::ControllerOf(seat);
        answeredAt_        = counts.answered;
        owner_             = owner;
        seat_              = seat;
        active_            = true;
    }

    ~NotifyScope()
    {
        if (!active_)
        {
            return;
        }
        g_notifyController = saved_;
        const Phase phase  = kNotifies[index_].phase;
        const auto  calls  = g_counts[seat_].answered - answeredAt_;
        if (phase != Phase::Tick && Line())
        {
            try
            {
                const wchar_t* const name = players::SeatName(seat_);
                platform::LogDebug(L"combat rumble: frame {}: {} {} on {}'s mesh: {}",
                                   platform::frame_watch::FrameIndex(), kNotifies[index_].what,
                                   kPhaseText[static_cast<int>(phase)],
                                   owner_ == players::generation::MemberOf(seat_) ? std::format(L"{}'s member", name)
                                                                         : std::format(L"{}'s ridden pawn", name),
                                   calls != 0 ? std::format(L"on {}'s controller (its first-controller call answered "
                                                            L"PC{})",
                                                            name, seat_ + 1)
                                              : std::format(L"nothing played (the notify's own gate closed with {} as "
                                                            L"the leader)",
                                                            name));
            }
            catch (...)
            {
            }
        }
    }

    NotifyScope(const NotifyScope&)            = delete;
    NotifyScope& operator=(const NotifyScope&) = delete;

private:
    std::size_t             index_      = 0;
    std::optional<SeatView> view_;
    void*                   saved_      = nullptr;
    const void*             owner_      = nullptr;
    std::uint32_t           answeredAt_ = 0;
    int                     seat_       = 0;
    bool                    active_     = false;
};

template <std::size_t I>
void Notify3(void* notify, void* mesh, void* animation) noexcept
{
    const NotifyScope scope(I, mesh);
    reinterpret_cast<Notify3Fn>(g_notifySlots[I].Original())(notify, mesh, animation);
}

template <std::size_t I>
void Notify4(void* notify, void* mesh, void* animation, float value) noexcept
{
    const NotifyScope scope(I, mesh);
    reinterpret_cast<Notify4Fn>(g_notifySlots[I].Original())(notify, mesh, animation, value);
}

template <std::size_t I>
constexpr std::uintptr_t WrapperOf() noexcept
{
    constexpr Phase phase = kNotifies[I].phase;
    if constexpr (phase == Phase::Begin || phase == Phase::Tick)
    {
        return reinterpret_cast<std::uintptr_t>(&Notify4<I>);
    }
    else
    {
        return reinterpret_cast<std::uintptr_t>(&Notify3<I>);
    }
}

template <std::size_t... I>
std::array<std::uintptr_t, sizeof...(I)> Wrappers(std::index_sequence<I...>) noexcept
{
    return {WrapperOf<I>()...};
}

/// A notify's GetFirstPlayerController call: a seat's controller while a notify for the seat runs.
void* FirstController(void* world) noexcept
{
    if (g_notifyController != nullptr && Ready())
    {
        const int seat = players::generation::SeatOfController(g_notifyController);
        ++g_counts[seat >= 1 ? seat : 0].answered;
        return g_notifyController;
    }
    return reinterpret_cast<FirstFn>(g_firstController)(world);
}

// ---------------------------------------------------------------------------------------------
// Plays (log only; dev tools)
// ---------------------------------------------------------------------------------------------

#if LSS_DEV
const wchar_t* SourceOf(std::uintptr_t returnAddress) noexcept
{
    const auto ida = g_image ? g_image->ResearchAddress(returnAddress) : returnAddress;
    for (const Source& source : kSources)
    {
        if (source.returnAddress == ida)
        {
            return source.what;
        }
    }
    return nullptr;
}

void LogPlay(const void* component, std::uint64_t channel, const void* name, std::uintptr_t returnAddress) noexcept
{
    if (!Ready())
    {
        return;
    }
    const void* controller = ReadPointer(component, kComponentController);
    const int   seat       = players::generation::SeatOfController(controller);  // 0 player 1's, a seat's, or -1
    (seat == 0  ? g_counts[0].playsOne
     : seat > 0 ? g_counts[seat].seatPlays
                : g_counts[0].playsOther) += 1;
    if (!Line())
    {
        return;
    }
    try
    {
        const auto*         source = SourceOf(returnAddress);
        const std::uint64_t key    = name != nullptr ? ReadField<std::uint64_t>(name, 0) : 0;
        platform::LogDebug(L"combat rumble: frame {}: play {} channel {} on {} from {}", platform::frame_watch::FrameIndex(),
                      players::unreal::NameText(key), channel & 0xFF, players::Who(controller),
                      source != nullptr ? std::wstring(source) : std::format(L"0x{:X}", g_image ? g_image->ResearchAddress(returnAddress) : returnAddress));
    }
    catch (...)
    {
    }
}

std::uint64_t Play(void* component, std::uint64_t channel, const void* name, std::uint64_t r9, std::uint64_t fifth,
                   void* sixth) noexcept
{
    LogPlay(component, channel, name, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    return reinterpret_cast<PlayFn>(g_play.Original())(component, channel, name, r9, fifth, sixth);
}

std::uint64_t PlayClassic(void* component, std::uint64_t channel, const void* name, std::uint64_t r9,
                          std::uint64_t fifth, void* sixth) noexcept
{
    const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (!g_image || g_image->ResearchAddress(from) != kFromDispatcher)  // the dispatcher's own call is logged by Play
    {
        LogPlay(component, channel, name, from);
    }
    return reinterpret_cast<PlayFn>(g_playClassic.Original())(component, channel, name, r9, fifth, sixth);
}

/// Places the two play entries; what the install line says of them.
const wchar_t* InstallPlays(const platform::GameImage& image) noexcept
{
    const auto play    = platform::FindFunction(image, kPlay);
    const auto classic = platform::FindFunction(image, kPlayClassic);
    const bool plays = play && classic &&
                       g_play.Install(*play, image.EntryInstruction(kPlay.address, kPlay.entryBytes),
                                      reinterpret_cast<std::uintptr_t>(&Play)) &&
                       g_playClassic.Install(*classic,
                                             image.EntryInstruction(kPlayClassic.address, kPlayClassic.entryBytes),
                                             reinterpret_cast<std::uintptr_t>(&PlayClassic));
    return plays ? L"logged with their source" : L"NOT logged (an entry is not what the research read)";
}
#else
const wchar_t* InstallPlays(const platform::GameImage&) noexcept { return L"not logged (dev tools)"; }
#endif

/// The status line: one per active seat, each with player 1's counts (taken once for all of them).
void LogStatus() noexcept
{
    const auto now = Clock::now();
    if (!players::generation::AnyActive() || !g_status.Due(now))
    {
        return;
    }
    const auto    all = std::exchange(g_counts, {});
    const Counts& one = all[0];
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const Counts& c = all[seat];
        if (!players::generation::Active(seat) ||
            one.hitsOne + c.seatHits + one.notifiesOne + c.seatNotifies + one.playsOne + c.seatPlays + one.playsOther ==
                0)
        {
            continue;
        }
        const wchar_t* const name = players::SeatName(seat);
        platform::LogDebug(L"combat rumble (10 s): battle hits player 1's {}, {}'s {} (gate passed {}, camera on {}'s "
                           L"{}, fallback to {}'s controller {}, refused {}); notifies player 1's {}, {}'s {} (first-"
                           L"controller calls answered PC{} {}, refused {}); plays on PC1 {}, PC{} {}, other {}",
                           one.hitsOne, name, c.seatHits, c.seatGates, name, c.seatCameras, name, c.seatFallbacks,
                           c.hitSkips, one.notifiesOne, name, c.seatNotifies, seat + 1, c.answered, c.notifySkips,
                           one.playsOne, seat + 1, c.seatPlays, one.playsOther);
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image = image;
    bool hits = false;
    if (platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        // Resolved now, installed by the first BeforeTick (the fallback call crosses a block).
        const auto hit = [&image](platform::CallRedirect& redirect, std::uintptr_t site, std::uintptr_t target,
                                  std::uintptr_t replacement) {
            const auto at = image.Rebase(site, 5);
            const auto to = image.Rebase(target);
            return at && to ? HitSite{&redirect, *at, *to, replacement} : HitSite{};
        };
        g_hitSites    = {hit(g_gateCall, kGateCall, kGate, reinterpret_cast<std::uintptr_t>(&Gate)),
                         hit(g_cameraCall, kCameraCall, kCamera, reinterpret_cast<std::uintptr_t>(&Camera)),
                         hit(g_fallbackCall, kFallbackCall, kGetPrimaryEndPlayerController,
                             reinterpret_cast<std::uintptr_t>(&Fallback))};
        g_hitsPending = std::all_of(g_hitSites.begin(), g_hitSites.end(),
                                    [](const HitSite& s) { return s.redirect != nullptr; });
    }
    else
    {
        const bool gate = g_gateCall.InstallResearch(image, kGateCall, kGate, reinterpret_cast<std::uintptr_t>(&Gate));
        const bool camera   = gate && g_cameraCall.InstallResearch(image, kCameraCall, kCamera,
                                                                   reinterpret_cast<std::uintptr_t>(&Camera));
        const bool fallback = gate && g_fallbackCall.InstallResearch(image, kFallbackCall,
                                                                    kGetPrimaryEndPlayerController,
                                                                     reinterpret_cast<std::uintptr_t>(&Fallback));
        if (gate && (!camera || !fallback))
        {
            // All three or none: a gate passing for a seat must not shake player 1's camera or play on player 1's pad.
            g_cameraCall.Restore();
            g_fallbackCall.Restore();
            g_gateCall.Restore();
        }
        hits = gate && camera && fallback;
    }

    // The notifies: the first-controller call sites are installed from the game thread's first tick (two cross a block).
    const auto first  = platform::FindFunction(image, facts::kGetFirstPlayerController);
    g_firstController = first ? *first : 0;
    if (g_firstController != 0)
    {
        const auto wrappers = Wrappers(std::make_index_sequence<kNotifies.size()>{});
        for (std::size_t i = 0; i < kNotifies.size(); ++i)
        {
            const auto slot     = image.Rebase(kNotifies[i].slot, sizeof(void*));
            const auto function = image.Rebase(kNotifies[i].function);
            const auto site     = image.Rebase(kNotifies[i].firstControllerCall, 5);
            if (slot && function && site && g_notifySlots[i].Replace(*slot, *function, wrappers[i]))
            {
                g_firstSites[i] = *site;
                ++g_notifiesWrapped;
            }
        }
    }

    platform::Log(L"combat rumble: battle hits {}; montage notifies wrapped {} of {}{}; plays {}",
                  hits          ? L"per player (player 2's or 3's member's hits ask the game's gate with that player as the leader)"
                  : g_hitsPending ? L"from the game thread's first tick (a call crosses a block; logged then)"
                                  : L"UNCHANGED (a call site is not what the research read)",
                  g_notifiesWrapped, kNotifies.size(),
                  g_firstController == 0 ? L" (GetFirstPlayerController is not what the research read)" : L"",
                  InstallPlays(image));
}

void BeforeTick() noexcept
{
    if (g_hitsPending && players::generation::OnGameThread())
    {
        g_hitsPending = false;  // here the game thread is outside every hit, as InstallFromOwnThread requires
        bool all      = true;
        g_hitsOwnThread.store(true, std::memory_order_relaxed);
        for (const HitSite& s : g_hitSites)
        {
            all = all && s.redirect->InstallFromOwnThread(s.site, s.target, s.replacement);
        }
        g_hitsOwnThread.store(all, std::memory_order_relaxed);
        if (!all)
        {
            // All three or none (as Install): restored from the same thread, still outside every hit.
            for (auto it = g_hitSites.rbegin(); it != g_hitSites.rend(); ++it)
            {
                if (it->redirect->IsInstalled())
                {
                    it->redirect->Restore();
                }
            }
        }
        platform::Log(L"combat rumble: battle hits {}",
                      all ? L"per player (player 2's or 3's member's hits ask the game's gate with that player as the leader; "
                            L"installed from the game thread)"
                          : L"UNCHANGED (a call site is not what the research read)");
    }
    if (!g_sitesTried && players::generation::OnGameThread() && g_firstController != 0)
    {
        g_sitesTried = true;  // here the game thread is outside every notify, as InstallFromOwnThread requires
        unsigned sites = 0;
        for (std::size_t i = 0; i < kNotifies.size(); ++i)
        {
            sites += g_firstSites[i] != 0 &&
                             g_firstCalls[i].InstallFromOwnThread(g_firstSites[i], g_firstController,
                                                                  reinterpret_cast<std::uintptr_t>(&FirstController))
                         ? 1
                         : 0;
        }
        platform::Log(L"combat rumble: {} of {} notify first-controller calls answer player 2's or 3's controller for that player's "
                      L"mesh",
                      sites, g_notifiesWrapped);
    }
    LogStatus();
}

void Remove() noexcept
{
    g_hitsPending = false;  // a first tick after this installs nothing
    g_sitesTried  = true;
    const bool gameThread = players::generation::OnGameThread();
    bool       restored   = true;
    bool       left       = false;
#if LSS_DEV
    restored = g_playClassic.Restore() && restored;
    restored = g_play.Restore() && restored;
#endif
    for (auto& call : g_firstCalls)
    {
        if (call.IsInstalled() && call.Crosses() && !gameThread)
        {
            left = true;  // a crossing call is restored only from the game thread (call_redirect.h)
            continue;
        }
        restored = call.Restore() && restored;
    }
    for (auto& slot : g_notifySlots)
    {
        restored = slot.Restore() && restored;
    }
    // The hits placed from the game thread (1.005: the fallback crosses a block) go back all three or none, from
    // that thread; the research build's, placed at install, from any.
    if (g_hitsOwnThread.load(std::memory_order_relaxed) && !gameThread)
    {
        left = true;
    }
    else
    {
        for (platform::CallRedirect* call : {&g_fallbackCall, &g_cameraCall, &g_gateCall})
        {
            restored = call->Restore() && restored;
        }
    }
    if (left)
    {
        platform::Log(L"combat rumble: the calls placed from the game thread are left in place (unloading is not that "
                      L"thread; they pass through)");
    }
    if (!restored)
    {
        platform::Log(L"combat rumble: a call, slot or entry was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::feedback::combat_rumble
