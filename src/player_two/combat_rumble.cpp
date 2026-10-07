#include "player_two/combat_rumble.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"
#include "players/unreal_access.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace lss::player_two::combat_rumble {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-rumble.md §10.4, §10.8, §10.12 (this build).
// BattleHit_ApplyReaction_CamFxAndFFB 0x14131CD8C: the leader gate (cl kind, rdx attacker, r8 target), the camera effect
// on the primary camera manager, and the primary-controller fallback when the attacker has no player controller.
constexpr std::uintptr_t kGateCall     = 0x14131FE21;
constexpr std::uintptr_t kGate         = 0x14169AE88;  // BattleHitFx_ShouldPlay_LeaderInvolved
constexpr std::uintptr_t kCameraCall   = 0x14131FE78;
constexpr std::uintptr_t kCamera       = 0x1416FAE10;  // PrimaryPCM_CameraEffect2DF0_Play (§10.12)
constexpr std::uintptr_t kFallbackCall = 0x14131FEA2;
constexpr std::uintptr_t kPrimary      = 0x140A61D1C;  // GetPrimaryEndPlayerController_FromPartyManager
// 1.005 (RE-1005-field-2.md §6): the fallback call is at 0x14139787C, %16 = 12, so it crosses a 16-byte block. There the
// three calls are changed together from the game thread's first tick (InstallFromOwnThread), outside every hit: the
// hit reaction runs on the game thread [ASSUMPTION: battle damage is applied in the world tick; the gate's leader
// scopes already rely on it, and Gate warns if a hit ever arrives off it].

// UWorld::GetFirstPlayerController (stock; entry `mov eax, 28h`, as hud/area_icons_two.cpp checks).
constexpr std::uintptr_t kFirstController        = 0x140EF3078;
constexpr std::uint8_t   kFirstControllerBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00};

// The force-feedback notifies (SE End classes; RUMBLE3-0x1411F7C78-AnimNotify_EndFFB_family.txt): each class's vtable
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

// The plays, for the log: EndFFBComp_PlayChannel (the dispatcher) and its classic path (also called directly).
constexpr std::uintptr_t kPlay               = 0x1411F71E8;
constexpr std::uint8_t   kPlayBytes[]        = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::uintptr_t kPlayClassic        = 0x1411F7244;
constexpr std::uint8_t   kPlayClassicBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20};
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

constexpr int  kMaxLines    = 600;
constexpr auto kStatusEvery = std::chrono::seconds(10);

using GateFn      = std::uint64_t (*)(std::uint64_t kind, void* attacker, void* target, void* functor);
using CameraFn    = void* (*)(void* handle, const void* location, const void* name);
using FallbackFn  = void* (*)();
using FirstFn     = void* (*)(void* world);
using Notify3Fn   = void (*)(void* notify, void* mesh, void* animation);
using Notify4Fn   = void (*)(void* notify, void* mesh, void* animation, float value);
// Six arguments, two on the stack (§10.12 item 7): rcx component, dl channel, r8 FName*, r9b, [rsp+0x28] u8,
// [rsp+0x30] u8*. Returns a byte.
using PlayFn = std::uint64_t (*)(void* component, std::uint64_t channel, const void* name, std::uint64_t r9,
                                 std::uint64_t fifth, void* sixth);

