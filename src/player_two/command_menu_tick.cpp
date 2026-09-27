#include "player_two/command_menu_tick.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/command_menu_two.h"
#include "player_two/command_rules.h"
#include "player_two/engine_facts.h"
#include "player_two/focus_scope.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/lock_on.h"
#include "player_two/pad_buttons.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two::command_menu_tick {

namespace {

// Facts read from the database (RE-p2-command-menu-tick.md §13.1; this build). Each call site's five
// bytes lie inside one aligned 16-byte block (checked again by CallRedirect).
constexpr std::uint8_t kTickBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                       0x24, 0x20, 0x57, 0xB8, 0xD0, 0x00, 0x00, 0x00, 0xE8, 0x82, 0x1C, 0x67, 0x01};
constexpr platform::FunctionFact kTick{L"FieldCommandCtrl__Tick", 0x140A3D294, kTickBytes};
constexpr std::uintptr_t         kTickCall = 0x140C92978;  // the game mode's phase-0 global tick; the only caller
constexpr std::uint8_t kSlotBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x1C, 0xB5, 0x38, 0x01};
constexpr platform::FunctionFact kSlot{L"FieldSys_SetFloatSlot3C0", 0x140D23A08, kSlotBytes};
constexpr std::uintptr_t         kSlotCall = 0x140D239EA;  // in FieldCommandCtrl_Tick_MenuSlowGameSpeed0
constexpr std::uint8_t kSnapshotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                           0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
constexpr platform::FunctionFact kSnapshot{L"EndUIRequestQueue_SetTypeCommandSnapshot", 0x141406348, kSnapshotBytes};
constexpr std::uintptr_t         kSnapshotCall = 0x140A3D613;
constexpr std::uint8_t kGuideShowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                            0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x20};
constexpr platform::FunctionFact kGuideShow{L"KeyGuide53_ShowMode", 0x1410E5AD8, kGuideShowBytes};
constexpr std::uintptr_t         kGuideShowCall = 0x140A64E05;
constexpr std::uint8_t kGuideMode8Bytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xDE, 0x10, 0x86, 0x00};
constexpr platform::FunctionFact kGuideMode8{L"KeyGuide53_SetMode8", 0x14184DE48, kGuideMode8Bytes};
constexpr std::uintptr_t         kGuideMode8Call = 0x140A64E1B;
constexpr std::uint8_t kHudModeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                          0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x3B, 0x6E, 0x00, 0x01};
constexpr platform::FunctionFact kSetHudMode{L"FieldCommandCtrl_SetBattleHudMode540", 0x1410A80E0, kHudModeBytes};
constexpr std::uint8_t kResetAllBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
                                           0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0xB8,
                                           0x20, 0x00, 0x00, 0x00, 0xE8, 0x11, 0xD2, 0x15, 0x01};
constexpr platform::FunctionFact kResetAll{L"FieldCommandCtrl_ResetAll", 0x140F51D00, kResetAllBytes};
// Pad queries, answered on pad 2's view inside a ButtonScope; the action keys (FKeys).
constexpr std::uint8_t kQueryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kIsPressed{L"SquareInput_IsActionPressed_Global", 0x14080619C, kQueryBytes};
constexpr platform::FunctionFact kIsHeld{L"SquareInput_IsActionHeld_Global", 0x1408063DC, kQueryBytes};
enum Action : std::size_t
{
    kAttack,
    kUnique,
    kGuard,
    kDodge,
    kActions
};
constexpr std::array<std::uintptr_t, kActions> kActionKeys{0x1491E44D8, 0x1491E4520, 0x1491E4598, 0x1491E45E0};

// The UI system's slots: the input object [2], the command controller [23], [50] (its +0x20 and +0x2C
// are written by the member selecter's tick). The input object's +0x64 is written by the same tick.
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kInput       = 0x10;
constexpr std::uint32_t  kController  = 0xB8;
constexpr std::uint32_t  kSlot50      = 0x190;
constexpr std::uint32_t  kSlot50Mode  = 0x20;
constexpr std::uint32_t  kSlot50Other = 0x2C;
constexpr std::uint32_t  kInputByte64 = 0x64;
// The party manager's leader type (u8; PM+0x148 is the leader's weak pointer, player_two/leader_scope).
constexpr std::uint32_t kLeaderType = 0x150;
constexpr std::uint32_t kPrimary    = 0x140;
constexpr std::uint32_t kLeaderWeak = 0x148;

