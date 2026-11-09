#include "battle/downed_commands.h"

#include "battle/command_rules.h"
#include "battle/downed_commands_rules.h"
#include "facts/command_controller.h"
#include "facts/hud.h"
#include "platform/call_redirect.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace lss::battle::downed_commands {

namespace {

// Facts read from the database (RE-downed-ally-commands.md §2.1, §5.2; the research build).
// Party_IsTypeDead_AIHolder6(unused, u8 type): the member's control state is Dead (6). The type arrives in dl.
constexpr std::uint8_t          kDeadBytes[] = {0x40, 0x53, 0xB8, 0x80, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kDead{L"Party_IsTypeDead_AIHolder6", 0x140805200, kDeadBytes};
// PartyType_HasStateId(u8 type, u8 status): the gates ask it for Petrify.
constexpr std::uint8_t kHasStatusBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                            0x24, 0x10, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kHasStatus{L"PartyType_HasStateId", 0x140A40050, kHasStatusBytes};
constexpr std::uint8_t           kPetrify = 22;
constexpr std::uint8_t           kNoType  = 0x10;  // command_rules::TypeAt's "none"
// The menu's two gates on its own member: the open (CmdState0_Vt20_NextStateForOpenMode) and the per-tick sweep
// (FieldCommandCtrl_OnLeaderChanged_Battle), each a dead test then a petrify test.
constexpr std::uintptr_t kOpenDeadCall       = 0x140C8933D;
constexpr std::uintptr_t kOpenPetrifiedCall  = 0x140C8934F;
constexpr std::uintptr_t kSweepDeadCall      = 0x1408053E4;
constexpr std::uintptr_t kSweepPetrifiedCall = 0x1408053F6;
// The controller's member record count: the open gate's bound on an ordinal. (A record's own bytes never name its
// member: the type at +0x66 is its queued command's, 0 while none was ever queued.)
constexpr std::uint32_t kCmdRecordCount = 0x18;

using DeadFn      = bool (*)(void* unused, std::uint8_t type);
using HasStatusFn = bool (*)(std::uint8_t type, std::uint8_t status);

// Set at install.
DeadFn                 g_dead      = nullptr;
HasStatusFn            g_hasStatus = nullptr;
std::uintptr_t         g_uiSystem  = 0;
platform::CallRedirect g_openDead, g_openPetrified, g_sweepDead, g_sweepPetrified;
std::atomic<bool>      g_on{false};

// Game thread. Per menu member type: the ordinal an open last answered for while that member was down (-1: none), so
// each player's change of ally is logged once whichever pass runs.
std::array<int, 32> g_openFor = [] {
    std::array<int, 32> none{};
    none.fill(-1);
    return none;
}();

/// A member that cannot act: down or petrified (the gates' two tests together).
bool CannotAct(void* unused, std::uint8_t type) noexcept
{
    return g_dead(unused, type) || g_hasStatus(type, kPetrify);
}

/// The ordinal the gate should ask about instead of the menu's own member, whose type is `type`, and that member's
/// type; -1 to keep the game's question. The running controller is the UI system's (a player's own in that player's
/// menu pass); the gate's type must be its menu member's, or the question is another controller's (or the game's
/// cached type lags a leader change) and the game's own answer stands.
int AskInstead(std::uint8_t type, std::uint8_t& askedType) noexcept
{
    if (!g_on.load(std::memory_order_acquire) || !players::generation::OnGameThread() ||
        !players::generation::AnyActive())
    {
        return -1;
    }
    const auto ctrl = platform::ReadAt<std::uintptr_t>(g_uiSystem + facts::kUICommandController);
    std::int32_t menu = -1, ally = -1, shortcut = -1, count = 0;
    if (ctrl == 0 || !platform::Read(ctrl + facts::kCmdMembers, menu) ||
        !platform::Read(ctrl + facts::kCmdMembers + 4, ally) || !platform::Read(ctrl + facts::kCmdMembers + 8, shortcut) ||
        !platform::Read(ctrl + kCmdRecordCount, count))
    {
        return -1;
    }
    // The game's own list, as the gates and the chooser read it (a record's own bytes do not name its member).
    const auto typeAt = [count](std::int32_t ordinal, std::uint8_t& out) {
        out = ordinal >= 0 && ordinal < count ? command_rules::TypeAt(ordinal) : kNoType;
        return out < kNoType;
    };
    std::uint8_t menuType = 0;
    if (!typeAt(menu, menuType) || menuType != type)
    {
        return -1;  // another controller's question: the game's
    }
    const int current = downed_commands_rules::CurrentMember(menu, ally, shortcut);
    const int asked   = downed_commands_rules::AskAbout(true, menu, current, count, command_rules::Refuses(current));
    return asked >= 0 && typeAt(asked, askedType) ? asked : -1;
}

/// The two dead tests: the game's answer for the menu's own member; when it is down, the member the menu opens for.
template <platform::CallRedirect& Call, bool IsOpen>
bool DeadTest(void* unused, std::uint8_t type) noexcept
{
    const bool down = reinterpret_cast<DeadFn>(Call.Original())(unused, type);
    std::uint8_t askedType = 0;
    const int    asked     = down ? AskInstead(type, askedType) : -1;
    if (IsOpen && type < g_openFor.size() && players::generation::OnGameThread())
    {
        int& last = g_openFor[type];
        if (asked >= 0 && asked != last)
        {
            platform::LogDebug(L"downed commands: the downed member type {}'s menu opens for member type {} (ordinal {})",
                               static_cast<int>(type), static_cast<int>(askedType), asked);
        }
        last = asked;
    }
    return asked >= 0 ? CannotAct(unused, askedType) : down;
}

/// The two petrify tests, the same way.
template <platform::CallRedirect& Call>
bool PetrifyTest(std::uint8_t type, std::uint8_t status) noexcept
{
    const bool held = reinterpret_cast<HasStatusFn>(Call.Original())(type, status);
    if (!held || status != kPetrify)
    {
        return held;
    }
    std::uint8_t askedType = 0;
    return AskInstead(type, askedType) >= 0 ? CannotAct(nullptr, askedType) : held;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto dead      = platform::FindFunction(image, kDead);
    const auto hasStatus = platform::FindFunction(image, kHasStatus);
    const auto system    = image.Rebase(facts::kEndUISystem, facts::kUICommandController + sizeof(void*));
    const auto site      = [&image](std::uintptr_t research) { return image.Rebase(research, 5); };
    const auto openDead = site(kOpenDeadCall), openPetrified = site(kOpenPetrifiedCall);
    const auto sweepDead = site(kSweepDeadCall), sweepPetrified = site(kSweepPetrifiedCall);
    if (!command_rules::Installed())
    {
        // Without the rules' refusals a downed player could be offered another player's character; their member list
        // (command_rules::TypeAt) names the member the gate asks about.
        platform::Log(L"downed commands: UNAVAILABLE (the command rules are not in); a downed player cannot command the "
                      L"AI allies");
        return false;
    }
    if (!dead || !hasStatus || !system || !openDead || !openPetrified || !sweepDead || !sweepPetrified)
    {
        platform::Log(L"downed commands: UNAVAILABLE (a function or call site was not found); a downed player cannot "
                      L"command the AI allies");
        return false;
    }
    g_dead      = reinterpret_cast<DeadFn>(*dead);
    g_hasStatus = reinterpret_cast<HasStatusFn>(*hasStatus);
    g_uiSystem  = *system;
    // At start-up no world exists, so no thread can be at a call (InstallFromOwnThread's rule for a call crossing two
    // blocks; one inside a block is placed as by Install). All four or none: the open and the sweep must agree, or an
    // ally menu that opens would be closed the next tick.
    const bool ok =
        g_openDead.InstallFromOwnThread(*openDead, *dead, reinterpret_cast<std::uintptr_t>(&DeadTest<g_openDead, true>)) &&
        g_openPetrified.InstallFromOwnThread(*openPetrified, *hasStatus,
                                             reinterpret_cast<std::uintptr_t>(&PetrifyTest<g_openPetrified>)) &&
        g_sweepDead.InstallFromOwnThread(*sweepDead, *dead, reinterpret_cast<std::uintptr_t>(&DeadTest<g_sweepDead, false>)) &&
        g_sweepPetrified.InstallFromOwnThread(*sweepPetrified, *hasStatus,
                                              reinterpret_cast<std::uintptr_t>(&PetrifyTest<g_sweepPetrified>));
    if (!ok)
    {
        Remove();  // at start-up still: whatever was placed goes back
        platform::Log(L"downed commands: UNAVAILABLE (a gate's call differs); a downed player cannot command the AI allies");
        return false;
    }
    g_on.store(true, std::memory_order_release);
    platform::Log(L"downed commands: in co-op a downed or petrified player still commands the AI allies (the shoulder "
                  L"buttons, the shortcut palette); their own character's menu stays shut");
    return true;
}

void Remove() noexcept
{
    g_on.store(false, std::memory_order_release);  // from now on every call passes through
    for (platform::CallRedirect* call : {&g_openDead, &g_openPetrified, &g_sweepDead, &g_sweepPetrified})
    {
        // A call crossing a block is put back only from the one thread that can run it, which unloading is not; it
        // passes through once off.
        if (call->IsInstalled() && !call->Crosses() && !call->Restore())
        {
            platform::Log(L"downed commands: a gate's call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::battle::downed_commands