// Set once at install.
platform::CallRedirect                                   g_gateCall, g_cameraCall, g_fallbackCall;
std::array<platform::CallRedirect, kNotifies.size()>     g_firstCalls;
std::array<std::uintptr_t, kNotifies.size()>             g_firstSites{};
std::array<platform::VTableEntry, kNotifies.size()>      g_notifySlots;
platform::EntryRedirect                                  g_play, g_playClassic;
std::uintptr_t                                           g_firstController = 0;
std::optional<platform::GameImage>                       g_image;      // live addresses as research IDA addresses
bool                                                     g_sitesTried = false;
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
// The last gate ran for player 2's member and passed. Read by the same hit's camera and fallback calls, a few
// instructions later; between the gate and them run only the target AI component's effect-name listeners
// (0x14173EB94 before the camera, IsEndPlayerController before the fallback), which replace an effect name and are not
// known to start another hit [ASSUMPTION, §10.12 item 4]. A nested hit there would overwrite the flag.
bool  g_hitTwo           = false;
void* g_notifyController = nullptr;  // player 2's controller while a notify for player 2 runs
int   g_lines            = 0;
bool  g_budgetSaid       = false;
Clock::time_point g_statusAt{};
struct Counts
{
    std::uint32_t hitsOne = 0, hitsTwo = 0, gatesTwo = 0, camerasTwo = 0, fallbacksTwo = 0, hitSkips = 0;
    std::uint32_t notifiesOne = 0, notifiesTwo = 0, notifySkips = 0, answered = 0;
    std::uint32_t playsOne = 0, playsTwo = 0, playsOther = 0;
};
Counts g_counts;

bool Ready() noexcept
{
    return generation::OnGameThread() && generation::Active();
}

/// A Debug line may be written (the level is on and the budget lasts).
bool Line() noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return false;
    }
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        return true;
    }
    if (!g_budgetSaid)
    {
        g_budgetSaid = true;
        platform::LogDebug(L"combat rumble: {} lines written; only the status line from here on", kMaxLines);
    }
    return false;
}

void* PlayerOnePawn() noexcept
{
    return ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
}

const wchar_t* Who(const void* character) noexcept
{
    if (character == nullptr)
    {
        return L"none";
    }
    if (character == generation::Member())
    {
        return L"player 2's member";
    }
    if (character == PlayerOnePawn())
    {
        return L"player 1's character";
    }
    return L"another character";
}

const wchar_t* ControllerText(const void* controller) noexcept
{
    return controller == nullptr                         ? L"no controller"
           : controller == generation::PlayerOneController() ? L"PC1"
           : controller == generation::PlayerTwoController() ? L"PC2"
                                                             : L"another controller";
}

/// Player 2's leader view (leader, leader type, primary controller) for one call; nothing held if any part refuses.
class PlayerTwoView
{
public:
    explicit PlayerTwoView(bool primary) noexcept
    {
        void*      member = generation::Member();
        const auto type   = party_member::PlayerTypeOf(member);
        const auto weak   = leader_scope::WeakOf(member);
        if (!type || weak == 0)
        {
            return;
        }
        if (primary)
        {
            primary_.emplace();
            if (!primary_->Held())
            {
                primary_.reset();
                return;
            }
        }
        leader_.emplace(weak);
        type_.emplace(*type);
        if (!leader_->Held() || !type_->Held())
        {
            type_.reset();
            leader_.reset();
            primary_.reset();
            return;
        }
        held_ = true;
    }

    bool Held() const noexcept { return held_; }

private:
    std::optional<primary_controller::Scope>       primary_;  // destroyed last (opened first)
    std::optional<leader_scope::LeaderScope>       leader_;
    std::optional<leader_scope::LeaderTypeScope>   type_;
    bool                                           held_ = false;
};

// ---------------------------------------------------------------------------------------------
// Battle hits
// ---------------------------------------------------------------------------------------------

