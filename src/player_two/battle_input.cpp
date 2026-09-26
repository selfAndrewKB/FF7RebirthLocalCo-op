#include "player_two/battle_input.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/battle_camera.h"
#include "player_two/camera_override.h"
#include "player_two/command_rules.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/lock_on.h"
#include "player_two/object_fields.h"

#include <array>
#include <optional>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::battle_input {

namespace {

// Facts read from the database (RE-p2-battle-input.md §1 to §5; this build). Each function's first
// bytes, through its stack probe, are checked before it is called.
constexpr std::uint8_t kAttackBeginBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8,
                                                0x48, 0xE3, 0x17, 0x01, 0x48, 0x2B, 0xE0, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t kAttackHoldBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
                                                0x40, 0x00, 0x00, 0x00, 0xE8, 0x5B, 0xD8, 0x74, 0x01, 0x48, 0x2B, 0xE0,
                                                0x0F, 0xB6, 0xF2, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t kAttackReleaseBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8,
                                                0x4C, 0xC4, 0x17, 0x01, 0x48, 0x2B, 0xE0, 0x40, 0x8A, 0xFA, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t kGuardHoldBytes[]     = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xF4, 0xD8, 0x74, 0x01, 0x48,
                                                0x2B, 0xE0, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x28, 0x0A, 0x00, 0x00};
constexpr std::uint8_t kGuardEndBytes[]      = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x00, 0xC2, 0x17, 0x01, 0x48,
                                                0x2B, 0xE0, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x28, 0x0A, 0x00, 0x00};
constexpr std::uint8_t kDodgeBytes[]         = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x90, 0xE9, 0x8F, 0x00, 0x48,
                                                0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x89, 0x38,
                                                0x06, 0x00, 0x00};
constexpr std::uint8_t kActionQueryBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8};
constexpr std::uint8_t kSoftTargetBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56,
                                                0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C,
                                                0x24, 0xE0, 0xB8, 0x20, 0x01, 0x00, 0x00, 0xE8};

constexpr platform::FunctionFact kAttackBegin{L"MoveState_BattleAttackBegin", 0x140F30BD8, kAttackBeginBytes};
constexpr platform::FunctionFact kAttackHold{L"MoveState_BattleAttackHold", 0x1409616C0, kAttackHoldBytes};
constexpr platform::FunctionFact kAttackRelease{L"MoveState_BattleAttackRelease", 0x140F32AD4, kAttackReleaseBytes};
constexpr platform::FunctionFact kGuardHold{L"CharHolder1758_BattleGuardHold", 0x140961630, kGuardHoldBytes};
constexpr platform::FunctionFact kGuardEnd{L"CharHolder1758_BattleGuardEnd", 0x140F32D24, kGuardEndBytes};
constexpr platform::FunctionFact kDodge{L"MoveState_BattleDodge_QueuePending9", 0x1417B0594, kDodgeBytes};
constexpr platform::FunctionFact kIsPressed{L"SquareInput_IsActionPressed_Global", 0x14080619C, kActionQueryBytes};
constexpr platform::FunctionFact kIsHeld{L"SquareInput_IsActionHeld_Global", 0x1408063DC, kActionQueryBytes};
constexpr platform::FunctionFact kSoftTarget{L"BattleTarget_UpdateLeaderSoftTarget", 0x140C8F104, kSoftTargetBytes};

// The battle target system (RE-p2-targeting.md §1, §2; a pointer global): the lock byte, the switch
// latch, the skip byte, the displayed target. The soft target itself lives in each character's
// BattlePlayer component (+0x258 object, +0x260 its reference controller).
constexpr std::uintptr_t kBattleSystem   = 0x14939BC58;
constexpr std::uint32_t  kLockMode       = 0x8C;
constexpr std::uint32_t  kSwitchLatch    = 0x8D;
constexpr std::uint32_t  kSkipTargeting  = 0x96;
constexpr std::uint32_t  kDisplayed      = 0x70;
constexpr std::uint32_t  kTargetSlot     = 0x258;
// The party manager's leader, an FWeakObjectPtr {index, serial} read by ResolvePartyLeaderWeakPtr.
constexpr std::uint32_t  kPartyManagerLeader = 0x148;

// The actions' FKeys (24 bytes each), in the order of Action.
constexpr std::array<std::uintptr_t, 4> kActionKeys{0x1491E44D8, 0x1491E4520, 0x1491E4598, 0x1491E45E0};
enum Action : std::size_t
{
    kAttack,
    kUnique,
    kGuard,
    kDodgeAction,
    kActions,
};

