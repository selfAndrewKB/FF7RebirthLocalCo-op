#include "camera/near_fade.h"

#include "facts/engine.h"
#include "facts/party.h"
#include "platform/call_redirect.h"
#include "platform/dev_switch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/object_fields.h"

#include <array>
#include <cstdint>

namespace lss::camera::near_fade {

namespace {

// 1.004 addresses; 1.005 through replaced-near-fade.json (both call sites and callees read from both exes).
constexpr std::uintptr_t kGatherCall = 0x140933A52;  // the near-fade pass: call EndCharaRegistry_GatherCharsWithin500OfPoint
constexpr std::uintptr_t kGather     = 0x141970B28;
constexpr std::uintptr_t kKickCall   = 0x143A9DE05;  // NearFade_KickActorNow_IfGameThread: call ..._FindUnit
constexpr std::uintptr_t kKick       = 0x143A950D8;
// Phase A: player 1's camera's occluder traces toward its pivot admit every party member's mesh (its
// bUpdateJointsFromAnimation bit), so a seat's character between player 1's camera and player 1 was faded. The hit
// actor's weak pointer is resolved here; a null answer makes the game skip the hit.
constexpr std::uintptr_t kHitResolveCall = 0x140933898;  // NearFade_BuildCandidateSet: call FWeakObjectPtr::Get(hit actor)
constexpr std::uint8_t   kAttachParentBytes[] = {0x48, 0x8B, 0x81, 0x38, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0C};
const platform::FunctionFact kAttachParent{L"Pawn_RootAttachParent_FieldA0", 0x1407E29B0, kAttachParentBytes};
// The overlap fade: climbing walls, spider walls and one-shot moves turn on a per-character tick that fades the
// character to 0.3 (hidden reason bit 9) while another party member is inside its capsule, unless the game's player test
// says it is the player's character. A seat's character is one too.
constexpr std::uintptr_t kOverlapPlayerCall = 0x1412FA1E6;  // CharActionCtrl_TickOverlapFade_HiddenReason9: call IsUCPC_...
constexpr std::uintptr_t kIsPlayerChar      = 0x140763270;  // IsUCPC_WithVehicleCheck(actor)

// The gather's output, a TArray<AEndCharacterBase*>.
constexpr std::uint32_t kArrayNum = 8;

// A seat's character's SE fade object (research/RE-p2-camera-near-fade.md, same in both builds): the EndActorInterface
// subobject's fade getter at vtable +0x1D0 (may return null).
constexpr std::uint32_t kFadeGetter    = 0x1D0;
constexpr std::uint32_t kFadeTarget    = 0x90;  // float: the opacity it fades toward
constexpr std::uint32_t kFadeReasons   = 0xA4;  // u32 hidden reasons: bit 0x10 the near fade
constexpr std::uint32_t kFadeDisable   = 0xAC;  // u32 near-fade disable reasons
constexpr std::uint32_t kFadeForce     = 0xB0;  // u32 near-fade force-enable reasons
using FadeOfFn = void* (*)(void* iface);

#if LSS_DEV
// The fade-state line (dev tools): each seat's character's fade object, logged on change (Tick).
struct FadeSeen
{
    std::uint32_t reasons = 0, disable = 0, force = 0;
    float         target  = 1.0f;
    bool          read    = false;
};
std::array<FadeSeen, players::kMaxPlayers> g_fadeSeen{};  // game thread, by seat; index 0 unused
#endif

using GatherFn = void (*)(void* registry, const void* cameraLocation, void* unused, void* out);
using KickFn   = void (*)(void* fadeManager, void* actor, void* unused, const void* cameraLocation);
using WeakGetFn      = void* (*)(const void* weak);
using AttachParentFn = void* (*)(const void* actor);
using IsPlayerFn     = bool (*)(const void* actor);

platform::CallRedirect g_gatherCall;
platform::CallRedirect g_kickCall;
platform::CallRedirect g_hitCall;
platform::CallRedirect g_overlapCall;
AttachParentFn         g_attachParent = nullptr;

// Game thread, by seat (index 0 unused): whether the last pass had the seat's member in its list (logged on change),
// the kicks skipped, and the logged occluder and overlap states.
std::array<bool, players::kMaxPlayers>     g_dropping{};
std::array<unsigned, players::kMaxPlayers> g_kicks{};
std::array<bool, players::kMaxPlayers>     g_hitDropped{};   // the last occluder hit dropped was the seat's
std::array<bool, players::kMaxPlayers>     g_overlapKept{};  // the overlap fade's last test of the seat's character
                                                             // answered "a player's"

std::size_t Index(int seat) noexcept
{
    return static_cast<std::size_t>(seat);
}

/// The built seat whose member `actor` is while its player is in, or 0. Game thread.
int MemberSeat(const void* actor) noexcept
{
    const int seat = players::generation::SeatOfMember(actor);
    return players::SeatBuilt(seat) && players::generation::Active(seat) ? seat : 0;
}

/// A built seat's player is in with a member: only then do the fades below look at a character.
bool AnyMember() noexcept
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat) && players::generation::MemberOf(seat) != nullptr)
        {
            return true;
        }
    }
    return false;
}