// The controller (RE-p2-command-menu-tick.md §1, §2, §7).
constexpr std::uint32_t kStoredLeaderType = 0x30;   // u8
constexpr std::uint32_t kMembers          = 0x34;   // i32 x2: leader ordinal, ally
constexpr std::uint32_t kBattleInput      = 0x25C;  // u8
constexpr std::uint32_t kSwitchingLayer   = 0x258;  // u8
constexpr std::uint32_t kRequests         = 0x570;  // TArray<int32> {data, num}
constexpr std::uint32_t kHudMode          = 0x540;  // u8
constexpr std::uint32_t kBlocked          = 0x541;  // u8
constexpr std::uint32_t kReady            = 0x5E1;  // u8
constexpr std::uint32_t kWaitFilter       = 0x644;  // i32
constexpr std::uint32_t kPinch            = 0x650;  // u8
constexpr std::uint32_t kGuardLatch       = 0x760;  // i32
constexpr std::uint32_t kHudSoundMode     = 0x774;  // u8
constexpr std::uint32_t kReadyB           = 0x782;  // u8
constexpr std::uint32_t kMenuLayer        = 0x783;  // u8
constexpr std::array<std::uint32_t, 4> kLoggedGates{0x776, 0x77C, 0x77F, 0x787};  // u8, logged only
enum Request : std::int32_t
{
    kPrevMember = 0,
    kNextMember = 1,
    kReqAttack  = 12,
    kReqUnique  = 13,
    kReqGuardDodge = 32,
    kReqGuard   = 37,
};
constexpr std::array<std::int32_t, 11> kLoggedRequests{0, 1, 5, 6, 12, 13, 25, 32, 37, 38, 39};
constexpr std::int32_t kDodgePress    = 3;  // req[32]: a dodge press
constexpr std::int32_t kDodgeGuarding = 4;  // req[32]: a dodge press while guarding
constexpr std::int32_t kGuardHeld     = 1;  // req[32]: guard held

constexpr std::size_t kSnapshotTypes = 15;
constexpr int         kMaxLines      = 400;

using TickFn      = void (*)(void* controller, float deltaTime);
using SlotFn      = void (*)(std::uint8_t slot, float value);
using SnapshotFn  = void (*)(void* queue, const std::uint64_t* commands, const std::uint8_t* categories);
using GuideShowFn = std::uint8_t (*)(void* guide, std::uint8_t mode);
using GuideMode8Fn = void (*)();
using HudModeFn   = void (*)(void* controller, std::uint8_t mode);
using ResetAllFn  = void (*)(void* controller);
using QueryFn     = std::uint8_t (*)(const void* key);

// Set once at install.
platform::CallRedirect g_tickCall, g_slotCall, g_snapshotCall, g_guideShowCall, g_guideMode8Call;
HudModeFn      g_hudMode   = nullptr;
ResetAllFn     g_resetAll  = nullptr;
QueryFn        g_isPressed = nullptr;
QueryFn        g_isHeld    = nullptr;
std::array<std::uintptr_t, kActions> g_keys{};
std::uintptr_t g_endUISystem  = 0;
std::uintptr_t g_partyManager = 0;  // the address holding the party manager

// Game thread.
bool         g_installed = false;
int          g_pass      = 0;  // 1: the game's controller ticks; 2: player 2's
std::uint64_t g_frame     = 0;  // ticks of the game's controller
std::uint64_t g_oneFrame  = 0;  // the frame player 1's slot value is from
float        g_oneSlot   = 1.0F;
float        g_twoSlot   = 1.0F;
float        g_written   = 1.0F;
bool         g_passed    = false;  // player 2's controller ticked this frame
int          g_lines     = 0;
unsigned     g_refusals  = 0, g_shieldHits = 0, g_guideSkips = 0, g_snapshotEntries = 0;

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    if (address != 0)
    {
        platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(T));
    }
    return value;
}

template <class T>
bool Put(std::uintptr_t address, T value) noexcept
{
    return address != 0 && platform::WriteGuarded(reinterpret_cast<void*>(address), &value, sizeof(T));
}

void Line(const std::wstring& text) noexcept
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"command menu tick: {}", text);
    }
}

