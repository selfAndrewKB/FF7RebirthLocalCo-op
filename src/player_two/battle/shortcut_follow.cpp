#include "player_two/battle/shortcut_follow.h"

#include <intrin.h>

#include "facts/hud.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/battle/command_menu_two_shared.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace lss::player_two::shortcut_follow {

namespace {

// The shortcut tables (RE-p2-shortcut-refresh.md): each controller keeps its own copy of the shortcut assignments
// (both sets, a row per battle ordinal, built from the save by MemberSelecter_RebuildShortcutTables on ctrl+0xF0);
// everything that shows or fires a shortcut reads the copy. The menus rebuild it right after each change through
// EndUI23_RebuildShortcutTables, which rebuilds EndUISystem[23]'s only (FieldCommandCtrl_RebuildShortcutTables:
// the rebuild, then +0x280 = -1, a repaint on the controller's next tick). Its entry, `mov eax, 28h` (5 bytes, inside
// one block on both builds), is redirected and the game's rebuild made again on the other controller. 1.004 also
// inlines it once (the status menu's member lambda, a call of the table builder); 1.005 calls the wrapper there.
// Item use rebuilds the executing controller's only (FieldCommandCtrl_ExecuteCommandRecord); its call is followed
// both ways (the inventory is shared). Each call inside one aligned 16-byte block.
constexpr std::uint8_t kRebuildShortcutsBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x86, 0xEC, 0x6D, 0xFE, 0x48, 0x2B, 0xE0,
                                                   0xE8, 0x8E, 0x66, 0xE3, 0xFC, 0x48, 0x8B, 0x88, 0xB8, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kRebuildShortcuts{L"EndUI23_RebuildShortcutTables", 0x1439D02A0, kRebuildShortcutsBytes};
constexpr std::size_t            kRebuildShortcutsFirst = 5;
constexpr std::uint8_t kCtrlShortcutsBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x60, 0xB1, 0xB8, 0x00, 0x48,
                                                0x2B, 0xE0, 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1, 0xF0, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kCtrlShortcuts{L"FieldCommandCtrl_RebuildShortcutTables", 0x141523DC4, kCtrlShortcutsBytes};
constexpr std::uint8_t kSelecterShortcutsBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                    0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x30, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kSelecterShortcuts{L"MemberSelecter_RebuildShortcutTables", 0x140D202B4,
                                                    kSelecterShortcutsBytes};
constexpr std::uintptr_t kStatusShortcutsCall = 0x1422FA6D9;  // 1.004 only: call of the table builder (inlined wrapper)
constexpr std::uintptr_t kItemShortcutsCall   = 0x141524C69;  // in ExecuteCommandRecord: call of the controller's rebuild
constexpr std::uint32_t  kSelecter            = 0xF0;         // the member selecter, embedded
constexpr std::uint32_t  kShortcutSets        = 0x208;        // selecter +0x118: two TArray<TArray<88-byte record>>
constexpr std::uint32_t  kShortcutTarget      = 0x3C;         // the ordinal whose shortcuts show
constexpr std::uint32_t  kShortcutShown       = 0x280;        // the last target shown (-1: repaint)
constexpr std::size_t    kRecordSize          = 88;
constexpr std::uint32_t  kRecordCommand       = 56;
constexpr int            kMaxShortcutLines    = 60;
using facts::kUICommandController;

using ShortcutsFn    = void (*)(void* controller);  // FieldCommandCtrl_RebuildShortcutTables
using SelecterShortcutsFn = void (*)(void* selecter);  // MemberSelecter_RebuildShortcutTables

// Set once at install.
platform::EntryRedirect g_rebuildShortcuts;
platform::CallRedirect  g_statusShortcutsCall, g_itemShortcutsCall;
ShortcutsFn             g_ctrlShortcuts     = nullptr;  // the game's rebuild of one controller's tables
SelecterShortcutsFn     g_selecterShortcuts = nullptr;  // 1.004: the status-menu call's target
std::uintptr_t          g_lastGame          = 0;        // the game's controller, as last read outside player 2's scope
int                     g_shortcutLines     = 0;
std::optional<platform::GameImage> g_image;  // for the logged return addresses
/// What Find found, for the redirects (placed at once, or on 1.005 from the game thread).
struct Sites
{
    std::uintptr_t rebuildShortcuts = 0, statusAt = 0, itemAt = 0;
};
Sites g_sites;

/// A line under the command menu's name: the follow is its.
void Line(const std::wstring& text) noexcept
{
    platform::Log(L"command menu two: {}", text);
}

/// The game's controller: the UI system's slot, or inside player 2's scope (the slot names player 2's) the one last
/// read outside it.
std::uintptr_t GameController() noexcept
{
    const auto slot = ReadAt<std::uintptr_t>(command_menu_two::UiSystem() + kUICommandController);
    if (slot != 0 && slot != reinterpret_cast<std::uintptr_t>(command_menu_two::Controller()))
    {
        g_lastGame = slot;
    }
    return g_lastGame;
}

/// The command ids (record +56) of the six assignable shortcuts in `row` of a controller's set, as text.
std::wstring ShortcutIds(std::uintptr_t controller, int set, std::int32_t row) noexcept
{
    const auto table = controller + kShortcutSets + 16 * set;
    const auto rows  = ReadAt<std::uintptr_t>(table);
    if (rows == 0 || row < 0 || row >= ReadAt<std::int32_t>(table + 8))
    {
        return L"[none]";
    }
    const auto records = ReadAt<std::uintptr_t>(rows + 16 * row);
    const auto count   = ReadAt<std::int32_t>(rows + 16 * row + 8);
    std::wstring text  = L"[";
    for (std::int32_t r = 0; r < 6 && r < count && records != 0; ++r)
    {
        text += std::format(L"{}{}", r != 0 ? L" " : L"",
                            ReadAt<std::uint32_t>(records + kRecordSize * r + kRecordCommand));
    }
    return text + L"]";
}

/// The game's rebuild of `target`'s shortcut tables, after the game rebuilt `source`'s (`why`, called from
/// `caller`). The rebuild reads the save and no leader or UI slot, so it needs no scope. Logged: both controllers'
/// ids in the shown row, which must match afterwards.
void FollowShortcuts(std::uintptr_t source, std::uintptr_t target, const wchar_t* why, std::uintptr_t caller) noexcept
{
    if (g_ctrlShortcuts == nullptr || target == 0 || target == source)
    {
        return;
    }
    const bool log   = g_shortcutLines < kMaxShortcutLines && platform::Enabled(platform::Level::Debug);
    const auto whose = target == reinterpret_cast<std::uintptr_t>(command_menu_two::Controller()) ? L"player 2"
                                                                                                   : L"player 1";
    const auto row   = ReadAt<std::int32_t>(target + kShortcutTarget);
    const auto ids   = [row](std::uintptr_t c, int set) { return ShortcutIds(c, set, row); };
    const auto before = log ? ids(target, 0) + L" " + ids(target, 1) : std::wstring{};
    if (!platform::CallGuarded(g_ctrlShortcuts, reinterpret_cast<void*>(target)))
    {
        Line(std::format(L"FAILED: rebuilding the shortcut tables of {}'s controller faulted ({})", whose, why));
        return;
    }
    if (log)
    {
        ++g_shortcutLines;
        const auto source0 = ids(source, 0), source1 = ids(source, 1), after0 = ids(target, 0), after1 = ids(target, 1);
        platform::LogDebug(L"command menu two: {}'s shortcut tables rebuilt too ({}, from 0x{:X}): row {}: the rebuilt "
                           L"one {} {} (was {}), the other {} {} (match: {}); +0x280={}",
                           whose, why, g_image ? g_image->ResearchAddress(caller) : caller, row, after0, after1,
                           before, source0,
                           source1, YesNo(after0 == source0 && after1 == source1),
                           ReadAt<std::int32_t>(target + kShortcutShown));
    }
}

/// Replaces EndUI23_RebuildShortcutTables: the game rebuilds its slot's controller, then the other controller is
/// rebuilt too. Outside player 2's scope that is player 2's; inside it (an event in player 2's pass) the game's.
void RebuildShortcutsCall() noexcept
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto ran    = ReadAt<std::uintptr_t>(command_menu_two::UiSystem() + kUICommandController);
    reinterpret_cast<void (*)()>(g_rebuildShortcuts.Original())();
    const auto two = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    if (command_menu_two::CanFollow(reinterpret_cast<void*>(ran), false))
    {
        FollowShortcuts(ran, two, L"the game rebuilt its shortcut tables", caller);
    }
    else if (ran != 0 && ran == two && command_menu_two::Ready() && generation::OnGameThread())
    {
        FollowShortcuts(two, GameController(), L"the game's shortcut rebuild ran in player 2's scope", caller);
    }
}