std::uint64_t Gate(std::uint64_t kind, void* attacker, void* target, void* functor) noexcept
{
    const auto original = reinterpret_cast<GateFn>(g_gateCall.Original());
    g_hitTwo            = false;
    // On 1.005 the fallback call was changed on the understanding that only the game thread runs a hit (call_redirect.h).
    static std::atomic<unsigned> offThread{0};
    if (g_hitsOwnThread.load(std::memory_order_relaxed) && !generation::OnGameThread() &&
        offThread.fetch_add(1, std::memory_order_relaxed) < 5)
    {
        platform::Log(L"WARNING: combat rumble: a battle hit ran off the game thread; its fallback call was changed on the "
                      L"understanding that only the game thread runs it");
    }
    if (!Ready())
    {
        return original(kind, attacker, target, functor);
    }
    void*      member = generation::Member();
    void*      one    = PlayerOnePawn();
    const bool isOne  = one != nullptr && (attacker == one || target == one);
    // Player 1's character on either side keeps the game's own answer (party hits are enemy against party).
    const bool isTwo  = !isOne && member != nullptr && (attacker == member || target == member);
    if (!isTwo)
    {
        const auto result = original(kind, attacker, target, functor);
        if (isOne)
        {
            ++g_counts.hitsOne;
            if (Line())
            {
                platform::LogDebug(L"combat rumble: frame {}: battle hit (gate kind {}), attacker {}, target {}: player 1's, gate {}",
                              platform::frame_watch::FrameIndex(), kind & 0xFF, Who(attacker), Who(target),
                              (result & 0xFF) != 0 ? L"passed" : L"closed");
            }
        }
        return result;
    }
    ++g_counts.hitsTwo;
    std::uint64_t result = 0;
    bool          held   = false;
    {
        const PlayerTwoView view(false);  // the gate reads the leader and its type only
        held   = view.Held();
        result = original(kind, attacker, target, functor);
    }
    g_hitTwo = held && (result & 0xFF) != 0;
    g_counts.gatesTwo += g_hitTwo ? 1 : 0;
    g_counts.hitSkips += held ? 0 : 1;
    if (Line())
    {
        platform::LogDebug(L"combat rumble: frame {}: battle hit (gate kind {}), attacker {}, target {}: player 2's{}, gate {}",
                      platform::frame_watch::FrameIndex(), kind & 0xFF, Who(attacker), Who(target),
                      held ? L" (the gate asked with player 2's member as the leader)"
                           : L" (player 2's leader view REFUSED: the game's own answer)",
                      (result & 0xFF) != 0 ? L"passed" : L"closed");
    }
    return result;
}

/// The hit's camera effect (PrimaryPCM_CameraEffect2DF0_Play: rcx the out handle, returned; rdx the location; r8 the
/// effect's FName*), which finds "the" camera manager through the primary controller: player 2's for player 2's hit.
void* Camera(void* handle, const void* location, const void* name) noexcept
{
    const auto original = reinterpret_cast<CameraFn>(g_cameraCall.Original());
    if (!g_hitTwo || !Ready())
    {
        return original(handle, location, name);
    }
    const primary_controller::Scope primary;
    if (!primary.Held())
    {
        ++g_counts.hitSkips;
        return original(handle, location, name);
    }
    void* result = original(handle, location, name);
    ++g_counts.camerasTwo;
    if (Line())
    {
        const bool played = handle != nullptr && ReadField<std::int32_t>(handle, 0) != -1;  // {-1, 0} when nothing plays
        platform::LogDebug(L"combat rumble: frame {}: the hit's camera effect {} on player 2's camera manager{}",
                      platform::frame_watch::FrameIndex(),
                      players::unreal::NameText(name != nullptr ? ReadField<std::uint64_t>(name, 0) : 0),
                      played ? L"" : L": NOTHING PLAYED (player 2's camera manager has no effect component?)");
    }
    return result;
}

/// The hit's controller when the attacker has none (GetPrimaryEndPlayerController_FromPartyManager, no arguments):
/// player 2's for player 2's hit.
void* Fallback() noexcept
{
    const auto original = reinterpret_cast<FallbackFn>(g_fallbackCall.Original());
    if (!g_hitTwo || !Ready())
    {
        return original();
    }
    const primary_controller::Scope primary;
    if (!primary.Held())
    {
        ++g_counts.hitSkips;
        return original();
    }
    ++g_counts.fallbacksTwo;
    return original();
}

// ---------------------------------------------------------------------------------------------
// Notifies
// ---------------------------------------------------------------------------------------------

