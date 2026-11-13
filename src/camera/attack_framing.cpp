#include "camera/attack_framing.h"

#include "battle/target_coin.h"
#include "camera/attack_cameras.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/every.h"
#include "platform/game_thread_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/leader_scope.h"
#include "session/party_member.h"
#include "world/world_rules.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace lss::camera::attack_framing {

namespace {

using Clock = std::chrono::steady_clock;

// research/RE-p2-attack-cameras.md (both builds; every site re-read from both exes).
// D1: the slot-20 compute's call of CamEntry_IsActive_LeaderKeyed(rcx unused, rdx entry) -> bool (1.005: crosses a block).
constexpr std::uintptr_t kActiveCall     = 0x140E58FD0;
constexpr std::uint8_t   kUsesTargetBytes[] = {0x8A, 0x91, 0x80, 0x08, 0x00, 0x00, 0x32, 0xC0};
// EndBattleCameraActor_UsesTargetMeshOrActor(cam) -> bool
constexpr platform::FunctionFact kUsesTarget{L"EndBattleCameraActor_UsesTargetMeshOrActor", 0x140AEED2C,
                                             kUsesTargetBytes};
constexpr std::uint8_t   kFrameBytes[]      = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
// EndBattleCameraActor_ShotInvolvesLeader(cam) -> bool
constexpr platform::FunctionFact kInvolvesLeader{L"EndBattleCameraActor_ShotInvolvesLeader", 0x140AEED90, kFrameBytes};
// EndBattleCameraActor_GetTarget(cam) -> UObject*
constexpr platform::FunctionFact kGetTarget{L"EndBattleCameraActor_GetTarget", 0x140AEEEEC, kFrameBytes};
constexpr platform::FunctionFact kIsActive{L"CamEntry_IsActive_LeaderKeyed", 0x140AEEB7C, kFrameBytes};
// D2: the calls of EndBattleCameraActor_UpdateFromOperatorPose(cam rcx, float dt xmm1, pose r8) -> byte: per camera in
// BattleCamSeqEntry_EvaluatePose (1.004: crosses a block) and the seed at play; and the three calls of
// PartyManager_GetCharacterByPlayerType(u8 lane, u8 type) -> UObject* that read the leader inside it (R1 selector 3,
// R2 selector 6, R3 the eye's relative rotation).
constexpr std::uintptr_t                kUpdateCalls[]  = {0x140C427CE, 0x140B6A798};
constexpr std::uint8_t                  kUpdatePoseBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57};
constexpr platform::FunctionFact kUpdatePose{L"EndBattleCameraActor_UpdateFromOperatorPose", 0x140AEF118,
                                             kUpdatePoseBytes};
constexpr std::uintptr_t                kLeaderCalls[]  = {0x140AF0EC5, 0x14292F6D9, 0x142AA2995};
using facts::kCharacterByPlayerType;

// The sequence list's entry, the sequence actor's camera and the camera's owner (facts/cameras.h).
using facts::kSequenceEntryPriority;
using facts::kSequenceEntryActor;
using facts::kSequenceActorCamera;
using facts::kBattleCameraOwner;
constexpr std::uint8_t                      kSyncPriority   = 10;
constexpr std::uint8_t                      kPlayerPriority = 20;
constexpr std::uint8_t                      kMenuPriority   = 70;  // and 71 Debug

constexpr std::size_t kLatches     = 32;
constexpr auto        kStatusEvery = std::chrono::seconds(10);

using IsActiveFn  = bool (*)(void* unused, void* entry);
using CameraFn    = bool (*)(const void* camera);
using TargetFn    = void* (*)(const void* camera);
using UpdateFn    = std::uint8_t (*)(void* camera, float deltaTime, const void* pose);
using CharacterFn = void* (*)(std::uint64_t lane, std::uint64_t type);