/// 1.004 only: replaces the status menu's call of the table builder (an inlined EndUI23_RebuildShortcutTables on the
/// game's controller), then rebuilds player 2's.
void StatusShortcutsCall(void* selecter) noexcept
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_selecterShortcuts(selecter);
    const auto controller = reinterpret_cast<std::uintptr_t>(selecter) - kSelecter;
    if (command_menu_two::CanFollow(reinterpret_cast<void*>(controller), false))
    {
        FollowShortcuts(controller, reinterpret_cast<std::uintptr_t>(command_menu_two::Controller()),
                        L"the status menu rebuilt the game's shortcut tables", caller);
    }
}

/// Replaces the item use's rebuild of the executing controller's tables, then rebuilds the other's: the inventory is
/// shared, so either player's item changes both players' shortcuts.
void ItemShortcutsCall(void* controller) noexcept
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_ctrlShortcuts(controller);
    const auto ran = reinterpret_cast<std::uintptr_t>(controller);
    const auto two = reinterpret_cast<std::uintptr_t>(command_menu_two::Controller());
    if (command_menu_two::CanFollow(controller, false))
    {
        FollowShortcuts(ran, two, L"an item use rebuilt the game's", caller);
    }
    else if (ran != 0 && ran == two && command_menu_two::Ready() && generation::OnGameThread())
    {
        FollowShortcuts(two, GameController(), L"player 2's item use rebuilt player 2's", caller);
    }
}

