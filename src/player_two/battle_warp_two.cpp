#include "player_two/battle_warp_two.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "players/unreal_access.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace lss::player_two::battle_warp_two {

namespace {

// Facts read from the database (RE-p2-battle-gather-warp.md §S-BTLWARP2; this build).
// The exclusion set's test, bool(InlineActorArray16* set, AActor** element): a pure linear search with 16 callers, so
// only its loop-1 call in PartyBattleAIMgr_QueueBattleInTeleports is redirected (rdx = &the battle member).
constexpr std::uint8_t kContainsBytes[] = {0x48, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC1, 0x48, 0x85,
                                           0xC0, 0x4C, 0x0F, 0x45, 0xC0, 0x48, 0x63, 0x81, 0x88, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kContains{L"InlineActorArray16_Contains", 0x140A6C818, kContainsBytes};
constexpr std::uintptr_t         kBattleInCall = 0x141251364;
// The battle end's per-member placement, void(ctx{&mgr, mgr}*, character, FVector* location, bool extra); its call
// in PartyBattleAIMgr_QueueBattleFinishTeleports (non-leader members, rdx = the member).
constexpr std::uint8_t kPlaceBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x4C, 0x89,
                                        0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x41, 0x54, 0x41};
constexpr platform::FunctionFact kPlace{L"PartyBattleAIMgr_BattleEnd_PlaceMember", 0x1421DCD50, kPlaceBytes};
constexpr std::uintptr_t         kBattleFinishCall = 0x141324E04;

constexpr std::uint32_t kControllerPawn = 0x258;  // AController::Pawn

using ContainsFn = bool (*)(void* set, void* const* element);
using PlaceFn    = void (*)(void* context, void* character, void* location, bool extra);

// Set once at install.
platform::CallRedirect g_battleInCall;
platform::CallRedirect g_battleFinishCall;
/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t contains = 0, place = 0, inSite = 0, endSite = 0;  // 0: not found (logged by the finder)
};
Sites                   g_sites;
platform::DeferredSites g_placement;  // 1.005: the sites placed from the game thread (platform/deferred_sites.h)

/// Player 2's member while player 2 plays, on the game thread; else null.
void* PlayerTwoMember() noexcept
{
    return generation::OnGameThread() && generation::Active() ? generation::Member() : nullptr;
}

/// Player 2's member's distance from player 1's character in metres, or -1 if it cannot be read.
float DistanceFromPlayerOne(void* member) noexcept
{
    void* pawn = nullptr;
    if (const auto controller = reinterpret_cast<std::uintptr_t>(generation::PlayerOneController()); controller != 0)
    {
        platform::CopyGuarded(&pawn, reinterpret_cast<const void*>(controller + kControllerPawn), sizeof(pawn));
    }
    float two[3]{}, one[3]{};
    if (pawn == nullptr || !players::unreal::ActorLocation(member, two) || !players::unreal::ActorLocation(pawn, one))
    {
        return -1.0f;
    }
    const float dx = two[0] - one[0], dy = two[1] - one[1], dz = two[2] - one[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;
}

/// Replaces the battle-in queue's exclusion test: player 2's member reads as excluded, so the game skips her as it
/// skips a vehicle rider (no warp entry, no fade, no AI pause; the next member takes her slot).
bool BattleInExcluded(void* set, void* const* element) noexcept
{
    void* member = PlayerTwoMember();
    if (member != nullptr && element != nullptr && *element == member)
    {
        platform::Log(L"battle warp: battle start leaves player 2's member where it is ({:.1f} m from player 1's "
                      L"character; -1: unknown)",
                      DistanceFromPlayerOne(member));
        return true;
    }
    return reinterpret_cast<ContainsFn>(g_battleInCall.Original())(set, element);
}

/// Replaces the battle-finish queue's placement call: none for player 2's member (as the placement's own
/// early-outs: no entry, no AI pause).
void BattleFinishPlace(void* context, void* character, void* location, bool extra) noexcept
{
    void* member = PlayerTwoMember();
    if (member != nullptr && character == member)
    {
        platform::Log(L"battle warp: battle end leaves player 2's member where it is ({:.1f} m from player 1's "
                      L"character; -1: unknown)",
                      DistanceFromPlayerOne(member));
        return;
    }
    reinterpret_cast<PlaceFn>(g_battleFinishCall.Original())(context, character, location, extra);
}

/// The redirects, from what Install found.
bool PlaceSites(const platform::GameImage&) noexcept
{
    const Sites& s     = g_sites;
    const bool   start = s.contains != 0 && s.inSite != 0 &&
                         g_placement.Place(g_battleInCall, s.inSite, s.contains,
                                           reinterpret_cast<std::uintptr_t>(&BattleInExcluded));
    const bool end = s.place != 0 && s.endSite != 0 &&
                     g_placement.Place(g_battleFinishCall, s.endSite, s.place,
                                       reinterpret_cast<std::uintptr_t>(&BattleFinishPlace));
    platform::Log(L"battle warp: battle start {}; battle end {}",
                  start ? L"leaves player 2's member where it is" : L"UNAVAILABLE (a check failed): warps player 2's member in",
                  end ? L"leaves player 2's member where it is" : L"UNAVAILABLE (a check failed): may warp player 2's member back");
    return start || end;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto contains = platform::FindFunction(image, kContains);
    const auto place    = platform::FindFunction(image, kPlace);
    const auto inSite   = image.Rebase(kBattleInCall, 5);
    const auto endSite  = image.Rebase(kBattleFinishCall, 5);
    g_sites             = {contains.value_or(0), place.value_or(0), inSite.value_or(0), endSite.value_or(0)};
    // The battle-in call crosses a 16-byte block in 1.005: the sites are placed from the game thread, between its
    // ticks (platform/deferred_sites.h; battle setup runs in the game mode's tick). The placement reads only the
    // addresses found here.
    const bool placed = g_placement.Install(image, &PlaceSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"battle warp: {}",
                      placed ? L"its calls are placed from the game thread (1.005: one crosses a 16-byte block)"
                             : L"UNAVAILABLE (no room to place its calls from the game thread)");
    }
    return placed;
}

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    for (auto* call : {&g_battleFinishCall, &g_battleInCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"battle warp: a battle warp call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::battle_warp_two
