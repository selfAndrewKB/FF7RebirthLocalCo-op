#include "player_two/chest_watch.h"

#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/field_pickup.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lss::player_two::chest_watch {

namespace {

// RE-p2-field-interact.md §13.5 (this build).
constexpr std::uintptr_t kChestKey          = 0x1414A8B08;  // TreasurePointMgr_OnFieldActionKey_RequestTreasureState(mgr, &key)
constexpr std::uint8_t   kChestKeyBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x10};
constexpr std::uintptr_t kLeaderTestCall    = 0x1434488E7;  // in CharCtrlStateTreasure_OnEnter
constexpr std::uintptr_t kLeaderTest        = 0x143447250;  // CharCtrlStateTreasure_IsCharLeaderOrRide(state)
constexpr std::uintptr_t kGrant             = 0x143444B64;  // CharCtrlStateTreasure_GrantRewards_MarkOpened
constexpr std::uint8_t   kGrantBytes[]      = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::size_t    kFirst             = 5;            // each entry's first instruction: mov [rsp+x], rbx
constexpr std::uint32_t  kPartyLeader       = 0x148;        // the party manager's leader, an FWeakObjectPtr
constexpr std::uint32_t  kStateBits         = 0x131;        // the treasure state's u8 bits; 0x20 granted (§13 listing)
constexpr std::uint8_t   kGrantedBit        = 0x20;
using Clock = std::chrono::steady_clock;
constexpr auto kStalledAfter = std::chrono::seconds(6);   // a press with no grant by then: the walk may be stalled (R-C2)
constexpr auto kWatchFor     = std::chrono::seconds(20);  // an open is followed this long at most

// Each wrapped function takes at most four integer arguments and no float (§13.5 listings); all four are passed on.
using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

platform::EntryRedirect g_chestKey, g_grant;
platform::CallRedirect  g_leaderTestCall;
std::uintptr_t          g_partyManager = 0;  // the pointer global's live address

// Game thread.
std::uint64_t g_pressFrame = 0;  // the last chest press that started an open

/// The open that press started: its character followed each frame until the grant (R-C2: does it reach the chest).
struct Open
{
    bool              active    = false;
    bool              warned    = false;
    void*             character = nullptr;
    std::uint64_t     weak      = 0;  // the character's weak value: it is read only while it still names it
    Clock::time_point at{};
    float             last[3]{};
    bool              lastKnown = false;
    float             moved     = 0.0f;  // cm along the way
};
Open g_open;

const wchar_t* Whose(const void* character) noexcept
{
    if (character == nullptr)
    {
        return L"nobody";
    }
    if (character == generation::Member())
    {
        return L"player 2's member";
    }
    if (character == ReadPointer(generation::PlayerOneController(), facts::kControllerPawn))
    {
        return L"player 1's character";
    }
    return L"another character (a ride body?)";
}

/// The party leader the game resolves now (PM+0x148).
const void* Leader() noexcept
{
    const void* manager = ReadPointer(reinterpret_cast<const void*>(g_partyManager), 0);
    return manager != nullptr ? leader_scope::ObjectAt(static_cast<const std::uint8_t*>(manager) + kPartyLeader) : nullptr;
}

const wchar_t* PassText() noexcept
{
    return field_pickup::InPass() ? L"in player 2's pass" : L"in player 1's (the game's own) run";
}

std::uint64_t KeyAt(std::uint64_t pointer) noexcept
{
    return pointer != 0 ? ReadField<std::uint64_t>(reinterpret_cast<const void*>(pointer), 0) : 0;
}

std::uint64_t ChestKey(std::uint64_t mgr, std::uint64_t key, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const void* leader = generation::OnGameThread() ? Leader() : nullptr;
    const auto  frame  = platform::frame_watch::FrameIndex();
    const auto  before = g_pressFrame;
    g_pressFrame       = frame;  // before the call: the treasure state's OnEnter runs inside it (§13 R-C1)
    const auto  result = reinterpret_cast<Fn>(g_chestKey.Original())(mgr, key, r8, r9);
    if ((result & 0xFF) == 0)
    {
        g_pressFrame = before;  // no open started
    }
    else if (generation::OnGameThread() && generation::Active())
    {
        g_open       = Open{true, false, const_cast<void*>(leader), 0, Clock::now()};
        g_open.weak      = leader_scope::WeakOf(g_open.character);
        g_open.lastKnown = g_open.weak != 0 && charapop::ActorLocation(g_open.character, g_open.last);
        platform::Log(L"chest watch: frame {}: chest {} pressed {}: the game opens it with {} (the leader it resolved)",
                      g_pressFrame, players::unreal::NameText(KeyAt(key)), PassText(), Whose(leader));
    }
    return result;
}

