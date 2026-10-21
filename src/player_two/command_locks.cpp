#include "player_two/command_locks.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/command_menu_two.h"
#include "player_two/game_thread_sites.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace lss::player_two::command_locks {

namespace {

// Facts read from the database (RE-p2-command-menu-levers.md §1, §3, §14; this build).
// The two lock setters: void(u8), each storing one byte on EndUISystem[23]; one call site each in
// BattleSystem_Tick, each call's five bytes inside one aligned 16-byte block.
constexpr std::uint8_t kEnabledBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xB0,
                                          0xD4, 0x6D, 0xFE, 0x48, 0x2B, 0xE0, 0x8A, 0xD9};
constexpr platform::FunctionFact kEnabled{L"EndUI23_SetCommandsEnabled5E2", 0x1439D1A74, kEnabledBytes};
constexpr std::uint8_t kLockedBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x84,
                                         0xD4, 0x6D, 0xFE, 0x48, 0x2B, 0xE0, 0x8A, 0xD9};
constexpr platform::FunctionFact kLocked{L"EndUI23_SetCommandsLocked5E3", 0x1439D1AA0, kLockedBytes};
constexpr std::uintptr_t kEnabledCall = 0x140C91C57;
constexpr std::uintptr_t kLockedCall  = 0x140C91C61;
// The category setter: void(u8 category, u8 enabled). Its first instruction, `mov [rsp+8], rbx`
// (5 bytes, no RIP-relative operand, no branch lands inside it), is the one the entry redirect replaces.
constexpr std::uint8_t kCategoryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00,
                                           0xE8, 0x18, 0xE1, 0x6D, 0xFE, 0x48, 0x2B, 0xE0, 0x40, 0x8A, 0xFA};
constexpr platform::FunctionFact kCategory{L"EndUI23_SetCommandCategoryEnabled", 0x1439D0E08, kCategoryBytes};
// The event on/off table's dispatcher: its first instruction, `mov [rsp+10h], rbx` (5 bytes, the same
// conditions), is replaced.
constexpr std::uint8_t kDispatcherBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                             0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8,
                                             0xF0, 0x03, 0x00, 0x00, 0xE8, 0x8B, 0xBB, 0x07, 0x01};
constexpr platform::FunctionFact kDispatcher{L"EventCmd_Dispatch_BattleModeCase87", 0x141033388, kDispatcherBytes};
constexpr std::size_t kFirstInstruction = 5;

constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kController  = 0xB8;  // EndUISystem[23]
// The controller's values (RE-p2-command-menu-views.md §10.2; levers §3.2).
constexpr std::uint32_t kCommandsEnabled = 0x5E2;  // u8
constexpr std::uint32_t kCommandsLocked  = 0x5E3;  // u8
constexpr std::uint32_t kCategoryLocks   = 0x2D8;  // u8 array {data, count +0x8}
constexpr std::uint32_t kCategoryCount   = 0x2E0;  // i32
constexpr std::int32_t  kMaxCategories   = 32;
// u8 each, stored on EndUISystem[23] by the dispatcher's cmds 0x2B (+0x77C = !on, EndUI23_Set77C), 0x3E, 0x3F
// and 0x43 (levers §3.1, §14.4).
constexpr std::array<std::uint32_t, 4> kTableBytes{0x77C, 0x77F, 0x780, 0x782};

using LockFn     = void (*)(std::uint8_t value);
using CategoryFn = void (*)(std::uint8_t category, std::uint8_t enabled);
using DispatchFn = void (*)(void* fieldSystem, std::int32_t mode);  // FieldSys (AEndGameMode+0x170), mode 0..3

// Set once at install.
platform::CallRedirect  g_enabledCall, g_lockedCall;
platform::EntryRedirect g_category, g_dispatcher;
std::uintptr_t          g_endUISystem = 0;
bool                    g_installed   = false;

/// What Install found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t                enabled = 0, locked = 0, category = 0, dispatcher = 0, enabledAt = 0, lockedAt = 0;
    std::span<const std::uint8_t> categoryFirst, dispatcherFirst;
};
Sites g_sites;
std::atomic<bool> g_fromGameThread {false};  // 1.005: the calls go in with InstallFromOwnThread
std::atomic<bool> g_placePending   {false};  // 1.005: queued, not yet placed (Remove clears it)

// Game thread.

/// A line of `level`.
void Line(const std::wstring& text, platform::Level level = platform::Level::Info) noexcept
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"command locks: {}", text);
    }
}

/// Debug for a follow that worked, Info for one that faulted or failed.
platform::Level FollowLevel(bool ok) noexcept
{
    return ok ? platform::Level::Debug : platform::Level::Info;
}