// The command controller (read only): its battle-phase gates, guard mode and request array.
constexpr std::uintptr_t kEndUISystem       = 0x1493496E0;
constexpr std::uint32_t  kController        = 0xB8;
constexpr std::uint32_t  kBattleHud         = 0x540;  // u8
constexpr std::uint32_t  kBattleBlocked     = 0x541;  // u8
constexpr std::uint32_t  kBattleReadyA      = 0x5E1;  // u8
constexpr std::uint32_t  kBattleReadyB      = 0x782;  // u8
constexpr std::uint32_t  kGuardMode         = 0x760;  // int32
constexpr std::uint32_t  kRequests          = 0x570;  // TArray<int32> {data, num}
constexpr std::array<std::int32_t, 10> kLoggedRequests{5, 6, 12, 13, 25, 26, 32, 37, 38, 39};

// A party character's battle parts (§1.4, §1.5).
constexpr std::uint32_t  kCharacterHolder   = 0x1758;  // holder whose +0 is the character
constexpr std::uint32_t  kCharacterAiHolder = 0x638;   // its vt+0x48 gives the BattlePlayer component
constexpr std::uint32_t  kOwnerSlot         = 0x48;
constexpr std::uintptr_t kBattlePlayerVTable = 0x145795ED8;
constexpr std::uint32_t  kCommandHandle     = 0x418;  // the component's handle; null: the actions do nothing
constexpr std::uint32_t  kHandleMode        = 0x112;  // u8 in the handle's object
constexpr std::array<std::uint32_t, 4> kStateBytes{0x49, 0x4A, 0x64, 0x65};
constexpr std::uint32_t  kPendingAction     = 0x84;   // int32

constexpr int kMaxLines = 600;

enum Part : unsigned
{
    kPartDodge       = 1,
    kPartAttack      = 2,
    kPartGuard       = 4,
    kPartSoftTarget  = 8,   // the game's leader soft-target update run for player 2's member
    kPartAttackScope = 16,  // the attack calls run with the leader answering player 2's member
    kPartLock        = 32,  // the TargetLock press toggles player 2's own lock (player_two/lock_on.h)
    kPartSwitch      = 64,  // pad 2's right stick switches player 2's locked target
    kPartLockCamera  = 128, // the LockOn module on player 2's camera follows player 2's lock and target
    kPartCursor      = 256, // player 2's own target cursor
};

using StateActionFn  = void (*)(void* state, std::uint8_t unique);
using HolderActionFn = void (*)(void* holder);
using DodgeFn        = void (*)(void* state);
using QueryFn        = std::uint8_t (*)(const void* key);
using OwnerFn        = void* (*)(void* aiHolder);
using SoftTargetFn   = void (*)(void* system);
using GlobalTestFn   = std::uint8_t (*)();
using GlobalObjectFn = void* (*)();

// Set once at install.
bool           g_installed = false;
StateActionFn  g_attackBegin   = nullptr;
StateActionFn  g_attackHold    = nullptr;
StateActionFn  g_attackRelease = nullptr;
HolderActionFn g_guardHold     = nullptr;
HolderActionFn g_guardEnd      = nullptr;
DodgeFn        g_dodge         = nullptr;
QueryFn        g_isPressed     = nullptr;
QueryFn        g_isHeld        = nullptr;
std::array<std::uintptr_t, kActions> g_keys{};
std::uintptr_t g_endUISystem   = 0;
std::uintptr_t g_battlePlayerVTable = 0;
SoftTargetFn   g_softTarget    = nullptr;
GlobalTestFn   g_isInBattle    = nullptr;
GlobalObjectFn g_resolveLeader = nullptr;
std::uintptr_t g_battleSystem  = 0;  // the address holding the battle system
std::uintptr_t g_partyManager  = 0;  // the address holding the party manager

// Game thread.
unsigned                 g_mask = kPartDodge | kPartAttack | kPartGuard | kPartSoftTarget | kPartAttackScope | kPartLock |
                                  kPartSwitch | kPartLockCamera | kPartCursor;
bool                     g_guarding = false;
std::array<bool, 2>      g_holdStarted{};
bool                     g_failed = false;
int                      g_lines = 0;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    Read(address, value);
    return value;
}