std::uint8_t Query(QueryFn query, std::size_t action) noexcept
{
    std::uint8_t answer = 0;
    return platform::CallGuarded(query, answer, reinterpret_cast<const void*>(g_keys[action])) ? answer : 0;
}

std::int32_t RequestOf(std::uintptr_t controller, std::int32_t index) noexcept
{
    const auto data = Field<std::uintptr_t>(controller + kRequests);
    return data != 0 && index < Field<std::int32_t>(controller + kRequests + 8)
               ? Field<std::int32_t>(data + 4 * static_cast<std::uint32_t>(index))
               : -1;
}

void SetRequest(std::uintptr_t controller, std::int32_t index, std::int32_t value) noexcept
{
    const auto data = Field<std::uintptr_t>(controller + kRequests);
    if (data != 0 && index < Field<std::int32_t>(controller + kRequests + 8))
    {
        Put(data + 4 * static_cast<std::uint32_t>(index), value);
    }
}

/// The party manager's leader type answering player 2's member while alive (the leader's weak
/// pointer is player_two/leader_scope's).
class LeaderTypeScope
{
public:
    LeaderTypeScope(std::uintptr_t manager, std::uint8_t type) noexcept
        : address_(manager != 0 ? manager + kLeaderType : 0), saved_(Field<std::uint8_t>(address_))
    {
        held_ = Put(address_, type);
    }
    ~LeaderTypeScope()
    {
        if (held_)
        {
            Put(address_, saved_);
        }
    }
    LeaderTypeScope(const LeaderTypeScope&)            = delete;
    LeaderTypeScope& operator=(const LeaderTypeScope&) = delete;
    bool Held() const noexcept { return held_; }

private:
    std::uintptr_t address_ = 0;
    std::uint8_t   saved_   = 0;
    bool           held_    = false;
};

/// Pad 2's four actions written into player 2's controller, as the game's input handler and up route
/// leave them (RE-p2-command-menu-tick.md §2, §8, §13.2 step 3).
void WriteRequests(std::uintptr_t two, std::uintptr_t one) noexcept
{
    SetRequest(two, kPrevMember, 0);  // player 2's d-pad never switches the party leader
    SetRequest(two, kNextMember, 0);
    const bool live = Field<std::uint8_t>(one + kBattleInput) != 0 && Field<std::uint8_t>(one + kMenuLayer) != 0 &&
                      Field<std::uint8_t>(two + kHudMode) != 0;
    const bool guard  = live && Query(g_isHeld, kGuard) != 0;
    const bool attack = live && Query(g_isHeld, kAttack) != 0;
    const bool unique = live && Query(g_isHeld, kUnique) != 0;
    const bool dodge  = live && Query(g_isPressed, kDodge) != 0 && Field<std::uint8_t>(two + kSwitchingLayer) == 0;
    SetRequest(two, kReqGuard, guard ? 1 : 0);
    SetRequest(two, kReqAttack, attack && !guard ? 1 : 0);
    SetRequest(two, kReqUnique, unique && !guard ? 1 : 0);
    SetRequest(two, kReqGuardDodge, dodge ? (guard ? kDodgeGuarding : kDodgePress) : (guard ? kGuardHeld : 0));
}

/// Player 2's controller follows the game's battle HUD mode through its own setter (§4.2, §7).
void MirrorHudMode(std::uintptr_t two, std::uintptr_t one, std::uint8_t memberType) noexcept
{
    const auto mode = Field<std::uint8_t>(one + kHudMode);
    if (Field<std::uint8_t>(two + kHudMode) != mode)
    {
        if (mode != 0)
        {
            Put(two + kStoredLeaderType, memberType);  // no leader-change handler on player 1's leader
        }
        const bool set   = platform::CallGuarded(g_hudMode, reinterpret_cast<void*>(two), mode);
        const bool reset = mode != 0 || platform::CallGuarded(g_resetAll, reinterpret_cast<void*>(two));
        Line(std::format(L"player 2's controller battle HUD mode -> {}{}{}", mode, set ? L"" : L" (the setter FAULTED)",
                         mode != 0 ? L"" : (reset ? L"; reset" : L"; the reset FAULTED")));
    }
    Put(two + kReadyB, Field<std::uint8_t>(one + kReadyB));  // every pass: the battle readiness stays live
}

struct Shielded
{
    std::int32_t mode = 0, other = 0;
    std::uint8_t input64 = 0;
    bool operator==(const Shielded&) const = default;
};