/// Player 2's controller if a game call may be followed on it now: it exists, we are on the game thread,
/// and the UI system's slot names the game's controller (not player 2's: no player-2 scope is open). 0
/// otherwise (logged when that is unexpected).
std::uintptr_t FollowingController(const wchar_t* what) noexcept
{
    const auto two = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    if (!g_installed || two == 0)
    {
        return 0;  // not built yet: Sync() copies at its join
    }
    if (!generation::OnGameThread())
    {
        Line(std::format(L"{} made off the game thread: NOT made on player 2's controller", what));
        return 0;
    }
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kController);
    if (one == 0 || one == two)
    {
        Line(std::format(L"{} made while the UI system's slot names {}: NOT followed", what,
                         one == 0 ? L"no controller" : L"player 2's controller (inside a player-2 scope)"));
        return 0;
    }
    return two;
}

/// Runs `call` (a guarded game call) with the UI system's controller slot naming `two`, and puts the
/// game's controller back.
template <class Call>
bool WithSlot(std::uintptr_t two, Call&& call) noexcept
{
    const auto slot = g_endUISystem + kController;
    const auto one  = ReadAt<std::uintptr_t>(slot);
    if (!WriteAt(slot, two))
    {
        return false;
    }
    const bool ran = call();
    if (!WriteAt(slot, one))
    {
        Line(L"FAILED to put the game's controller back in the UI system's slot");
    }
    return ran;
}

/// For BattleSystem_Tick's call of a lock setter: the game's controller, then player 2's.
void Follow(const platform::CallRedirect& redirect, const wchar_t* what, std::uint32_t offset, std::uint8_t value) noexcept
{
    const auto original = reinterpret_cast<LockFn>(redirect.Original());
    original(value);
    const auto two = FollowingController(what);
    if (two == 0)
    {
        return;
    }
    const bool ran = WithSlot(two, [&] { return platform::CallGuarded(original, value); });
    Line(std::format(L"{} {} -> player 2's controller too{} (now {})", what, value, ran ? L"" : L" (FAULTED)",
                     ReadAt<std::uint8_t>(two + offset)),
         FollowLevel(ran));
}

void CommandsEnabled(std::uint8_t value) noexcept
{
    Follow(g_enabledCall, L"commands enabled", kCommandsEnabled, value);
}

void CommandsLocked(std::uint8_t value) noexcept
{
    Follow(g_lockedCall, L"commands locked", kCommandsLocked, value);
}

/// Replaces the category setter's entry: the game's call, then the same call on player 2's controller
/// (its global table store repeats the same value).
void CategoryEnabled(std::uint8_t category, std::uint8_t enabled) noexcept
{
    const auto original = reinterpret_cast<CategoryFn>(g_category.Original());
    original(category, enabled);
    const auto two = FollowingController(L"command category lock");
    if (two == 0)
    {
        return;
    }
    const bool ran = WithSlot(two, [&] { return platform::CallGuarded(original, category, enabled); });
    Line(std::format(L"command category {} enabled {} -> player 2's controller too{}", category, enabled,
                     ran ? L"" : L" (FAULTED)"),
         FollowLevel(ran));
}

/// Replaces the on/off table dispatcher's entry: it runs once (it consumes its queue and drives music,
/// sounds and cameras); the bytes it changed on the game's controller are then stored on player 2's. Its
/// category cmds reach CategoryEnabled nested inside it.
void Dispatch(void* fieldSystem, std::int32_t mode) noexcept
{
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kController);
    std::array<std::uint8_t, kTableBytes.size()> before{};
    for (std::size_t i = 0; i < kTableBytes.size(); ++i)
    {
        before[i] = ReadAt<std::uint8_t>(one + kTableBytes[i]);
    }
    reinterpret_cast<DispatchFn>(g_dispatcher.Original())(fieldSystem, mode);
    if (one == 0 || one != ReadAt<std::uintptr_t>(g_endUISystem + kController))
    {
        return;
    }
    std::uintptr_t two = 0;
    for (std::size_t i = 0; i < kTableBytes.size(); ++i)
    {
        const auto after = ReadAt<std::uint8_t>(one + kTableBytes[i]);
        if (after == before[i] || (two == 0 && (two = FollowingController(L"on/off table")) == 0))
        {
            continue;
        }
        const bool stored = WriteAt(two + kTableBytes[i], after);
        Line(std::format(L"on/off table: +{:#x} {} -> {} on the game's controller; player 2's {}", kTableBytes[i],
                         before[i], after, stored ? L"too" : L"NOT (write failed)"),
             FollowLevel(stored));
    }
}

}  // namespace