void Gather(void* registry, const void* cameraLocation, void* unused, void* out) noexcept
{
    reinterpret_cast<GatherFn>(g_gatherCall.Original())(registry, cameraLocation, unused, out);
    if (out == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    if (!AnyMember())
    {
        return;
    }
    auto**       data  = static_cast<void**>(ReadPointer(out, 0));
    const auto   count = ReadField<std::int32_t>(out, kArrayNum);
    std::int32_t kept  = 0;
    std::array<bool, players::kMaxPlayers> dropped{};
    for (std::int32_t i = 0; data != nullptr && i < count; ++i)
    {
        const int seat = MemberSeat(data[i]);
        if (seat == 0)
        {
            data[kept++] = data[i];  // the list's order stays the game's
        }
        else
        {
            dropped[Index(seat)] = true;
        }
    }
    if (data != nullptr && kept != count)
    {
        WriteField<std::int32_t>(out, kArrayNum, kept);
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat) || players::generation::MemberOf(seat) == nullptr ||
            dropped[Index(seat)] == g_dropping[Index(seat)])
        {
            continue;
        }
        g_dropping[Index(seat)] = dropped[Index(seat)];
        platform::LogDebug(L"near fade: {}'s character {} player 1's camera's near-fade list{}", players::SeatName(seat),
                           dropped[Index(seat)] ? L"taken out of" : L"no longer in",
                           dropped[Index(seat)] ? L" (the game's pass leaves it visible)" : L"");
    }
}

void Kick(void* fadeManager, void* actor, void* unused, const void* cameraLocation) noexcept
{
    const int seat = actor != nullptr && players::generation::OnGameThread() ? MemberSeat(actor) : 0;
    if (seat != 0)
    {
        if (g_kicks[Index(seat)]++ == 0)
        {
            platform::LogDebug(L"near fade: a camera's immediate fade of {}'s character skipped (first of this "
                               L"session)",
                               players::SeatName(seat));
        }
        return;
    }
    reinterpret_cast<KickFn>(g_kickCall.Original())(fadeManager, actor, unused, cameraLocation);
}

/// Phase A's resolve of a trace hit's actor (game thread): a seat's character, or anything attached to it (a weapon),
/// resolves to none, so the game skips the hit.
void* HitActor(const void* weak) noexcept
{
    void* actor = reinterpret_cast<WeakGetFn>(g_hitCall.Original())(weak);
    if (actor == nullptr || !players::generation::OnGameThread() || !AnyMember())
    {
        return actor;
    }
    // The actor itself, or up the attach chain a few levels (a prop on a weapon on the character).
    int         seat     = MemberSeat(actor);
    bool        attached = false;
    const void* child    = actor;
    for (int level = 0; seat == 0 && g_attachParent != nullptr && level < 4; ++level)
    {
        void* parent = nullptr;
        if (!platform::CallGuarded(g_attachParent, parent, child) || parent == nullptr)
        {
            break;
        }
        seat     = MemberSeat(parent);
        attached = seat != 0;
        child    = parent;
    }
    if (seat == 0)
    {
        return actor;
    }
    if (!g_hitDropped[Index(seat)])
    {
        g_hitDropped[Index(seat)] = true;
        platform::LogDebug(L"near fade: player 1's camera's occluder trace hit {}'s {}; the hit is skipped",
                           players::SeatName(seat), attached ? L"attachment (a weapon or prop)" : L"character");
    }
    return nullptr;
}

