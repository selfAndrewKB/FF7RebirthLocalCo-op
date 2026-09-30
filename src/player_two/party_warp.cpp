#include "player_two/party_warp.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lss::player_two::party_warp {

namespace {

// RE-p2-party-warp.md §1 and §7 (this build). The three slots are checked at install to hold the
// functions the research read, and a state is driven only if its vtable is this one.
constexpr std::uintptr_t kFollowStatePlayerVTable = 0x14580C8F8;
constexpr std::uint32_t  kTeleportQuerySlot       = 0x58;
constexpr std::uint32_t  kForceTeleportSlot       = 0x270;
constexpr std::uint32_t  kFinishNowSlot           = 0x278;
constexpr std::uint32_t  kUpdateTeleportSlot      = 0x298;
constexpr std::uintptr_t kTeleportQuery           = 0x140CB9618;  // FollowState_TickTeleportEQS_Vt58
constexpr std::uintptr_t kForceTeleport           = 0x14183BD94;  // FollowState_ForceTeleport_Vt270
constexpr std::uintptr_t kFinishNow               = 0x1431CF7D4;  // FollowState_FinishForcedTeleportNow_Vt278
constexpr std::uintptr_t kUpdateTeleport          = 0x140B1975C;  // FollowStatePlayer_UpdateTeleport_Vt298_UnlessCtrlAD8

// PartyManager_IsIdle (bool()), the guard PartyFollow_ForceTeleportAll uses before it forces anyone.
constexpr std::uint8_t kPartyIdleBytes[] = {0x48, 0x8B, 0x05, 0x99, 0x11, 0x88, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x07,
                                            0x80, 0x38, 0x00, 0x0F, 0x94, 0xC0, 0xC3, 0xB0, 0x01, 0xC3, 0xCC, 0xCC};
constexpr platform::FunctionFact kPartyIdle{L"PartyManager_IsIdle", 0x140B1AB40, kPartyIdleBytes};

constexpr std::uint32_t kAiBrain          = 0x518;
constexpr std::uint32_t kAiSkipsUpdate    = 0xAD8;  // the PLAYER update returns at once while set
constexpr std::uint32_t kBrainState       = 0x20;
constexpr std::uint32_t kStateType        = 0x18;
constexpr std::uint32_t kStateForced      = 0x39D;  // forced teleport pending; cleared by the commit
constexpr std::uint32_t kCharacterWarping = 0xB59;  // a warp request is queued on the character
constexpr std::uint8_t  kStateFollow      = 7;
// The teleport query runner (RE-p2-party-warp.md §10): the result pointer and its status (1 = done).
constexpr std::uint32_t kStateQueryResult = 0x540;
constexpr std::uint32_t kQueryStatus      = 0x28;

/// A forced request left unfinished this long is completed by the game's own "finish now". The game's
/// fast-travel wait allows 20 s; a stranded player should not wait that long, and the EQS, the 0.2 s fade
/// and the commit take a fraction of this when they work.
constexpr float kFinishAfterSeconds = 3.0f;

/// The force parameters ForceTeleport copies (mode @0, FTransform @0x10, bFade @0x40).
struct alignas(16) FollowTeleportParams
{
    std::uint8_t mode = 0;  // 0: the normal follow warp (FollowTeleportEQS)
    std::uint8_t padding0[15]{};
    float        rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float        translation[4]{};
    float        scale[4]{1.0f, 1.0f, 1.0f, 0.0f};
    std::uint8_t fade = 1;  // fade out before the warp, as the game's forced mode can
    std::uint8_t padding1[15]{};
};
static_assert(offsetof(FollowTeleportParams, rotation) == 0x10);
static_assert(offsetof(FollowTeleportParams, fade) == 0x40);
static_assert(sizeof(FollowTeleportParams) == 0x50);

using ForceFn  = void (*)(void* state, const FollowTeleportParams* params);
using UpdateFn = void (*)(void* state, float seconds);  // also the query slot's type
using FinishFn = void (*)(void* state);
using TestFn   = bool (*)();

// Set once at install.
std::uintptr_t g_vtable    = 0;
ForceFn        g_force     = nullptr;
UpdateFn       g_update    = nullptr;
UpdateFn       g_query     = nullptr;
FinishFn       g_finish    = nullptr;
TestFn         g_partyIdle = nullptr;
TestFn         g_inBattle  = nullptr;
bool           g_installed = false;

// Game thread.
void*  g_state   = nullptr;  // the follow state being driven
void*  g_member  = nullptr;
float  g_elapsed = 0.0f;
int    g_updates = 0;

template <class T>
bool Read(const void* object, std::uint32_t offset, T& out) noexcept
{
    return object != nullptr && platform::CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

void* PointerAt(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    return Read(object, offset, value) ? value : nullptr;
}

std::uint8_t ByteAt(const void* object, std::uint32_t offset) noexcept
{
    std::uint8_t value = 0xFF;
    Read(object, offset, value);
    return value;
}

/// The member's retained AI's current state, if it is the PLAYER follow state.
void* FollowStateOf(void* member, const void** aiOut = nullptr) noexcept
{
    void* ai    = PointerAt(member, facts::kCharacterRetainedAi);
    void* state = PointerAt(PointerAt(ai, kAiBrain), kBrainState);
    if (aiOut != nullptr)
    {
        *aiOut = ai;
    }
    std::uintptr_t vtable = 0;
    return Read(state, 0, vtable) && vtable == g_vtable && ByteAt(state, kStateType) == kStateFollow ? state : nullptr;
}

bool Test(TestFn fn, bool& out) noexcept
{
    return platform::CallGuarded(fn, out);
}

void Stop() noexcept
{
    g_state   = nullptr;
    g_member  = nullptr;
    g_elapsed = 0.0f;
    g_updates = 0;
}

/// The follow query's status for a log line: 1 = a result is ready; -1 = no result object (the query never ran).
int QueryStatus() noexcept
{
    const void* result = PointerAt(g_state, kStateQueryResult);
    std::uint8_t status = 0;
    return result == nullptr ? -1 : Read(result, kQueryStatus, status) ? status : -2;
}

/// The game's own completion of a forced request (warp to the follow target, fade in).
void FinishNow(const wchar_t* why) noexcept
{
    const int  query = QueryStatus();
    const bool ok    = platform::CallGuarded(g_finish, g_state);
    platform::Log(L"party warp: {} after {:.2f} s ({} update(s), query status {}); the game's finish-now called{}", why,
                  g_elapsed, g_updates, query, ok ? L"" : L" and FAULTED");
    Stop();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto vtable    = image.Rebase(kFollowStatePlayerVTable);
    const auto slot      = [&image](std::uint32_t offset) { return image.ReadPointer(kFollowStatePlayerVTable + offset); };
    const auto force     = slot(kForceTeleportSlot);
    const auto finish    = slot(kFinishNowSlot);
    const auto update    = slot(kUpdateTeleportSlot);
    const auto query     = slot(kTeleportQuerySlot);
    const auto partyIdle = platform::FindFunction(image, kPartyIdle);
    const auto inBattle  = platform::FindFunction(image, facts::kIsInBattle);
    const auto is        = [&image](const std::optional<std::uintptr_t>& live, std::uintptr_t ida) {
        const auto expected = image.Rebase(ida);
        return live && expected && *live == *expected;
    };
    if (!vtable || !is(force, kForceTeleport) || !is(finish, kFinishNow) || !is(update, kUpdateTeleport) || !is(query, kTeleportQuery) || !partyIdle ||
        !inBattle)
    {
        platform::Log(L"party warp: UNAVAILABLE (a check failed); a stranded player 2 is not brought back");
        return false;
    }
    g_vtable    = *vtable;
    g_force     = reinterpret_cast<ForceFn>(*force);
    g_finish    = reinterpret_cast<FinishFn>(*finish);
    g_update    = reinterpret_cast<UpdateFn>(*update);
    g_query     = reinterpret_cast<UpdateFn>(*query);
    g_partyIdle = reinterpret_cast<TestFn>(*partyIdle);
    g_inBattle  = reinterpret_cast<TestFn>(*inBattle);
    g_installed = true;
    platform::Log(L"party warp: a stranded player 2 is brought back by the game's follow warp");
    return true;
}

bool Start(void* member, bool attached, bool splitShown) noexcept
{
    if (!g_installed || g_state != nullptr || member == nullptr || !generation::OnGameThread())
    {
        return false;
    }
    const void* ai    = nullptr;
    void*       state = FollowStateOf(member, &ai);
    bool        idle = false, battle = true;
    // A forced request skips the state's own can-teleport checks, so the game's other conditions for a party
    // warp are checked here. Riding: the game warps a rider's ride body, not the rider (RE-p2-party-warp.md §4).
    const wchar_t* refused = !splitShown                                ? L"the split is not on screen"
                             : attached                                 ? L"she is attached to something (riding?)"
                             : state == nullptr                         ? L"her AI is not in its follow state"
                             : ByteAt(state, kStateForced) != 0         ? L"a forced warp is already pending"
                             : ByteAt(ai, kAiSkipsUpdate) != 0          ? L"her AI skips the teleport update"
                             : ByteAt(member, kCharacterWarping) != 0   ? L"a warp is already queued on her"
                             : !Test(g_inBattle, battle)                ? L"the battle test FAULTED"
                             : battle                                   ? L"in battle"
                             : !Test(g_partyIdle, idle)                 ? L"the party test FAULTED"
                             : !idle                                    ? L"the party manager is busy"
                                                                        : nullptr;
    if (refused != nullptr)
    {
        platform::Log(L"party warp: not started ({})", refused);
        return false;
    }
    const FollowTeleportParams params{};
    if (!platform::CallGuarded(g_force, state, &params))
    {
        platform::Log(L"party warp: the forced request FAULTED; not driven");
        return false;
    }
    g_state   = state;
    g_member  = member;
    g_elapsed = 0.0f;
    g_updates = 0;
    platform::Log(L"party warp: forced follow warp requested (mode 0, fade), pending {}", ByteAt(state, kStateForced));
    return true;
}

void Tick(void* member, bool splitShown, float seconds) noexcept
{
    if (g_state == nullptr || !generation::OnGameThread())
    {
        return;
    }
    if (member != g_member)
    {
        // Player 2 left or changed members: her AI takes her back and its own tick finishes the request.
        platform::Log(L"party warp: player 2's member changed mid-warp after {:.2f} s; left to her AI", g_elapsed);
        Stop();
        return;
    }
    if (FollowStateOf(member) != g_state)
    {
        // Her AI moved to another state under the same player. The follow state object stays in the brain
        // (it is the same one when she returns to following), so the game's finish-now still applies to it
        // and fades her back in, as long as it still is that state with the request pending.
        std::uintptr_t vtable = 0;
        if (Read(g_state, 0, vtable) && vtable == g_vtable && ByteAt(g_state, kStateForced) != 0)
        {
            FinishNow(L"her AI left its follow state mid-warp");
        }
        else
        {
            platform::Log(L"party warp: her AI left its follow state mid-warp after {:.2f} s; nothing pending", g_elapsed);
            Stop();
        }
        return;
    }
    if (ByteAt(g_state, kStateForced) == 0)
    {
        platform::Log(L"party warp: done after {:.2f} s ({} update(s)), warp queued {}", g_elapsed, g_updates,
                      ByteAt(member, kCharacterWarping));
        Stop();
        return;
    }
    bool       battle = true;
    const bool tested = Test(g_inBattle, battle);
    if (!splitShown || !tested || battle)
    {
        FinishNow(!splitShown ? L"the split left the screen mid-warp"
                  : !tested   ? L"the battle test FAULTED mid-warp"
                              : L"a battle began mid-warp");
        return;
    }
    if (g_elapsed >= kFinishAfterSeconds)
    {
        FinishNow(L"not finished by the update");
        return;
    }
    // The two per-tick calls the gated AI would make: the query tick starts the follow EQS (with her as
    // the requester) once the update has armed it; the update takes its result, fades, warps and commits.
    if (!platform::CallGuarded(g_query, g_state, seconds) || !platform::CallGuarded(g_update, g_state, seconds))
    {
        platform::Log(L"party warp: the teleport update FAULTED; finishing");
        FinishNow(L"update fault");
        return;
    }
    ++g_updates;
    g_elapsed += seconds;
}

}  // namespace lss::player_two::party_warp