Shielded ReadShielded(std::uintptr_t slot50, std::uintptr_t input) noexcept
{
    return {Field<std::int32_t>(slot50 + kSlot50Mode), Field<std::int32_t>(slot50 + kSlot50Other),
            Field<std::uint8_t>(input + kInputByte64)};
}

void RestoreShielded(std::uintptr_t slot50, std::uintptr_t input, const Shielded& saved) noexcept
{
    if (ReadShielded(slot50, input) == saved)
    {
        return;
    }
    ++g_shieldHits;
    Put(slot50 + kSlot50Mode, saved.mode);
    Put(slot50 + kSlot50Other, saved.other);
    Put(input + kInputByte64, saved.input64);
    if (g_shieldHits <= 5 || g_shieldHits % 600 == 0)
    {
        Line(std::format(L"player 2's tick changed a shared UI value; put back ({} time(s))", g_shieldHits));
    }
}

/// What the log shows of player 2's controller, as raw values.
struct Snapshot
{
    std::array<std::int32_t, kLoggedRequests.size()> requests{};
    std::array<std::uint8_t, 7>  bytes{};  // 540 541 5E1 782 25C 258 774
    std::array<std::uint8_t, kLoggedGates.size()> gates{};
    std::int32_t guardLatch = 0, waitFilter = 0;
    std::uint8_t storedType = 0, pinch = 0;
    std::array<std::int32_t, 2> members{};
    bool operator==(const Snapshot&) const = default;
};
Snapshot g_logged{};
bool     g_loggedOnce = false;

void Watch(std::uintptr_t two) noexcept
{
    if (g_lines >= kMaxLines)
    {
        return;
    }
    Snapshot now;
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        now.requests[i] = RequestOf(two, kLoggedRequests[i]);
    }
    constexpr std::array<std::uint32_t, 7> kBytes{kHudMode, kBlocked, kReady, kReadyB, kBattleInput, kSwitchingLayer,
                                                  kHudSoundMode};
    for (std::size_t i = 0; i < kBytes.size(); ++i)
    {
        now.bytes[i] = Field<std::uint8_t>(two + kBytes[i]);
    }
    for (std::size_t i = 0; i < kLoggedGates.size(); ++i)
    {
        now.gates[i] = Field<std::uint8_t>(two + kLoggedGates[i]);
    }
    now.guardLatch = Field<std::int32_t>(two + kGuardLatch);
    now.waitFilter = Field<std::int32_t>(two + kWaitFilter);
    now.storedType = Field<std::uint8_t>(two + kStoredLeaderType);
    now.pinch      = Field<std::uint8_t>(two + kPinch);
    now.members    = {Field<std::int32_t>(two + kMembers), Field<std::int32_t>(two + kMembers + 4)};
    if (g_loggedOnce && now == g_logged)
    {
        return;
    }
    g_logged     = now;
    g_loggedOnce = true;
    std::wstring requests;
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        requests += std::format(L" {}:{}", kLoggedRequests[i], now.requests[i]);
    }
    const auto& b = now.bytes;
    const auto& g = now.gates;
    Line(std::format(L"player 2's controller: req{} | guard latch {} | 540={} 541={} 5E1={} 782={} 25C={} 258={} 774={} | "
                     L"type {} leader {} ally {} | wait {} pinch {} | 776={} 77C={} 77F={} 787={} | slot 0: P1 {:.3f} "
                     L"P2 {:.3f} written {:.3f}",
                     requests, now.guardLatch, b[0], b[1], b[2], b[3], b[4], b[5], b[6], now.storedType, now.members[0],
                     now.members[1], now.waitFilter, now.pinch, g[0], g[1], g[2], g[3], g_oneSlot, g_twoSlot, g_written));
}

void Refused(const wchar_t* why) noexcept
{
    ++g_refusals;
    if (g_refusals <= 5 || g_refusals % 600 == 0)
    {
        Line(std::format(L"player 2's pass skipped: {} ({} time(s))", why, g_refusals));
    }
}