/// The overlap fade's player test (game thread, the character's own tick): a seat's character is a player's, so the
/// game neither fades it nor keeps it faded (it fades it back in itself).
bool OverlapPlayerTest(const void* actor) noexcept
{
    const bool game = reinterpret_cast<IsPlayerFn>(g_overlapCall.Original())(actor);
    const int  seat = !game && actor != nullptr && players::generation::OnGameThread() ? MemberSeat(actor) : 0;
    if (seat == 0)
    {
        return game;
    }
    if (!g_overlapKept[Index(seat)])
    {
        g_overlapKept[Index(seat)] = true;
        platform::LogDebug(L"near fade: the climbing overlap fade asked about {}'s character; answered as a "
                           L"player's (it stays visible)",
                           players::SeatName(seat));
    }
    return true;
}

/// A seat's character's fade object, by the game's getter (guarded), or null.
const void* FadeOfMember(void* member) noexcept
{
    void*      iface  = static_cast<std::byte*>(member) + facts::kCharacterActorInterface;
    const auto getter = reinterpret_cast<FadeOfFn>(ReadPointer(ReadPointer(iface, 0), kFadeGetter));
    void*      fade   = nullptr;
    return getter != nullptr && platform::CallGuarded(getter, fade, iface) ? fade : nullptr;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const bool gather = g_gatherCall.InstallSource(image, kGatherCall, kGather,
                                                     reinterpret_cast<std::uintptr_t>(&Gather));
    const bool kick   = g_kickCall.InstallSource(image, kKickCall, kKick, reinterpret_cast<std::uintptr_t>(&Kick));
    const auto parent = platform::FindFunction(image, kAttachParent);
    g_attachParent    = parent ? reinterpret_cast<AttachParentFn>(*parent) : nullptr;
    const bool hits   = g_hitCall.InstallSource(image, kHitResolveCall, facts::kWeakObjectGet.address,
                                                  reinterpret_cast<std::uintptr_t>(&HitActor));
    const bool overlap = g_overlapCall.InstallSource(image, kOverlapPlayerCall, kIsPlayerChar,
                                                       reinterpret_cast<std::uintptr_t>(&OverlapPlayerTest));
    platform::Log(L"near fade: player 2's or 3's character {} by player 1's camera's near fade; the cameras' immediate fade "
                  L"{}; the occluder traces {}; the climbing overlap fade {}",
                  gather ? L"is never faded" : L"is STILL faded (the gather call differs)",
                  kick ? L"skips it too" : L"is not guarded (its call differs)",
                  hits ? (g_attachParent != nullptr ? L"skip it and its attachments" : L"skip it (not its attachments)")
                       : L"STILL fade it (the hit call differs)",
                  overlap ? L"skips it" : L"STILL fades it (its call differs)");
}

void Tick() noexcept
{
#if LSS_DEV
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        FadeSeen&   seen   = g_fadeSeen[Index(seat)];
        void*       member = players::generation::OnGameThread() && players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        const void* fade   = member != nullptr ? FadeOfMember(member) : nullptr;
        if (fade == nullptr)
        {
            seen = {};
            continue;
        }
        const FadeSeen now{ReadField<std::uint32_t>(fade, kFadeReasons), ReadField<std::uint32_t>(fade, kFadeDisable),
                           ReadField<std::uint32_t>(fade, kFadeForce), ReadField<float>(fade, kFadeTarget), true};
        if (seen.read && now.reasons == seen.reasons && now.disable == seen.disable && now.force == seen.force &&
            now.target == seen.target)
        {
            continue;
        }
        seen = now;
        if ((now.reasons & 0x10u) == 0)
        {
            g_hitDropped[Index(seat)] = false;  // the next skipped hit is logged again
        }
        if ((now.force & 0x20u) == 0)
        {
            g_overlapKept[Index(seat)] = false;  // the overlap tick is off (no climb): the next climb's first answer is
                                                 // logged again
        }
        platform::LogDebug(L"near fade: {}'s character's fade: hidden reasons {:#x}{}, near-fade disable {:#x}, "
                           L"force {:#x}, opacity toward {:.2f}{}",
                           players::SeatName(seat), now.reasons, (now.reasons & 0x10u) != 0 ? L" (the near fade)" : L"",
                           now.disable, now.force, now.target,
                           g_dropping[Index(seat)] ? L"; this frame's near-fade list had it (taken out)" : L"");
    }
#endif
}

void Remove() noexcept
{
    for (platform::CallRedirect* redirect : {&g_overlapCall, &g_hitCall, &g_kickCall, &g_gatherCall})
    {
        if (redirect->IsInstalled() && !redirect->Restore())
        {
            platform::Log(L"near fade: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::camera::near_fade