/// Opens player 2's view around a notify on player 2's mesh, and makes its first-controller calls answer player 2's.
class NotifyScope
{
public:
    NotifyScope(std::size_t index, void* mesh) noexcept : index_(index)
    {
        // Without its first-controller call answering PC2, an open gate would play player 2's notify on player 1's pad.
        if (!Ready() || !g_firstCalls[index].IsInstalled())
        {
            return;
        }
        void*       owner   = ReadPointer(mesh, kComponentOwner);
        void*       two     = generation::PlayerTwoController();
        const void* twoPawn = ReadPointer(two, facts::kControllerPawn);
        const bool  isTwo   = owner != nullptr && (owner == generation::Member() || owner == twoPawn);
        const Phase phase   = kNotifies[index].phase;
        if (!isTwo)
        {
            if (owner != nullptr && owner == PlayerOnePawn())
            {
                ++g_counts.notifiesOne;
                if (phase != Phase::Tick && Line())
                {
                    platform::LogDebug(L"combat rumble: frame {}: {} {} on player 1's character's mesh: the game's own (PC1)",
                                  platform::frame_watch::FrameIndex(), kNotifies[index].what,
                                  kPhaseText[static_cast<int>(phase)]);
                }
            }
            return;
        }
        ++g_counts.notifiesTwo;
        view_.emplace(true);
        if (!view_->Held())
        {
            view_.reset();
            ++g_counts.notifySkips;
            if (phase != Phase::Tick && Line())
            {
                platform::LogDebug(L"combat rumble: frame {}: {} {} on player 2's mesh: player 2's leader view REFUSED (the "
                              L"game's own: player 1's controller or none)",
                              platform::frame_watch::FrameIndex(), kNotifies[index].what, kPhaseText[static_cast<int>(phase)]);
            }
            return;
        }
        saved_             = g_notifyController;
        g_notifyController = two;
        answeredAt_        = g_counts.answered;
        owner_             = owner;
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
        const auto  calls  = g_counts.answered - answeredAt_;
        if (phase != Phase::Tick && Line())
        {
            platform::LogDebug(L"combat rumble: frame {}: {} {} on {}'s mesh: {}", platform::frame_watch::FrameIndex(),
                          kNotifies[index_].what, kPhaseText[static_cast<int>(phase)],
                          owner_ == generation::Member() ? L"player 2's member" : L"player 2's ridden pawn",
                          calls != 0 ? L"on player 2's controller (its first-controller call answered PC2)"
                                     : L"nothing played (the notify's own gate closed with player 2 as the leader)");
        }
    }

