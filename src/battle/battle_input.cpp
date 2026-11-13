#include "battle/battle_input.h"

#include "battle/lock_on.h"
#include "facts/command_controller.h"
#include "facts/controller.h"
#include "facts/hud.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settled_log.h"
#include "platform/verified_code.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/leader_scope.h"

#include <array>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::battle::battle_input {

namespace {

// The battle target system (research/RE-p2-targeting.md; a pointer global): the lock byte, the switch
// latch, the skip byte, the displayed target. The soft target itself lives in each character's
// BattlePlayer component (+0x258 object, +0x260 its reference controller).
using facts::kBattleStateGlobal;
constexpr std::uint32_t  kLockMode       = 0x8C;
constexpr std::uint32_t  kSwitchLatch    = 0x8D;
constexpr std::uint32_t  kSkipTargeting  = 0x96;
constexpr std::uint32_t  kDisplayed      = 0x70;
constexpr std::uint32_t  kTargetSlot     = 0x258;
// The party manager's leader, an FWeakObjectPtr {index, serial} read by ResolvePartyLeaderWeakPtr.
using facts::kPartyManagerLeader;

// The command controller (read only, for the log): its battle-phase gates, guard mode and request array.
using facts::kCmdHudMode;
using facts::kCmdBlocked;
using facts::kCmdReady;
using facts::kCmdReadyB;
using facts::kCmdGuardLatch;
using facts::kCmdRequests;
constexpr std::array<std::int32_t, 10> kLoggedRequests{5, 6, 12, 13, 25, 26, 32, 37, 38, 39};

// A party character's battle parts: the holder whose +0 is the character. In 1.005 both the vt+0xA18 getter and
// CreateMoveState read it at [rcx+1778h].
constexpr platform::PerBuild<std::uint32_t> kCharacterHolder = {0x1758, 0x1778};
using facts::kCharacterControlHolder;
constexpr std::uint32_t  kOwnerSlot         = 0x48;
using facts::kBattlePlayerComponentVTable;
constexpr std::uint32_t  kCommandHandle     = 0x418;  // the component's handle; null: the actions do nothing
constexpr std::uint32_t  kHandleMode        = 0x112;  // u8 in the handle's object
constexpr std::array<std::uint32_t, 4> kStateBytes{0x49, 0x4A, 0x64, 0x65};
constexpr std::uint32_t  kPendingAction     = 0x84;   // int32


using OwnerFn        = void* (*)(void* aiHolder);
using GlobalTestFn   = std::uint8_t (*)();

// Set once at install.
bool           g_installed = false;
std::uintptr_t g_endUISystem   = 0;
std::uintptr_t g_battlePlayerVTable = 0;
std::uintptr_t g_battleSystem  = 0;  // the address holding the battle system
std::uintptr_t g_partyManager  = 0;  // the address holding the party manager

// Game thread.

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
        !Read(parts.holder, first) || first != character || !Read(character + kCharacterControlHolder, aiHolder) ||
        aiHolder == 0)
    {
        return parts;
    }
    parts.owner = reinterpret_cast<std::uintptr_t>(OwnerGuarded(aiHolder));
    if (parts.owner == 0 || ReadAt<std::uintptr_t>(parts.owner) != g_battlePlayerVTable)
    {
        return parts;
    }
    parts.handle = ReadAt<std::uintptr_t>(parts.owner + kCommandHandle);
    parts.mode   = parts.handle != 0 ? ReadAt<std::uint8_t>(parts.handle + kHandleMode) : 0;
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
        character.bytes[i] = ReadAt<std::uint8_t>(state + kStateBytes[i]);
    }
    character.pending = state != 0 ? ReadAt<std::int32_t>(state + kPendingAction) : 0;
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

std::wstring Describe(int seat, const Snapshot& now)
{
    const wchar_t* who = players::SeatName(seat);
    std::wstring line = std::format(L"ctrl hud {} blocked {} ready {} guardMode {} req", now.hud ? 1 : 0, now.blocked ? 1 : 0,
                                    now.ready ? 1 : 0, now.guardMode);
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        line += std::format(L" {}:{}", kLoggedRequests[i], now.requests[i]);
    }
    line += L" | " + DescribeCharacter(L"player 1", now.characters[0]);
    line += L" | " + DescribeCharacter(who, now.characters[1]);
    return line + std::format(L" | targeting {} lock {} latch {} displayed {:#x} player 1 target {:#x} {} target {:#x} leader "
                              L"{:#x} member {:#x}",
                              now.targeting ? 1 : 0, now.lock, now.latch, now.displayed, now.targets[0], who, now.targets[1],
                              now.leaderWeak, now.memberWeak);
}

constexpr std::uint32_t kStillFrames = 30;
using Requests = std::array<std::int32_t, kLoggedRequests.size()>;
using SeatLog = platform::SettledLog<Snapshot, Requests>;
// Game thread, by seat.
SeatLog g_log[players::kMaxPlayers] = {SeatLog(kStillFrames), SeatLog(kStillFrames), SeatLog(kStillFrames)};
static_assert(players::kMaxPlayers == 3, "g_log's initialiser names every player");