/// One call site redirected: at install on the research build, from the game thread on 1.005.
template <class Fn>
bool Redirect(platform::DeferredSites& placement, platform::CallRedirect& call, std::uintptr_t site,
              std::uintptr_t target, Fn* with) noexcept
{
    return placement.Place(call, site, target, reinterpret_cast<std::uintptr_t>(with));
}

}  // namespace

void Find(const platform::GameImage& image) noexcept
{
    g_image.emplace(image);
    const auto site = [&image](std::uintptr_t address) { return image.Rebase(address, 5); };
    // The shortcut follows: the wrapper and the controller's rebuild on both builds; the status menu's inlined
    // rebuild on the research build only.
    const auto wrapper = platform::FindFunction(image, kRebuildShortcuts);
    const auto rebuild = platform::FindFunction(image, kCtrlShortcuts);
    g_ctrlShortcuts    = rebuild ? reinterpret_cast<ShortcutsFn>(*rebuild) : nullptr;
    g_sites.rebuildShortcuts = wrapper ? *wrapper : 0;
    g_sites.itemAt           = site(kItemShortcutsCall).value_or(0);
    if (platform::RunningBuild() != platform::BuildIndex::V1005)
    {
        const auto builder  = platform::FindFunction(image, kSelecterShortcuts);
        g_selecterShortcuts = builder ? reinterpret_cast<SelecterShortcutsFn>(*builder) : nullptr;
        g_sites.statusAt    = site(kStatusShortcutsCall).value_or(0);
    }
}

/// The shortcut follows, from what Install found; each optional (a failed one leaves only that source unfollowed).
void Place(const platform::GameImage& image, platform::DeferredSites& placement) noexcept
{
    const Sites& s = g_sites;
    if (g_ctrlShortcuts == nullptr || s.rebuildShortcuts == 0)
    {
        platform::Log(L"command menu two: player 2's shortcut tables do NOT follow the game's rebuilds (a check failed); "
                      L"they change at the next Members panel rebuild");
        return;
    }
    const bool menus = g_rebuildShortcuts.Install(
        s.rebuildShortcuts,
        image.EntryInstruction(kRebuildShortcuts.address, std::span<const std::uint8_t>(kRebuildShortcutsBytes, kRebuildShortcutsFirst)),
        reinterpret_cast<std::uintptr_t>(&RebuildShortcutsCall));
    const bool status = s.statusAt != 0 && g_selecterShortcuts != nullptr &&
                        Redirect(placement, g_statusShortcutsCall, s.statusAt,
                                 reinterpret_cast<std::uintptr_t>(g_selecterShortcuts),
                              &StatusShortcutsCall);
    const bool items = s.itemAt != 0 &&
                       Redirect(placement, g_itemShortcutsCall, s.itemAt,
                                reinterpret_cast<std::uintptr_t>(g_ctrlShortcuts), &ItemShortcutsCall);
    platform::Log(L"command menu two: the shortcut tables follow the game's rebuilds: menus and character events {}, the "
                  L"status menu's inlined one {}, item use {} (logged at Debug)",
                  menus ? L"yes" : L"NO", platform::RunningBuild() == platform::BuildIndex::V1005 ? L"(1.005 calls the wrapper)"
                                                                                                 : (status ? L"yes" : L"NO"),
                  items ? L"yes" : L"NO");
}

void NoteGameController() noexcept
{
    GameController();
}

void Remove() noexcept
{
    if (g_rebuildShortcuts.IsInstalled() && !g_rebuildShortcuts.Restore())
    {
        platform::Log(L"command menu two: the shortcut rebuild was changed after us; left in place (it passes through)");
    }
    for (auto* call : {&g_statusShortcutsCall, &g_itemShortcutsCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"command menu two: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::shortcut_follow