// Set once at install / placement.
platform::CallRedirect                                         g_activeCall;
std::array<platform::CallRedirect, std::size(kUpdateCalls)>    g_updateCalls;
std::array<platform::CallRedirect, std::size(kLeaderCalls)>    g_leaderCalls;
CameraFn  g_usesTarget     = nullptr;
TargetFn  g_getTarget      = nullptr;
CameraFn  g_involvesLeader = nullptr;
struct Sites
{
    std::uintptr_t active = 0, isActive = 0;
    std::array<std::uintptr_t, std::size(kUpdateCalls)> update{};
    std::uintptr_t updatePose = 0;
    std::array<std::uintptr_t, std::size(kLeaderCalls)> leader{};
    std::uintptr_t characterByType = 0;
    bool           d1 = false, d2 = false;
};
Sites             g_sites;
std::atomic<bool> g_pending{false};  // a placement queued and not yet run (Remove clears it)

// Game thread.
// D1: per sequence (its actor's weak) and camera (LatchKey), the decision: show (native) or keep off this camera.
// D2: per shot (the camera actor's weak), the enemy's coin: a player number (CoinPlayer), 0 the game's own answer.
using world::world_rules::Latch;
std::array<Latch, kLatches> g_d1{};
std::array<Latch, kLatches> g_d2{};
std::uint64_t               g_tick = 0;
const void*                 g_shotCamera = nullptr;  // the camera actor being evaluated (game thread)

// The coin's values are target_coin's (the battle group's): the player number, 1 player 1, 2 player 2, 3 player 3. A
// coin c names the player in seat c - 1 here, for any built seat (a coin past the built seats is the game's answer).

struct Counts
{
    std::uint32_t kept = 0, offOther = 0, native = 0, coinNone = 0, answers = 0;
    std::array<std::uint32_t, players::kMaxPlayers> coin{};  // leader-framing shots on each player, by seat
};
Counts            g_counts;
platform::Every g_status{kStatusEvery};

/// The seat a coin names (coin c: seat c - 1), or -1 for none (0, or a player not built).
int CoinPlayer(int coin) noexcept
{
    return coin >= 1 && coin <= players::kSeatsBuilt + 1 ? coin - 1 : -1;
}

bool Line() noexcept
{
    return platform::Enabled(platform::Level::Debug);
}

/// The latch for `key`, made (value `fresh`) if absent; the least recently used goes when full.
Latch& Find(std::array<Latch, kLatches>& latches, std::uint64_t key, bool& made) noexcept
{
    return world::world_rules::LatchFor(latches, key, g_tick, made);
}

void* PlayerOneCharacter() noexcept
{
    return players::actor_fields::Pawn(players::generation::PlayerOneController());
}

/// The seat's member while its player is in, or null.
void* SeatMember(int seat) noexcept
{
    return players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
}

