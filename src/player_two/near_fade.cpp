#include "player_two/near_fade.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <cstdint>

namespace lss::player_two::near_fade {

namespace {

// 1.004 addresses; 1.005 through replaced-near-fade.json (both call sites and callees read from both exes).
constexpr std::uintptr_t kGatherCall = 0x140A54F89;  // the near-fade pass: call EndCharaRegistry_GatherCharsWithin500OfPoint
constexpr std::uintptr_t kGather     = 0x14093D388;
constexpr std::uintptr_t kKickCall   = 0x1418969B1;  // NearFade_KickActorNow_IfGameThread: call ..._FindUnit
constexpr std::uintptr_t kKick       = 0x1418969BC;
// Phase A: player 1's camera's occluder traces toward its pivot admit every party member's mesh (its
// bUpdateJointsFromAnimation bit), so player 2's character between player 1's camera and player 1 was faded. The hit
// actor's weak pointer is resolved here; a null answer makes the game skip the hit.
constexpr std::uintptr_t kHitResolveCall = 0x140A54DCB;  // NearFade_BuildCandidateSet: call FWeakObjectPtr::Get(hit actor)
constexpr std::uintptr_t kWeakGet        = 0x141905998;
constexpr std::uint8_t   kAttachParentBytes[] = {0x48, 0x8B, 0x81, 0x38, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0C};
const platform::FunctionFact kAttachParent{L"Pawn_RootAttachParent_FieldA0", 0x1408F18AC, kAttachParentBytes};
// The overlap fade (§9): climbing walls, spider walls and one-shot moves turn on a per-character tick that fades the
// character to 0.3 (hidden reason bit 9) while another party member is inside its capsule, unless the game's player test
// says it is the player's character. Player 2's character is one too.
constexpr std::uintptr_t kOverlapPlayerCall = 0x1414D74C6;  // CharActionCtrl_TickOverlapFade_HiddenReason9: call IsUCPC_...
constexpr std::uintptr_t kIsPlayerChar      = 0x140806FE0;  // IsUCPC_WithVehicleCheck(actor)

// The gather's output, a TArray<AEndCharacterBase*>.
constexpr std::uint32_t kArrayNum = 8;

// Player 2's character's SE fade object (RE-p2-camera-near-fade.md §1.4, same in both builds): the EndActorInterface
// subobject at +0x4B8, its fade getter at vtable +0x1D0 (may return null).
constexpr std::uint32_t kCharInterface = 0x4B8;
constexpr std::uint32_t kFadeGetter    = 0x1D0;
constexpr std::uint32_t kFadeTarget    = 0x90;  // float: the opacity it fades toward
constexpr std::uint32_t kFadeReasons   = 0xA4;  // u32 hidden reasons: bit 0x10 the near fade
constexpr std::uint32_t kFadeDisable   = 0xAC;  // u32 near-fade disable reasons
constexpr std::uint32_t kFadeForce     = 0xB0;  // u32 near-fade force-enable reasons
using FadeOfFn = void* (*)(void* iface);

struct FadeSeen
{
    std::uint32_t reasons = 0, disable = 0, force = 0;
    float         target  = 1.0f;
    bool          read    = false;
};
FadeSeen g_fadeSeen;  // game thread

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

// Game thread: whether the last pass had player 2's member in its list (logged on change), and the kicks skipped.
bool     g_dropping = false;
unsigned g_kicks    = 0;
bool     g_hitDropped = false;  // the last occluder hit dropped was player 2's (logged on change)
bool     g_overlapKept = false;  // the overlap fade's last test of player 2's character answered "a player's"

void Gather(void* registry, const void* cameraLocation, void* unused, void* out) noexcept
{
    reinterpret_cast<GatherFn>(g_gatherCall.Original())(registry, cameraLocation, unused, out);
    const void* member = generation::Active() ? generation::Member() : nullptr;
    if (member == nullptr || out == nullptr || !generation::OnGameThread())
    {
        return;
    }
    auto**     data    = static_cast<void**>(ReadPointer(out, 0));
    const auto count   = ReadField<std::int32_t>(out, kArrayNum);
    std::int32_t kept  = 0;
    for (std::int32_t i = 0; data != nullptr && i < count; ++i)
    {
        if (data[i] != member)
        {
            data[kept++] = data[i];  // the list's order stays the game's
        }
    }
    const bool dropped = data != nullptr && kept != count;
    if (dropped)
    {
        WriteField<std::int32_t>(out, kArrayNum, kept);
    }
    if (dropped != g_dropping)
    {
        g_dropping = dropped;
        platform::LogDebug(L"near fade: player 2's character {} player 1's camera's near-fade list{}",
                           dropped ? L"taken out of" : L"no longer in",
                           dropped ? L" (the game's pass leaves it visible)" : L"");
    }
}

void Kick(void* fadeManager, void* actor, void* unused, const void* cameraLocation) noexcept
{
    if (actor != nullptr && generation::OnGameThread() && generation::Active() && actor == generation::Member())
    {
        if (g_kicks++ == 0)
        {
            platform::LogDebug(L"near fade: a camera's immediate fade of player 2's character skipped (first of this "
                               L"session)");
        }
        return;
    }
    reinterpret_cast<KickFn>(g_kickCall.Original())(fadeManager, actor, unused, cameraLocation);
}

/// Phase A's resolve of a trace hit's actor (game thread): player 2's character, or anything attached to it (a weapon),
/// resolves to none, so the game skips the hit.
void* HitActor(const void* weak) noexcept
{
    void*       actor  = reinterpret_cast<WeakGetFn>(g_hitCall.Original())(weak);
    const void* member = actor != nullptr && generation::OnGameThread() && generation::Active() ? generation::Member()
                                                                                               : nullptr;
    if (member == nullptr)
    {
        return actor;
    }
    // Up the attach chain a few levels (a prop on a weapon on the character).
    bool        attached = false;
    const void* child    = actor;
    for (int level = 0; actor != member && g_attachParent != nullptr && level < 4 && !attached; ++level)
    {
        void* parent = nullptr;
        if (!platform::CallGuarded(g_attachParent, parent, child) || parent == nullptr)
        {
            break;
        }
        attached = parent == member;
        child    = parent;
    }
    if (actor != member && !attached)
    {
        return actor;
    }
    if (!g_hitDropped)
    {
        g_hitDropped = true;
        platform::LogDebug(L"near fade: player 1's camera's occluder trace hit player 2's {}; the hit is skipped",
                           attached ? L"attachment (a weapon or prop)" : L"character");
    }
    return nullptr;
}

/// The overlap fade's player test (game thread, the character's own tick): player 2's character is a player's, so the
/// game neither fades it nor keeps it faded (it fades it back in itself).
bool OverlapPlayerTest(const void* actor) noexcept
{
    const bool game = reinterpret_cast<IsPlayerFn>(g_overlapCall.Original())(actor);
    if (game || actor == nullptr || !generation::OnGameThread() || !generation::Active() || actor != generation::Member())
    {
        return game;
    }
    if (!g_overlapKept)
    {
        g_overlapKept = true;
        platform::LogDebug(L"near fade: the climbing overlap fade asked about player 2's character; answered as a "
                           L"player's (it stays visible)");
    }
    return true;
}

/// Player 2's character's fade object, by the game's getter (guarded), or null.
const void* FadeOfMember(void* member) noexcept
{
    void*      iface  = static_cast<std::byte*>(member) + kCharInterface;
    const auto getter = reinterpret_cast<FadeOfFn>(ReadPointer(ReadPointer(iface, 0), kFadeGetter));
    void*      fade   = nullptr;
    return getter != nullptr && platform::CallGuarded(getter, fade, iface) ? fade : nullptr;
}

bool Redirect(const platform::GameImage& image, platform::CallRedirect& redirect, std::uintptr_t site,
              std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    const auto at = image.Rebase(site, 5);
    const auto to = image.Rebase(target);
    return at && to && redirect.Install(*at, *to, replacement);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const bool gather = Redirect(image, g_gatherCall, kGatherCall, kGather, reinterpret_cast<std::uintptr_t>(&Gather));
    const bool kick   = Redirect(image, g_kickCall, kKickCall, kKick, reinterpret_cast<std::uintptr_t>(&Kick));
    const auto parent = platform::FindFunction(image, kAttachParent);
    g_attachParent    = parent ? reinterpret_cast<AttachParentFn>(*parent) : nullptr;
    const bool hits   = Redirect(image, g_hitCall, kHitResolveCall, kWeakGet, reinterpret_cast<std::uintptr_t>(&HitActor));
    const bool overlap = Redirect(image, g_overlapCall, kOverlapPlayerCall, kIsPlayerChar,
                                  reinterpret_cast<std::uintptr_t>(&OverlapPlayerTest));
    platform::Log(L"near fade: player 2's character {} by player 1's camera's near fade; the cameras' immediate fade "
                  L"{}; the occluder traces {}; the climbing overlap fade {}",
                  gather ? L"is never faded" : L"is STILL faded (the gather call differs)",
                  kick ? L"skips it too" : L"is not guarded (its call differs)",
                  hits ? (g_attachParent != nullptr ? L"skip it and its attachments" : L"skip it (not its attachments)")
                       : L"STILL fade it (the hit call differs)",
                  overlap ? L"skips it" : L"STILL fades it (its call differs)");
}

void Tick() noexcept
{
    void*       member = generation::OnGameThread() && generation::Active() ? generation::Member() : nullptr;
    const void* fade   = member != nullptr ? FadeOfMember(member) : nullptr;
    if (fade == nullptr)
    {
        g_fadeSeen = {};
        return;
    }
    const FadeSeen now{ReadField<std::uint32_t>(fade, kFadeReasons), ReadField<std::uint32_t>(fade, kFadeDisable),
                       ReadField<std::uint32_t>(fade, kFadeForce), ReadField<float>(fade, kFadeTarget), true};
    if (g_fadeSeen.read && now.reasons == g_fadeSeen.reasons && now.disable == g_fadeSeen.disable &&
        now.force == g_fadeSeen.force && now.target == g_fadeSeen.target)
    {
        return;
    }
    g_fadeSeen = now;
    if ((now.reasons & 0x10u) == 0)
    {
        g_hitDropped = false;  // the next skipped hit is logged again
    }
    if ((now.force & 0x20u) == 0)
    {
        g_overlapKept = false;  // the overlap tick is off (no climb): the next climb's first answer is logged again
    }
    platform::LogDebug(L"near fade: player 2's character's fade: hidden reasons {:#x}{}, near-fade disable {:#x}, "
                       L"force {:#x}, opacity toward {:.2f}{}",
                       now.reasons, (now.reasons & 0x10u) != 0 ? L" (the near fade)" : L"", now.disable, now.force,
                       now.target, g_dropping ? L"; this frame's near-fade list had it (taken out)" : L"");
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

}  // namespace lss::player_two::near_fade