std::uint64_t LeaderTest(std::uint64_t state, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9) noexcept
{
    const auto result = reinterpret_cast<Fn>(g_leaderTestCall.Original())(state, rdx, r8, r9);
    if (generation::OnGameThread() && generation::Active())
    {
        platform::Log(L"chest watch: frame {}: the treasure state's OnEnter runs {} ({} frame(s) after the press); its "
                      L"leader test says {}: the menu locks {}",
                      platform::frame_watch::FrameIndex(), PassText(), platform::frame_watch::FrameIndex() - g_pressFrame,
                      (result & 0xFF) != 0 ? L"yes" : L"no", (result & 0xFF) != 0 ? L"apply for the open" : L"do not apply");
    }
    return result;
}

/// The grant (rcx = the state). The game also calls it as a no-op (OnExit's guarded call): a grant is logged only when
/// the call set the state's own "granted" bit.
std::uint64_t Grant(std::uint64_t state, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
{
    const auto* bits   = reinterpret_cast<const void*>(state);
    const auto  before = ReadField<std::uint8_t>(bits, kStateBits);
    const auto  result = reinterpret_cast<Fn>(g_grant.Original())(state, b, c, d);
    const auto  after  = ReadField<std::uint8_t>(bits, kStateBits);
    if (generation::OnGameThread() && generation::Active() && (before & kGrantedBit) == 0 && (after & kGrantedBit) != 0)
    {
        const auto frame = platform::frame_watch::FrameIndex();
        platform::Log(L"chest watch: frame {}: the chest's rewards granted {} ({} frame(s) after the press; the opener moved "
                      L"{:.1f} m on the way)",
                      frame, PassText(), frame - g_pressFrame, g_open.active ? g_open.moved / 100.0f : -1.0f);
        g_open.active = false;
    }
    return result;
}

/// Follows an open from its press to its grant (R-C2).
void FollowOpen() noexcept
{
    if (!g_open.active)
    {
        return;
    }
    if (g_open.weak == 0 || !leader_scope::StillNames(g_open.weak, g_open.character))
    {
        g_open.active = false;  // the opener is gone
        return;
    }
    const auto since = Clock::now() - g_open.at;
    float      now[3]{};
    if (charapop::ActorLocation(g_open.character, now))
    {
        g_open.moved += g_open.lastKnown ? charapop::Distance(now, g_open.last) : 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            g_open.last[i] = now[i];
        }
        g_open.lastKnown = true;
        platform::LogTrace(L"chest watch: frame {}: the opener at ({:.0f}, {:.0f}, {:.0f}), {:.1f} m moved since the press",
                           platform::frame_watch::FrameIndex(), now[0], now[1], now[2], g_open.moved / 100.0f);
    }
    if (since >= kStalledAfter && !g_open.warned)
    {
        g_open.warned = true;
        platform::Log(L"chest watch: WARNING: no grant {} s after the press; the opener ({}) moved {:.1f} m: the walk to the "
                      L"chest may be stalled",
                      std::chrono::duration_cast<std::chrono::seconds>(kStalledAfter).count(), Whose(g_open.character),
                      g_open.moved / 100.0f);
    }
    if (since >= kWatchFor)
    {
        g_open.active = false;
    }
}

}  // namespace

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    FollowOpen();
}

namespace {

bool Entry(const platform::GameImage& image, platform::EntryRedirect& redirect, std::uintptr_t address,
           std::span<const std::uint8_t> bytes, Fn replacement) noexcept
{
    const auto at = image.Rebase(address);
    return at && image.BytesMatch(address, bytes) &&
           redirect.Install(*at, image.EntryInstruction(address, bytes.first(kFirst)), reinterpret_cast<std::uintptr_t>(replacement));
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    g_partyManager     = manager ? *manager : 0;
    const bool chests  = g_partyManager != 0 && Entry(image, g_chestKey, kChestKey, kChestKeyBytes, &ChestKey);
    const bool grants  = Entry(image, g_grant, kGrant, kGrantBytes, &Grant);
    const auto site    = image.Rebase(kLeaderTestCall, 5);
    const auto test    = image.Rebase(kLeaderTest);
    const bool tests   = site && test && g_leaderTestCall.Install(*site, *test, reinterpret_cast<std::uintptr_t>(&LeaderTest));
    platform::Log(L"chest watch (logging only): chest presses {}, the open's leader test {}, grants {}",
                  chests ? L"watched" : L"NOT watched", tests ? L"watched" : L"NOT watched",
                  grants ? L"watched" : L"NOT watched");
}

}  // namespace lss::player_two::chest_watch