/// Player 2's controller's tick, inside player 2's scopes (§13.2).
void PlayerTwoPass(float deltaTime) noexcept
{
    const auto two    = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    const auto one    = Field<std::uintptr_t>(g_endUISystem + kController);
    void*      member = generation::Member();
    if (two == 0 || one == 0 || one == two || member == nullptr ||
        (Field<std::uint8_t>(one + kHudMode) == 0 && Field<std::uint8_t>(two + kHudMode) == 0))
    {
        return;  // out of battle: player 2's controller has nothing to do yet
    }
    const auto memberType = party_member::PlayerTypeOf(member);
    const auto memberWeak = leader_scope::WeakOf(member);
    const auto manager    = Field<std::uintptr_t>(g_partyManager);
    if (!memberType || memberWeak == 0 || manager == 0)
    {
        return Refused(L"player 2's member could not be read");
    }
    const auto before = std::array<std::uint64_t, 3>{Field<std::uint64_t>(manager + kPrimary),
                                                     Field<std::uint64_t>(manager + kLeaderWeak),
                                                     Field<std::uint8_t>(manager + kLeaderType)};
    const auto leaderType = static_cast<std::uint8_t>(before[2]);
    {
        focus_scope::FocusScope focus(reinterpret_cast<void*>(two));
        if (!focus.Entered())
        {
            return Refused(focus.Refusal());
        }
        const pad_sticks::StickScope    sticks(pad_sticks::ControllerIdOf(generation::PlayerTwoController()));
        const primary_controller::Scope primary;
        if (!sticks.Held() || !primary.Held())
        {
            return Refused(L"pad 2's sticks or the primary controller");
        }
        const pad_buttons::ButtonScope   buttons;
        const lock_on::TargetScope       targets;
        const leader_scope::LeaderScope  leader(memberWeak);
        const LeaderTypeScope            type(manager, *memberType);
        if (!buttons.Held() || !leader.Held() || !type.Held())
        {
            return Refused(L"pad 2's buttons or the party leader");
        }
        const command_rules::PlayerTwoRules rules(leaderType);
        g_pass = 2;
        MirrorHudMode(two, one, *memberType);
        WriteRequests(two, one);
        Put(two + kHudSoundMode, Field<std::uint8_t>(two + kHudMode));  // no second battle-HUD sound
        const auto     slot50 = Field<std::uintptr_t>(g_endUISystem + kSlot50);
        const auto     input  = Field<std::uintptr_t>(g_endUISystem + kInput);
        const Shielded saved  = ReadShielded(slot50, input);
        const bool     ticked = platform::CallGuarded(reinterpret_cast<TickFn>(g_tickCall.Original()),
                                                      reinterpret_cast<void*>(two), deltaTime);
        RestoreShielded(slot50, input, saved);
        g_pass   = 0;
        g_passed = ticked;
        if (!ticked)
        {
            focus.Abandon();
            g_installed = false;
            Line(L"FAILED: player 2's controller's tick faulted; player 2's command controller stops (the direct actions "
                 L"come back)");
        }
        Watch(two);
    }
    const auto after = std::array<std::uint64_t, 3>{Field<std::uint64_t>(manager + kPrimary),
                                                    Field<std::uint64_t>(manager + kLeaderWeak),
                                                    Field<std::uint8_t>(manager + kLeaderType)};
    if (after != before)
    {
        Line(L"WARNING: the primary controller or the party leader differs after player 2's pass");
    }
}

/// Replaces the game's controller tick call: the game's controller, then player 2's.
void Tick(void* controller, float deltaTime) noexcept
{
    ++g_frame;
    g_pass   = 1;
    reinterpret_cast<TickFn>(g_tickCall.Original())(controller, deltaTime);
    g_pass   = 0;
    g_passed = false;
    if (g_installed && generation::OnGameThread() && generation::Active())
    {
        PlayerTwoPass(deltaTime);
    }
}

/// Replaces the world-slow write of every controller tick: either menu slows the world (policy A).
void Slot(std::uint8_t slot, float value) noexcept
{
    const auto original = reinterpret_cast<SlotFn>(g_slotCall.Original());
    if (g_pass == 2)
    {
        g_twoSlot = value;
        if (g_oneFrame != g_frame)
        {
            Line(L"WARNING: player 2's slow value came without player 1's this frame; written as is");
        }
        g_written = g_oneFrame == g_frame ? std::min(g_oneSlot, value) : value;
        return original(slot, g_written);
    }
    if (g_pass == 1)
    {
        g_oneSlot  = value;
        g_oneFrame = g_frame;
        g_written  = value;
    }
    original(slot, value);
}