/// On the game thread with a built seat's player in with a member.
bool Bound() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return false;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (SeatMember(seat) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/// Each player's character by seat: player 1's pawn, each seat's member while its player is in.
void Humans(const void* (&humans)[players::kMaxPlayers]) noexcept
{
    humans[0] = PlayerOneCharacter();
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        humans[seat] = players::SeatBuilt(seat) ? SeatMember(seat) : nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// D1
// ---------------------------------------------------------------------------------------------

constexpr std::uint8_t kUndecided = 0, kShow = 1, kOff = 2;

/// Whether the shot of `camera` (an entry the game's test found active) stays on this camera.
std::uint8_t Decide(std::uint8_t priority, const void* camera) noexcept
{
    bool frames = false;
    if (!platform::CallGuarded(g_usesTarget, frames, camera))
    {
        return kShow;
    }
    if (priority == kSyncPriority)
    {
        // A grab: the game's own test already keeps it to the caught player's camera when it involves the leader. Only
        // the ones that passed through their ActorName / Summon selectors are decided here.
        bool involves = true;
        if (!platform::CallGuarded(g_involvesLeader, involves, camera) || involves)
        {
            return kShow;
        }
        frames = true;
    }
    if (!frames)
    {
        return kShow;
    }
    void* target = nullptr;
    if (!platform::CallGuarded(g_getTarget, target, camera) || target == nullptr)
    {
        return kUndecided;  // decided on a later frame, once there is a target
    }
    // The AI ally or an enemy: every camera, as solo shows it; another player's target is off unless this camera's
    // player owns the shot (the camera: the seat whose update is open, 0 player 1's).
    const void* humans[players::kMaxPlayers] = {};
    Humans(humans);
    return ShotShowsOn(target, humans, attack_cameras::InsideSeat(), ReadPointer(camera, kBattleCameraOwner)) ? kShow
                                                                                                               : kOff;
}

bool IsActive(void* unused, void* entry) noexcept
{
    const bool active = reinterpret_cast<IsActiveFn>(g_activeCall.Original())(unused, entry);
    if (!active || !Bound())
    {
        return active;
    }
    const auto priority = ReadField<std::uint8_t>(entry, kSequenceEntryPriority);
    if (priority == kPlayerPriority || priority >= kMenuPriority)
    {
        return active;
    }
    const auto key = ReadField<std::uint64_t>(entry, kSequenceEntryActor);
    if (key == 0)
    {
        return active;
    }
    const int           seat      = attack_cameras::InsideSeat();  // this camera's (0 player 1's)
    const std::uint64_t perCamera = LatchKey(key, seat);
    bool                made      = false;
    Latch&              latch     = Find(g_d1, perCamera, made);
    const void*         camera    = nullptr;
    if (latch.value == kUndecided)
    {
        const void* actor = scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(entry) + kSequenceEntryActor);
        camera            = ReadPointer(actor, kSequenceActorCamera);
        if (camera == nullptr)
        {
            return active;
        }
        latch.value = Decide(priority, camera);
        if (latch.value != kUndecided)
        {
            (latch.value == kOff ? g_counts.offOther : g_counts.native) += 1;
            if (latch.value == kOff && Line())
            {
                platform::LogDebug(L"attack framing: {}'s shot (priority {}) targets the other player: not on {}'s camera",
                                   players::NameOf(ReadPointer(camera, kBattleCameraOwner)), priority, players::SeatName(seat));
            }
        }
    }
    if (latch.value == kOff)
    {
        return false;
    }
    ++g_counts.kept;
    return active;
}

// ---------------------------------------------------------------------------------------------
// D2
// ---------------------------------------------------------------------------------------------

/// The shot's latched coin: tossed once, from its owner's coin (the enemy's controller), on its first evaluation.
std::uint8_t CoinOf(const void* camera) noexcept
{
    const std::uint64_t key = scopes::leader_scope::WeakOf(const_cast<void*>(camera));
    if (key == 0)
    {
        return 0;
    }
    bool   made  = false;
    Latch& latch = Find(g_d2, key, made);
    if (made)
    {
        const void* owner      = ReadPointer(camera, kBattleCameraOwner);
        const void* controller = owner != nullptr ? battle::target_coin::ControllerOfEnemy(owner) : nullptr;
        const int   coin       = controller != nullptr ? battle::target_coin::CoinFor(controller) : 0;
        const int   player     = CoinPlayer(coin);
        latch.value            = static_cast<std::uint8_t>(player >= 0 ? coin : 0);
        (player >= 0 ? g_counts.coin[static_cast<std::size_t>(player)] : g_counts.coinNone) += 1;
    }
    return latch.value;
}

/// Around UpdateFromOperatorPose: the shot being evaluated, for the leader reads inside it.
std::uint8_t UpdatePose(std::size_t site, void* camera, float deltaTime, const void* pose) noexcept
{
    const auto original = reinterpret_cast<UpdateFn>(g_updateCalls[site].Original());
    if (!players::generation::OnGameThread())
    {
        return original(camera, deltaTime, pose);  // the shot camera belongs to the game thread
    }
    const void* saved = g_shotCamera;
    g_shotCamera      = camera;
    const auto result = original(camera, deltaTime, pose);
    g_shotCamera         = saved;
    return result;
}

std::uint8_t UpdatePoseEvaluate(void* camera, float deltaTime, const void* pose) noexcept
{
    return UpdatePose(0, camera, deltaTime, pose);
}

std::uint8_t UpdatePoseSeed(void* camera, float deltaTime, const void* pose) noexcept
{
    return UpdatePose(1, camera, deltaTime, pose);
}

/// A leader read inside a shot: the player the owner's coin picked, by the game's own lookup of that player's type.
void* Character(std::size_t site, std::uint64_t lane, std::uint64_t type) noexcept
{
    const auto original = reinterpret_cast<CharacterFn>(g_leaderCalls[site].Original());
    if (g_shotCamera == nullptr || !Bound())
    {
        return original(lane, type);
    }
    const int player = CoinPlayer(CoinOf(g_shotCamera));
    if (player < 0)
    {
        return original(lane, type);
    }
    const auto playerType = session::party_member::PlayerTypeOf(player != 0 ? players::generation::MemberOf(player) : PlayerOneCharacter());
    if (!playerType)
    {
        return original(lane, type);
    }
    ++g_counts.answers;
    return original(lane, *playerType);
}

void* CharacterR1(std::uint64_t lane, std::uint64_t type) noexcept
{
    return Character(0, lane, type);
}

void* CharacterR2(std::uint64_t lane, std::uint64_t type) noexcept
{
    return Character(1, lane, type);
}

void* CharacterR3(std::uint64_t lane, std::uint64_t type) noexcept
{
    return Character(2, lane, type);
}

// ---------------------------------------------------------------------------------------------
// Placement
// ---------------------------------------------------------------------------------------------

/// Game thread, from the session's tick (outside every camera update): every site, each item whole or not at all.
void Place() noexcept
{
    if (!g_pending.exchange(false))
    {
        return;  // removed before the session's first tick
    }
    bool d1 = g_sites.d1 && g_activeCall.InstallFromOwnThread(g_sites.active, g_sites.isActive,
                                                               reinterpret_cast<std::uintptr_t>(&IsActive));
    bool d2 = g_sites.d2;
    const std::array<std::uintptr_t, std::size(kUpdateCalls)> updates{
        reinterpret_cast<std::uintptr_t>(&UpdatePoseEvaluate), reinterpret_cast<std::uintptr_t>(&UpdatePoseSeed)};
    const std::array<std::uintptr_t, std::size(kLeaderCalls)> leaders{reinterpret_cast<std::uintptr_t>(&CharacterR1),
                                                                      reinterpret_cast<std::uintptr_t>(&CharacterR2),
                                                                      reinterpret_cast<std::uintptr_t>(&CharacterR3)};
    // The answers first, then the context: until the context is in, the answers see no shot and pass through.
    for (std::size_t i = 0; d2 && i < leaders.size(); ++i)
    {
        d2 = g_leaderCalls[i].InstallFromOwnThread(g_sites.leader[i], g_sites.characterByType, leaders[i]);
    }
    for (std::size_t i = 0; d2 && i < updates.size(); ++i)
    {
        d2 = g_updateCalls[i].InstallFromOwnThread(g_sites.update[i], g_sites.updatePose, updates[i]);
    }
    if (!d2)
    {
        for (auto& call : g_updateCalls)
        {
            if (call.IsInstalled())
            {
                call.Restore();
            }
        }
        for (auto& call : g_leaderCalls)
        {
            if (call.IsInstalled())
            {
                call.Restore();
            }
        }
    }
    platform::Log(L"attack framing: enemy specials that frame their target {}; enemy specials that frame the leader {}",
                  d1 ? L"play only on the targeted player's camera" : L"UNCHANGED (the activity call did not install)",
                  d2 ? L"frame the enemy's coin player" : L"UNCHANGED (a call did not install)");
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
    g_sites.active   = at(kActiveCall, 5);
    g_sites.isActive = found(kIsActive);
    const auto uses     = found(kUsesTarget);
    const auto target   = found(kGetTarget);
    const auto involves = found(kInvolvesLeader);
    g_sites.d1 = g_sites.active != 0 && g_sites.isActive != 0 && uses != 0 && target != 0 && involves != 0;
    if (g_sites.d1)
    {
        g_usesTarget     = reinterpret_cast<CameraFn>(uses);
        g_getTarget      = reinterpret_cast<TargetFn>(target);
        g_involvesLeader = reinterpret_cast<CameraFn>(involves);
    }
    g_sites.updatePose      = found(kUpdatePose);
    g_sites.characterByType = at(kCharacterByPlayerType.address);
    g_sites.d2              = g_sites.updatePose != 0 && g_sites.characterByType != 0;
    for (std::size_t i = 0; i < std::size(kUpdateCalls); ++i)
    {
        g_sites.update[i] = at(kUpdateCalls[i], 5);
        g_sites.d2        = g_sites.d2 && g_sites.update[i] != 0;
    }
    for (std::size_t i = 0; i < std::size(kLeaderCalls); ++i)
    {
        g_sites.leader[i] = at(kLeaderCalls[i], 5);
        g_sites.d2        = g_sites.d2 && g_sites.leader[i] != 0;
    }
    if (!(g_sites.d1 || g_sites.d2))
    {
        platform::Log(L"attack framing: the sites are not what the research read; enemy specials keep the game's own "
                      L"framing");
    }
    else
    {
        g_pending.store(true);
        if (!platform::game_thread_sites::Queue(&Place))
        {
            g_pending.store(false);
            platform::Log(L"attack framing: the game-thread placement queue is full; enemy specials keep the game's "
                          L"own framing");
        }
    }
}

void Tick() noexcept
{
    ++g_tick;
    const auto now = Clock::now();
    if (!g_status.Due(now))
    {
        return;
    }
    const Counts  c     = std::exchange(g_counts, {});
    std::uint32_t coins = 0;
    for (const std::uint32_t n : c.coin)
    {
        coins += n;
    }
    if (c.kept + c.offOther + c.native + coins + c.coinNone + c.answers == 0)
    {
        return;
    }
    try
    {
        std::wstring on;  // "player 1 N, player 2 N": player 1 and each built seat
        for (int seat = 0; seat <= players::kSeatsBuilt; ++seat)
        {
            on += std::format(L"{}{} {}", seat > 0 ? L", " : L"", players::SeatName(seat),
                              c.coin[static_cast<std::size_t>(seat)]);
        }
        platform::LogDebug(L"attack framing (10 s): shots decided native {}, kept off the other player's camera {}; "
                           L"evaluations kept {}; leader-framing shots on {}, the game's own {}; "
                           L"leader reads answered {}",
                           c.native, c.offOther, c.kept, on, c.coinNone, c.answers);
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    g_pending.store(false);  // a placement still queued installs nothing then
    const bool gameThread = players::generation::OnGameThread();
    bool       restored   = true;
    bool       left       = false;
    const auto restore    = [&](platform::CallRedirect& call) {
        if (!call.IsInstalled())
        {
            return;
        }
        if (call.Crosses() && !gameThread)
        {
            left = true;  // a crossing call is restored only from the game thread (call_redirect.h)
            return;
        }
        restored = call.Restore() && restored;
    };
    for (auto& call : g_updateCalls)
    {
        restore(call);
    }
    for (auto& call : g_leaderCalls)
    {
        restore(call);
    }
    restore(g_activeCall);
    if (left)
    {
        platform::Log(L"attack framing: a call crossing two blocks is left in place (unloading is not the game thread; "
                      L"it passes through)");
    }
    if (!restored)
    {
        platform::Log(L"attack framing: a call was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::camera::attack_framing