bool Acts(Part part) noexcept
{
    return (g_mask & part) != 0;
}

std::uint8_t QueryGuarded(QueryFn query, std::uintptr_t key) noexcept
{
    __try
    {
        return query(reinterpret_cast<const void*>(key));
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return 0;
    }
}

void* OwnerGuarded(std::uintptr_t aiHolder) noexcept
{
    __try
    {
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(aiHolder);
        return reinterpret_cast<OwnerFn>(*reinterpret_cast<const std::uintptr_t*>(vtable + kOwnerSlot))(
            reinterpret_cast<void*>(aiHolder));
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return nullptr;
    }
}

bool CallGuarded(StateActionFn action, void* state, std::uint8_t unique) noexcept
{
    __try
    {
        action(state, unique);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// The one-argument actions (guard hold and end take the holder, dodge the move state).
bool CallGuarded(void (*action)(void*), void* argument) noexcept
{
    __try
    {
        action(argument);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// A character's battle parts, as the game's per-character functions need them.
struct Parts
{
    std::uintptr_t holder  = 0;  // [char+0x1758], whose +0 must be the character
    std::uintptr_t owner   = 0;  // the BattlePlayer component
    std::uintptr_t handle  = 0;  // owner+0x418
    std::uint8_t   mode    = 0;  // handle+0x112
    bool           ready   = false;
};

Parts PartsOf(std::uintptr_t character) noexcept
{
    Parts parts;
    std::uintptr_t first = 0, aiHolder = 0;
    if (character == 0 || !Read(character + kCharacterHolder, parts.holder) || parts.holder == 0 ||
        !Read(parts.holder, first) || first != character || !Read(character + kCharacterAiHolder, aiHolder) || aiHolder == 0)
    {
        return parts;
    }
    parts.owner = reinterpret_cast<std::uintptr_t>(OwnerGuarded(aiHolder));
    if (parts.owner == 0 || Field<std::uintptr_t>(parts.owner) != g_battlePlayerVTable)
    {
        return parts;
    }
    parts.handle = Field<std::uintptr_t>(parts.owner + kCommandHandle);
    parts.mode   = parts.handle != 0 ? Field<std::uint8_t>(parts.handle + kHandleMode) : 0;
    parts.ready  = parts.handle != 0;
    return parts;
}

/// One character's battle preconditions and move-state bytes, as the log shows them.
struct CharacterState
{
    bool         holder = false, owner = false, handle = false;
    std::uint8_t mode   = 0;
    std::array<std::uint8_t, kStateBytes.size()> bytes{};
    std::int32_t pending = 0;
    bool operator==(const CharacterState&) const = default;
};

CharacterState CharacterOf(std::uintptr_t state, const Parts& parts) noexcept
{
    CharacterState character{parts.holder != 0, parts.owner != 0, parts.handle != 0, parts.mode};
    for (std::size_t i = 0; i < kStateBytes.size(); ++i)
    {
        character.bytes[i] = Field<std::uint8_t>(state + kStateBytes[i]);
    }
    character.pending = state != 0 ? Field<std::int32_t>(state + kPendingAction) : 0;
    return character;
}

/// Everything the per-frame log shows, as raw values.
struct Snapshot
{
    bool                                   hud = false, blocked = false, ready = false;
    std::int32_t                           guardMode = 0;
    std::array<std::int32_t, kLoggedRequests.size()> requests{};
    std::array<CharacterState, 2>          characters{};
    bool                                   targeting = false;
    std::uint8_t                           lock = 0, latch = 0;
    std::uintptr_t                         displayed = 0;
    std::array<std::uintptr_t, 2>          targets{};
    std::uint64_t                          leaderWeak = 0, memberWeak = 0;
    std::array<bool, kActions>             pressed{}, held{};
    bool operator==(const Snapshot&) const = default;
};

std::wstring DescribeCharacter(const wchar_t* who, const CharacterState& character)
{
    std::wstring text = std::format(L"{} holder {} component {} handle {} mode {} state", who, character.holder ? 1 : 0,
                                    character.owner ? 1 : 0, character.handle ? 1 : 0, character.mode);
    for (const std::uint8_t byte : character.bytes)
    {
        text += std::format(L" {}", byte);
    }
    return text + std::format(L" pending {}", character.pending);
}

std::wstring Describe(const Snapshot& now)
{
    std::wstring line = std::format(L"ctrl hud {} blocked {} ready {} guardMode {} req", now.hud ? 1 : 0, now.blocked ? 1 : 0,
                                    now.ready ? 1 : 0, now.guardMode);
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        line += std::format(L" {}:{}", kLoggedRequests[i], now.requests[i]);
    }
    line += L" | " + DescribeCharacter(L"P1", now.characters[0]);
    line += L" | " + DescribeCharacter(L"P2", now.characters[1]);
    line += std::format(L" | targeting {} lock {} latch {} displayed {:#x} P1 target {:#x} P2 target {:#x} leader {:#x} member {:#x}",
                        now.targeting ? 1 : 0, now.lock, now.latch, now.displayed, now.targets[0], now.targets[1], now.leaderWeak,
                        now.memberWeak);
    const auto key = [&now](std::size_t action) {
        return std::format(L"{}{}", now.pressed[action] ? L"P" : L"", now.held[action] ? L"H" : L"-");
    };
    return line + std::format(L" | pad2 attack {} unique {} guard {} dodge {}", key(kAttack), key(kUnique), key(kGuard),
                              key(kDodgeAction));
}

Snapshot g_logged;  // game thread: as last logged
void Stop(const wchar_t* what)
{
    g_failed = true;
    platform::Log(L"battle input: {} raised an exception; player 2's battle actions are off this session", what);
}

bool SoftTargetGuarded(std::uintptr_t system) noexcept
{
    __try
    {
        g_softTarget(reinterpret_cast<void*>(system));
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

std::uint8_t InBattleGuarded() noexcept
{
    __try
    {
        return g_isInBattle();
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return 0;
    }
}

void* LeaderGuarded() noexcept
{
    __try
    {
        return g_resolveLeader();
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return nullptr;
    }
}

std::uintptr_t BattleSystem() noexcept
{
    return Field<std::uintptr_t>(g_battleSystem);
}

// The leader answering player 2's member (player_two/leader_scope.h); player 2's lock byte is in
// place through the member tick's lock_on::TargetScope.
using leader_scope::LeaderScope;

/// Player 2's own command layer: the controller's edge logic on pad 2's view (RE-p2-battle-input.md §1.3, §2).
void Act(void* state, const Parts& parts, std::uint64_t memberWeak, bool attackGate, bool guardGate,
         const std::array<bool, kActions>& pressed, const std::array<bool, kActions>& held)
{
    const std::uintptr_t holder = parts.holder;
    if (Acts(kPartGuard) && guardGate)
    {
        g_guarding = held[kGuard];
        if (!CallGuarded(g_guarding ? g_guardHold : g_guardEnd, reinterpret_cast<void*>(holder)))
        {
            return Stop(L"a guard call");
        }
    }
    if (Acts(kPartAttack) && attackGate)
    {
        // The game's attack correction picks around the leader: with the scope, around player 2's member.
        std::optional<LeaderScope> scope;
        if (Acts(kPartAttackScope))
        {
            scope.emplace(memberWeak);
        }
        for (const std::uint8_t unique : {std::uint8_t{0}, std::uint8_t{1}})
        {
            const Action key = unique != 0 ? kUnique : kAttack;
            bool         ok  = true;
            if (pressed[key] && !g_guarding)
            {
                const auto before     = Field<std::uintptr_t>(parts.owner + kTargetSlot);
                ok                    = CallGuarded(g_attackBegin, state, unique);
                g_holdStarted[unique] = true;
                if (g_lines < kMaxLines)
                {
                    ++g_lines;
                    const bool memberLeads = reinterpret_cast<std::uintptr_t>(LeaderGuarded()) ==
                                             Field<std::uintptr_t>(parts.holder);
                    platform::Log(L"battle input: player 2's attack {} began; target {:#x} -> {:#x}; the game's leader during "
                                  L"the call: {} (leader scope {})",
                                  unique, before, Field<std::uintptr_t>(parts.owner + kTargetSlot),
                                  memberLeads ? L"player 2's member" : L"another character",
                                  scope && scope->Held() ? L"held" : L"off");
                }
            }
            else if (held[key] && g_holdStarted[unique])
            {
                ok = CallGuarded(g_attackHold, state, unique);
            }
            if (ok && !held[key])
            {
                g_holdStarted[unique] = false;
                ok                    = CallGuarded(g_attackRelease, state, unique);  // every frame, as the game does
            }
            if (!ok)
            {
                return Stop(L"an attack call");
            }
        }
    }
    if (Acts(kPartDodge) && guardGate && pressed[kDodgeAction] && !CallGuarded(g_dodge, state))
    {
        Stop(L"the dodge call");
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    g_attackBegin   = reinterpret_cast<StateActionFn>(find(kAttackBegin));
    g_attackHold    = reinterpret_cast<StateActionFn>(find(kAttackHold));
    g_attackRelease = reinterpret_cast<StateActionFn>(find(kAttackRelease));
    g_guardHold     = reinterpret_cast<HolderActionFn>(find(kGuardHold));
    g_guardEnd      = reinterpret_cast<HolderActionFn>(find(kGuardEnd));
    g_dodge         = reinterpret_cast<DodgeFn>(find(kDodge));
    g_isPressed     = reinterpret_cast<QueryFn>(find(kIsPressed));
    g_isHeld        = reinterpret_cast<QueryFn>(find(kIsHeld));
    g_softTarget    = reinterpret_cast<SoftTargetFn>(find(kSoftTarget));
    g_isInBattle    = reinterpret_cast<GlobalTestFn>(find(facts::kIsInBattle));
    g_resolveLeader = reinterpret_cast<GlobalObjectFn>(find(facts::kResolvePartyLeader));
    const auto battleSystem = image.Rebase(kBattleSystem, sizeof(void*));
    const auto partyManager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    bool keys       = true;
    for (std::size_t i = 0; i < kActions; ++i)
    {
        const auto live = image.Rebase(kActionKeys[i], 24);
        g_keys[i]       = live ? *live : 0;
        keys            = keys && live;
    }
    const auto system = image.Rebase(kEndUISystem, kController + sizeof(void*));
    const auto vtable = image.Rebase(kBattlePlayerVTable, sizeof(void*));
    g_installed = g_attackBegin && g_attackHold && g_attackRelease && g_guardHold && g_guardEnd && g_dodge && g_isPressed &&
                  g_isHeld && g_softTarget && leader_scope::Install(image) && g_isInBattle && g_resolveLeader && battleSystem &&
                  partyManager && keys && system && vtable;
    g_battleSystem       = battleSystem ? *battleSystem : 0;
    g_partyManager       = partyManager ? *partyManager : 0;
    g_endUISystem        = system ? *system : 0;
    g_battlePlayerVTable = vtable ? *vtable : 0;
    platform::Log(L"battle input: {} (acting parts mask {})", g_installed ? L"ready" : L"UNAVAILABLE (a check failed)", g_mask);
    return g_installed;
}

void InScopes(void* member, void* state, float deltaTime) noexcept
{
    battle_camera::Watch();          // both camera managers' operators, logged on change
    command_rules::Watch();          // the command controller and the menu's time slow, logged on change
    camera_override::Tick(member);   // the member's animation camera overrides reach player 2's camera
    if (!g_installed || g_failed || member == nullptr || state == nullptr)
    {
        return;
    }
    try
    {
        std::uintptr_t controller = 0;
        Read(g_endUISystem + kController, controller);
        const bool battleHud  = controller != 0 && Field<std::uint8_t>(controller + kBattleHud) != 0;
        const bool blocked    = controller != 0 && Field<std::uint8_t>(controller + kBattleBlocked) != 0;
        const bool ready      = controller != 0 && (Field<std::uint8_t>(controller + kBattleReadyA) != 0 ||
                                               Field<std::uint8_t>(controller + kBattleReadyB) != 0);
        const bool attackGate = battleHud && ready;
        const bool guardGate  = battleHud && !blocked;

        std::array<bool, kActions> pressed{}, held{};
        for (std::size_t i = 0; i < kActions; ++i)
        {
            pressed[i] = QueryGuarded(g_isPressed, g_keys[i]) != 0;
            held[i]    = QueryGuarded(g_isHeld, g_keys[i]) != 0;
        }
        const auto           memberAddress = reinterpret_cast<std::uintptr_t>(member);
        const Parts          two           = PartsOf(memberAddress);
        const std::uintptr_t system        = BattleSystem();
        const std::uint64_t  memberWeak    = leader_scope::WeakOf(member);
        // In battle: the member tick's TargetScope already asked the game (lock_on::Held), else ask.
        const bool           targeting     = system != 0 && Field<std::uint8_t>(system + kSkipTargeting) == 0 &&
                                   (lock_on::Held() || InBattleGuarded() != 0);
        if (two.ready && targeting && Acts(kPartSoftTarget))
        {
            // Player 2's soft target, picked by the game's own leader update for player 2's member.
            const LeaderScope scope(memberWeak);
            if (scope.Held() && !SoftTargetGuarded(system))
            {
                Stop(L"the soft-target update");
            }
        }
        if (two.ready && targeting && !g_failed)
        {
            // Player 2's lock, lock camera and switching, as the game's targeting tick does them next.
            const LeaderScope scope(memberWeak);
            if (scope.Held())
            {
                lock_on::Tick(member, reinterpret_cast<void*>(two.owner), deltaTime,
                              {.lock      = Acts(kPartLock),
                               .switching = Acts(kPartSwitch),
                               .camera    = Acts(kPartLockCamera),
                               .cursor    = Acts(kPartCursor)});
            }
        }
        else if (!targeting)
        {
            lock_on::Tick(member, reinterpret_cast<void*>(two.owner), deltaTime, {});  // ends player 2's lock out of battle
        }
        if (two.ready && g_mask != 0 && !g_failed)
        {
            Act(state, two, memberWeak, attackGate, guardGate, pressed, held);
        }

        if (g_lines >= kMaxLines)
        {
            return;
        }
        // The log: player 1's controller, both characters, pad 2's actions; raw values compared each
        // frame, the text built only when they change.
        Snapshot now;
        now.hud       = battleHud;
        now.blocked   = blocked;
        now.ready     = ready;
        now.guardMode = controller != 0 ? Field<std::int32_t>(controller + kGuardMode) : 0;
        std::uintptr_t requests = 0;
        std::int32_t   count    = 0;
        if (controller != 0)
        {
            Read(controller + kRequests, requests);
            Read(controller + kRequests + 8, count);
        }
        for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
        {
            const std::int32_t index = kLoggedRequests[i];
            now.requests[i] = requests != 0 && index < count ? Field<std::int32_t>(requests + 4 * static_cast<std::uint32_t>(index)) : -1;
        }
        const auto one      = reinterpret_cast<std::uintptr_t>(ReadPointer(generation::PlayerOneController(), facts::kControllerPawn));
        const auto oneState = one != 0 ? reinterpret_cast<std::uintptr_t>(ReadPointer(reinterpret_cast<void*>(one),
                                                                                        facts::kCharacterMoveState))
                                       : 0;
        const Parts oneParts = PartsOf(one);
        now.characters[0]    = CharacterOf(oneState, oneParts);
        now.characters[1]    = CharacterOf(reinterpret_cast<std::uintptr_t>(state), two);
        if (const auto manager = Field<std::uintptr_t>(g_partyManager); manager != 0)
        {
            Read(manager + kPartyManagerLeader, now.leaderWeak);
        }
        now.targeting  = targeting;
        now.lock       = system != 0 ? Field<std::uint8_t>(system + kLockMode) : 0;
        now.latch      = system != 0 ? Field<std::uint8_t>(system + kSwitchLatch) : 0;
        now.displayed  = system != 0 ? Field<std::uintptr_t>(system + kDisplayed) : 0;
        now.targets[0] = oneParts.owner != 0 ? Field<std::uintptr_t>(oneParts.owner + kTargetSlot) : 0;
        now.targets[1] = two.owner != 0 ? Field<std::uintptr_t>(two.owner + kTargetSlot) : 0;
        now.memberWeak = memberWeak;
        now.pressed    = pressed;
        now.held       = held;
        if (now != g_logged)
        {
            g_logged = now;
            ++g_lines;
            platform::Log(L"battle input: {}", Describe(now));
        }
    }    catch (...)
    {
    }
}

void SetMask(unsigned mask) noexcept
{
    g_mask        = mask;
    g_guarding    = false;
    g_holdStarted = {};
    g_lines       = 0;
    platform::Log(L"battle input: mask {} (dodge {}, attack {}, guard {}, soft target {}, attack leader scope {}, lock {}, "
                  L"switching {}, lock camera {}, cursor {})",
                  mask, Acts(kPartDodge), Acts(kPartAttack), Acts(kPartGuard), Acts(kPartSoftTarget), Acts(kPartAttackScope),
                  Acts(kPartLock), Acts(kPartSwitch), Acts(kPartLockCamera), Acts(kPartCursor));
}

}  // namespace lss::player_two::battle_input