/// Replaces the per-type command snapshot write: player 2's pass leaves the game's in place.
void CommandSnapshot(void* queue, const std::uint64_t* commands, const std::uint8_t* categories) noexcept
{
    if (g_pass != 2)
    {
        return reinterpret_cast<SnapshotFn>(g_snapshotCall.Original())(queue, commands, categories);
    }
    std::array<std::uint64_t, kSnapshotTypes> entries{};
    platform::CopyGuarded(entries.data(), commands, sizeof(entries));
    const auto used = static_cast<unsigned>(std::count_if(entries.begin(), entries.end(), [](auto e) { return e != 0; }));
    if (used != g_snapshotEntries)
    {
        g_snapshotEntries = used;
        Line(std::format(L"player 2's command snapshot (not written): {} type(s) with a command", used));
    }
}

std::uint8_t GuideShow(void* guide, std::uint8_t mode) noexcept
{
    if (g_pass == 2)
    {
        ++g_guideSkips;
        Line(std::format(L"player 2's key-guide mode {} not shown ({} time(s))", mode, g_guideSkips));
        return 0;
    }
    return reinterpret_cast<GuideShowFn>(g_guideShowCall.Original())(guide, mode);
}

void GuideMode8() noexcept
{
    if (g_pass == 2)
    {
        ++g_guideSkips;
        Line(std::format(L"player 2's key-guide mode 8 not set ({} time(s))", g_guideSkips));
        return;
    }
    reinterpret_cast<GuideMode8Fn>(g_guideMode8Call.Original())();
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto tick = find(kTick), slot = find(kSlot), snapshot = find(kSnapshot), show = find(kGuideShow),
               mode8 = find(kGuideMode8), hud = find(kSetHudMode), reset = find(kResetAll), pressed = find(kIsPressed),
               held = find(kIsHeld);
    const auto system  = image.Rebase(kEndUISystem, kSlot50 + sizeof(void*));
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    bool       ok      = tick && slot && snapshot && show && mode8 && hud && reset && pressed && held && system && manager;
    for (std::size_t i = 0; ok && i < kActions; ++i)
    {
        const auto key = image.Rebase(kActionKeys[i], 24);
        g_keys[i]      = key ? *key : 0;
        ok             = key.has_value();
    }
    if (ok)
    {
        g_hudMode      = reinterpret_cast<HudModeFn>(*hud);
        g_resetAll     = reinterpret_cast<ResetAllFn>(*reset);
        g_isPressed    = reinterpret_cast<QueryFn>(*pressed);
        g_isHeld       = reinterpret_cast<QueryFn>(*held);
        g_endUISystem  = *system;
        g_partyManager = *manager;
    }
    // The shields first, the tick last: no pass runs before its shields are in place.
    const auto redirect = [&image](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto live = image.Rebase(site, 5);
        return live && call.Install(*live, target, reinterpret_cast<std::uintptr_t>(with));
    };
    ok = ok && redirect(g_slotCall, kSlotCall, *slot, &Slot) && redirect(g_snapshotCall, kSnapshotCall, *snapshot, &CommandSnapshot) &&
         redirect(g_guideShowCall, kGuideShowCall, *show, &GuideShow) &&
         redirect(g_guideMode8Call, kGuideMode8Call, *mode8, &GuideMode8) &&
         redirect(g_tickCall, kTickCall, *tick, &Tick);
    g_installed = ok;
    if (!ok)
    {
        Remove();
    }
    platform::Log(L"command menu tick: {}", ok ? L"player 2's command controller ticks in battle and takes player 2's "
                                                 L"attack, guard and dodge (logged)"
                                               : L"UNAVAILABLE (a check failed): player 2's actions stay direct");
    return ok;
}

void Remove() noexcept
{
    g_installed = false;
    for (auto* call : {&g_tickCall, &g_guideMode8Call, &g_guideShowCall, &g_snapshotCall, &g_slotCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu tick: a call was changed after us; left in place (it passes through)");
        }
    }
}

// A frame whose pass is refused (a prompt holds player 1's capture map, a scope cannot be held) leaves
// player 2's actions paused rather than switching drivers mid-action: the direct calls and the
// controller's requests keep different hold state (logged as refusals).
bool DrivesActions() noexcept
{
    return g_installed && command_menu_two::Controller() != nullptr;
}

}  // namespace lss::player_two::command_menu_tick