    NotifyScope(const NotifyScope&)            = delete;
    NotifyScope& operator=(const NotifyScope&) = delete;

private:
    std::size_t                  index_      = 0;
    std::optional<PlayerTwoView> view_;
    void*                        saved_      = nullptr;
    const void*                  owner_      = nullptr;
    std::uint32_t                answeredAt_ = 0;
    bool                         active_     = false;
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

/// A notify's GetFirstPlayerController call: player 2's controller while a notify for player 2 runs.
void* FirstController(void* world) noexcept
{
    if (g_notifyController != nullptr && Ready())
    {
        ++g_counts.answered;
        return g_notifyController;
    }
    return reinterpret_cast<FirstFn>(g_firstController)(world);
}

// ---------------------------------------------------------------------------------------------
// Plays (log only)
// ---------------------------------------------------------------------------------------------

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
    (controller == generation::PlayerOneController()   ? g_counts.playsOne
     : controller == generation::PlayerTwoController() ? g_counts.playsTwo
                                                       : g_counts.playsOther) += 1;
    if (!Line())
    {
        return;
    }
    try
    {
        const auto*         source = SourceOf(returnAddress);
        const std::uint64_t key    = name != nullptr ? ReadField<std::uint64_t>(name, 0) : 0;
        platform::LogDebug(L"combat rumble: frame {}: play {} channel {} on {} from {}", platform::frame_watch::FrameIndex(),
                      players::unreal::NameText(key), channel & 0xFF, ControllerText(controller),
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

void LogStatus() noexcept
{
    const auto now = Clock::now();
    if (now - g_statusAt < kStatusEvery || !generation::Active())
    {
        return;
    }
    g_statusAt = now;
    const Counts c = g_counts;
    g_counts       = {};
    if (c.hitsOne + c.hitsTwo + c.notifiesOne + c.notifiesTwo + c.playsOne + c.playsTwo + c.playsOther == 0)
    {
        return;
    }
    platform::LogDebug(L"combat rumble (10 s): battle hits player 1's {}, player 2's {} (gate passed {}, camera on player 2's "
                  L"{}, fallback to player 2's controller {}, refused {}); notifies player 1's {}, player 2's {} (first-"
                  L"controller calls answered PC2 {}, refused {}); plays on PC1 {}, PC2 {}, other {}",
                  c.hitsOne, c.hitsTwo, c.gatesTwo, c.camerasTwo, c.fallbacksTwo, c.hitSkips, c.notifiesOne, c.notifiesTwo,
                  c.answered, c.notifySkips, c.playsOne, c.playsTwo, c.playsOther);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image = image;
    const auto call = [&image](platform::CallRedirect& redirect, std::uintptr_t site, std::uintptr_t target,
                               std::uintptr_t replacement) {
        const auto at = image.Rebase(site, 5);
        const auto to = image.Rebase(target);
        return at && to && redirect.Install(*at, *to, replacement);
    };
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
                         hit(g_fallbackCall, kFallbackCall, kPrimary, reinterpret_cast<std::uintptr_t>(&Fallback))};
        g_hitsPending = std::all_of(g_hitSites.begin(), g_hitSites.end(),
                                    [](const HitSite& s) { return s.redirect != nullptr; });
    }
    else
    {
        const bool gate = call(g_gateCall, kGateCall, kGate, reinterpret_cast<std::uintptr_t>(&Gate));
        const bool camera   = gate && call(g_cameraCall, kCameraCall, kCamera, reinterpret_cast<std::uintptr_t>(&Camera));
        const bool fallback = gate && call(g_fallbackCall, kFallbackCall, kPrimary, reinterpret_cast<std::uintptr_t>(&Fallback));
        if (gate && (!camera || !fallback))
        {
            // All three or none: a gate passing for player 2 must not shake player 1's camera or play on player 1's pad.
            g_cameraCall.Restore();
            g_fallbackCall.Restore();
            g_gateCall.Restore();
        }
        hits = gate && camera && fallback;
    }

    // The notifies: the first-controller call sites are installed from the game thread's first tick (two cross a block).
    const auto first = image.Rebase(kFirstController);
    g_firstController = first && image.BytesMatch(kFirstController, kFirstControllerBytes) ? *first : 0;
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

    const auto play    = image.Rebase(kPlay);
    const auto classic = image.Rebase(kPlayClassic);
    const bool plays = play && classic && image.BytesMatch(kPlay, kPlayBytes) &&
                       image.BytesMatch(kPlayClassic, kPlayClassicBytes) &&
                       g_play.Install(*play, image.EntryInstruction(kPlay, kPlayBytes), reinterpret_cast<std::uintptr_t>(&Play)) &&
                       g_playClassic.Install(*classic, image.EntryInstruction(kPlayClassic, kPlayClassicBytes),
                                             reinterpret_cast<std::uintptr_t>(&PlayClassic));
    platform::Log(L"combat rumble: battle hits {}; montage notifies wrapped {} of {}{}; plays {}",
                  hits          ? L"per player (player 2's member's hits ask the game's gate with player 2 as the leader)"
                  : g_hitsPending ? L"from the game thread's first tick (a call crosses a block; logged then)"
                                  : L"UNCHANGED (a call site is not what the research read)",
                  g_notifiesWrapped, kNotifies.size(),
                  g_firstController == 0 ? L" (GetFirstPlayerController is not what the research read)" : L"",
                  plays ? L"logged with their source" : L"NOT logged (an entry is not what the research read)");
}

void BeforeTick() noexcept
{
    if (g_hitsPending && generation::OnGameThread())
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
                      all ? L"per player (player 2's member's hits ask the game's gate with player 2 as the leader; "
                            L"installed from the game thread)"
                          : L"UNCHANGED (a call site is not what the research read)");
    }
    if (!g_sitesTried && generation::OnGameThread() && g_firstController != 0)
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
        platform::Log(L"combat rumble: {} of {} notify first-controller calls answer player 2's controller for player 2's "
                      L"mesh",
                      sites, g_notifiesWrapped);
    }
    LogStatus();
}

}  // namespace lss::player_two::combat_rumble