namespace {

/// The redirects, in order, from what Install found. At install on the research build; on 1.005 from the game
/// thread, where the locked call's five bytes cross a 16-byte block (player_two/game_thread_sites.h).
bool PlaceSites() noexcept
{
    const Sites& s     = g_sites;
    const auto   place = [](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        const auto replacement = reinterpret_cast<std::uintptr_t>(with);
        return g_fromGameThread ? call.InstallFromOwnThread(site, target, replacement) : call.Install(site, target, replacement);
    };
    g_installed = g_installed && place(g_enabledCall, s.enabledAt, s.enabled, &CommandsEnabled) &&
                  place(g_lockedCall, s.lockedAt, s.locked, &CommandsLocked) &&
                  g_category.Install(s.category, s.categoryFirst, reinterpret_cast<std::uintptr_t>(&CategoryEnabled)) &&
                  g_dispatcher.Install(s.dispatcher, s.dispatcherFirst, reinterpret_cast<std::uintptr_t>(&Dispatch));
    if (!g_installed)
    {
        Remove();
    }
    platform::Log(L"command locks: {}",
                  g_installed ? L"the battle's command locks, category locks and on/off-table bytes reach player 2's "
                                L"controller through the game's setters (logged)"
                              : L"UNAVAILABLE (a check failed): player 2's command menu keeps the values of its last join");
    return g_installed;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto enabled = platform::FindFunction(image, kEnabled), locked = platform::FindFunction(image, kLocked),
               category = platform::FindFunction(image, kCategory), dispatcher = platform::FindFunction(image, kDispatcher);
    const auto enabledAt = image.Rebase(kEnabledCall, 5), lockedAt = image.Rebase(kLockedCall, 5);
    const auto system    = image.Rebase(kEndUISystem, kController + sizeof(void*));
    g_endUISystem        = system ? *system : 0;
    g_installed          = enabled && locked && category && dispatcher && enabledAt && lockedAt && system;
    const auto first     = [](const std::uint8_t* bytes) { return std::span<const std::uint8_t>(bytes, kFirstInstruction); };
    if (g_installed)
    {
        g_sites = {*enabled,
                   *locked,
                   *category,
                   *dispatcher,
                   *enabledAt,
                   *lockedAt,
                   image.EntryInstruction(kCategory.address, first(kCategoryBytes)),
                   image.EntryInstruction(kDispatcher.address, first(kDispatcherBytes))};
    }
    if (g_installed && platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        g_fromGameThread = true;
        g_installed      = false;  // set again by the placement
        g_placePending   = game_thread_sites::Queue([]() noexcept {
            if (g_placePending)
            {
                g_placePending = false;
                g_installed    = true;
                PlaceSites();
            }
        });
        platform::Log(L"command locks: {}", g_placePending ? L"its calls are placed from the game thread (1.005: one crosses a 16-byte block)"
                                                           : L"UNAVAILABLE (no room to place its calls from the game thread)");
        return g_placePending;
    }
    return PlaceSites();
}

void Remove() noexcept
{
    g_placePending = false;  // a placement still queued does nothing
    g_installed    = false;
    const auto restore = [](auto& redirect) {
        if (redirect.IsInstalled() && !redirect.Restore())
        {
            platform::Log(L"command locks: a redirect was changed after us; left in place (it passes through)");
        }
    };
    restore(g_dispatcher);
    restore(g_category);
    restore(g_lockedCall);
    restore(g_enabledCall);
}

void Sync(void* playerTwoController) noexcept
{
    const auto two = reinterpret_cast<std::uintptr_t>(playerTwoController);
    const auto one = ReadAt<std::uintptr_t>(g_endUISystem + kController);
    if (two == 0 || one == 0 || one == two)
    {
        return;
    }
    std::wstring text;
    for (const std::uint32_t offset :
         {kCommandsEnabled, kCommandsLocked, kTableBytes[0], kTableBytes[1], kTableBytes[2], kTableBytes[3]})
    {
        const auto value = ReadAt<std::uint8_t>(one + offset);
        text += std::format(L" +{:#x} {}->{}", offset, ReadAt<std::uint8_t>(two + offset), value);
        WriteAt(two + offset, value);
    }
    const auto oneLocks = ReadAt<std::uintptr_t>(one + kCategoryLocks);
    const auto twoLocks = ReadAt<std::uintptr_t>(two + kCategoryLocks);
    const auto count    = std::min(
        {ReadAt<std::int32_t>(one + kCategoryCount), ReadAt<std::int32_t>(two + kCategoryCount), kMaxCategories});
    std::array<std::uint8_t, kMaxCategories> locks{};
    if (oneLocks != 0 && twoLocks != 0 && count > 0 &&
        platform::CopyGuarded(locks.data(), reinterpret_cast<const void*>(oneLocks), static_cast<std::size_t>(count)) &&
        platform::WriteGuarded(reinterpret_cast<void*>(twoLocks), locks.data(), static_cast<std::size_t>(count)))
    {
        text += L" | categories";
        for (std::int32_t i = 0; i < count; ++i)
        {
            text += std::format(L" {}", locks[static_cast<std::size_t>(i)]);
        }
    }
    else
    {
        text += std::format(L" | categories NOT copied (count {})", count);
    }
    Line(L"join sync (the pushes made before player 2's controller followed):" + text);
}

}  // namespace lss::player_two::command_locks
