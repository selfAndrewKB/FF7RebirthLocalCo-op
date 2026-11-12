#include "world/shooting_targets.h"

#include "facts/field_action.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/object_fields.h"
#include "players/generation.h"
#include "players/weak_object.h"
#include "scopes/leader_scope.h"

#include <cstdint>

namespace lss::world::shooting_targets {

namespace {

using RefreshFn = void (*)(void* global);

RefreshFn      g_refresh = nullptr;
std::uintptr_t g_slot    = 0;      // the live address of the game's pointer to the field-action global (null before it exists)
bool           g_faulted = false;  // the refresh faulted once: left to the game from then on (said)

/// The game's refresh, asked for by its own byte, from the party leader as the slots hold it now; nothing while the
/// game has no field-action global yet.
void Refresh(const wchar_t* whose) noexcept
{
    if (g_refresh == nullptr || g_faulted)
    {
        return;
    }
    auto* global = reinterpret_cast<std::uint8_t*>(ReadAt<std::uintptr_t>(g_slot));
    if (global == nullptr)
    {
        return;
    }
    constexpr std::uint8_t kWanted = 1;
    if (!platform::WriteGuarded(global + facts::kFieldActionGlobalWanted, &kWanted, sizeof(kWanted)) ||
        !platform::CallGuarded(g_refresh, static_cast<void*>(global)))
    {
        g_faulted = true;
        platform::LogWarning(L"shooting targets: the game's refresh of the boxes' leader-near byte faulted for {}; the boxes "
                             L"answer for player 1 alone from here on",
                             whose);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto refresh = platform::FindFunction(image, facts::kRefreshShootingTargets);
    const auto slot    = image.Rebase(facts::kFieldActionGlobal, sizeof(void*));
    g_refresh          = refresh ? reinterpret_cast<RefreshFn>(*refresh) : nullptr;
    g_slot             = slot ? *slot : 0;
    const bool ready   = g_refresh != nullptr && g_slot != 0;
    platform::Log(L"shooting targets: a breakable box counts as near for a player 2 or 3's throw by their own distance {}",
                  ready ? L"(the game's leader-near refresh run for their member before their tick, for player 1 after)"
                        : L"UNAVAILABLE (a check failed): by player 1's distance, as the game has it");
    if (!ready)
    {
        g_refresh = nullptr;
    }
    return ready;
}

void Remove() noexcept
{
    g_refresh = nullptr;
    g_slot    = 0;
    g_faulted = false;
}

void RefreshFor(void* member) noexcept
{
    if (g_refresh == nullptr || member == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    const scopes::leader_scope::LeaderScope leader(players::weak_object::WeakOf(member));
    if (!leader.Held())
    {
        return;
    }
    Refresh(L"a seat's member");
}

void RefreshForPlayerOne() noexcept
{
    if (g_refresh == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    Refresh(L"player 1");
}

}  // namespace lss::world::shooting_targets