std::uintptr_t BattleSystem() noexcept
{
    return ReadAt<std::uintptr_t>(g_battleSystem);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    const auto battleSystem = image.Rebase(kBattleStateGlobal, sizeof(void*));
    const auto partyManager = players::game_globals::Address(players::game_globals::Global::PartyManager);
    const auto system = players::game_globals::Address(players::game_globals::Global::UiSystem);
    const auto vtable = image.Rebase(kBattlePlayerComponentVTable, sizeof(void*));
    g_installed = scopes::leader_scope::Installed() && facts::InBattleKnown() && battleSystem && partyManager && system &&
                  vtable;
    g_battleSystem       = battleSystem ? *battleSystem : 0;
    g_partyManager       = partyManager;
    g_endUISystem        = system;
    g_battlePlayerVTable = vtable ? *vtable : 0;
    platform::Log(L"battle input: {}", g_installed ? L"ready (logs player 2's and 3's battle states)" : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Watch(int seat, void* member, void* state) noexcept
{
    if (!g_installed || !players::SeatIndex(seat) || member == nullptr || state == nullptr)
    {
        return;
    }
    SeatLog& log = g_log[seat];
    try
    {
        std::uintptr_t controller = 0;
        Read(g_endUISystem + facts::kUICommandController, controller);
        const bool battleHud  = controller != 0 && ReadAt<std::uint8_t>(controller + kCmdHudMode) != 0;
        const bool blocked    = controller != 0 && ReadAt<std::uint8_t>(controller + kCmdBlocked) != 0;
        const bool ready      = controller != 0 && (ReadAt<std::uint8_t>(controller + kCmdReady) != 0 ||
                                               ReadAt<std::uint8_t>(controller + kCmdReadyB) != 0);
        const auto           memberAddress = reinterpret_cast<std::uintptr_t>(member);
        const Parts          parts         = PartsOf(memberAddress);
        const std::uintptr_t system        = BattleSystem();
        const std::uint64_t  memberWeak    = scopes::leader_scope::WeakOf(member);
        // In battle: the member tick's TargetScope already asked the game (lock_on::Held), else ask.
        const bool           targeting     = system != 0 && ReadAt<std::uint8_t>(system + kSkipTargeting) == 0 &&
                                   (lock_on::Held() || facts::InBattle() != 0);
        // The seat's soft target, lock, lock camera, switching and cursor: the game's targeting tick run
        // for the seat (battle/lock_on.h). Attack, unique, guard and dodge: the seat's command
        // controller (battle/command_menu_tick.h).

        if (!platform::Enabled(platform::Level::Trace))
        {
            log.Reset();  // off: the next line starts fresh
            return;
        }
        // The log: player 1's controller, player 1's and the seat's characters, the targets; raw values compared each
        // frame, the text built only when they change.
        Snapshot now;
        now.hud       = battleHud;
        now.blocked   = blocked;
        now.ready     = ready;
        now.guardMode = controller != 0 ? ReadAt<std::int32_t>(controller + kCmdGuardLatch) : 0;
        std::uintptr_t requests = 0;
        std::int32_t   count    = 0;
        if (controller != 0)
        {
            Read(controller + kCmdRequests, requests);
            Read(controller + kCmdRequests + 8, count);
        }
        for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
        {
            const std::int32_t index = kLoggedRequests[i];
            now.requests[i] = requests != 0 && index < count
                                  ? ReadAt<std::int32_t>(requests + 4 * static_cast<std::uint32_t>(index))
                                  : -1;
        }
        const auto one      = reinterpret_cast<std::uintptr_t>(ReadPointer(players::generation::PlayerOneController(), facts::kControllerPawn));
        const auto oneState = one != 0 ? reinterpret_cast<std::uintptr_t>(ReadPointer(reinterpret_cast<void*>(one),
                                                                                        facts::kCharacterMoveState))
                                       : 0;
        const Parts oneParts = PartsOf(one);
        now.characters[0]    = CharacterOf(oneState, oneParts);
        now.characters[1]    = CharacterOf(reinterpret_cast<std::uintptr_t>(state), parts);
        if (const auto manager = ReadAt<std::uintptr_t>(g_partyManager); manager != 0)
        {
            Read(manager + kPartyManagerLeader, now.leaderWeak);
        }
        now.targeting  = targeting;
        now.lock       = system != 0 ? ReadAt<std::uint8_t>(system + kLockMode) : 0;
        now.latch      = system != 0 ? ReadAt<std::uint8_t>(system + kSwitchLatch) : 0;
        now.displayed  = system != 0 ? ReadAt<std::uintptr_t>(system + kDisplayed) : 0;
        now.targets[0] = oneParts.owner != 0 ? ReadAt<std::uintptr_t>(oneParts.owner + kTargetSlot) : 0;
        now.targets[1] = parts.owner != 0 ? ReadAt<std::uintptr_t>(parts.owner + kTargetSlot) : 0;
        now.memberWeak = memberWeak;
        Snapshot steady = now;
        steady.requests = {};  // the action requests flip with each press: logged once they hold
        if (log.Offer(steady, now.requests))
        {
            platform::LogTrace(L"battle input: {}", Describe(seat, now));
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::battle::battle_input
