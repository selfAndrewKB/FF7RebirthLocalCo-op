#include "player_two/camera_effects_two.h"

#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"
#include "player_two/primary_controller.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace lss::player_two::camera_effects_two {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-attack-cameras.md §9.4-§9.6 and §9.11 (S-ATKCAMEDGE); this build's slots and the functions they hold.
// Guard effect: vt_FEndCharacterControllerComponentBattlePlayer +0x678 (1.005 +0x680): char fn(component, const FVector*).
constexpr std::uintptr_t kGuardSlot = 0x145796550;
constexpr std::uintptr_t kGuard     = 0x14143C948;  // CharComp_Vt_PlayCameraEffectIfEnabled
// The shake notifies (the AnimNotify slots as combat_rumble wraps them: Notify / End (notify, mesh, animation), Begin
// with the duration).
constexpr std::uintptr_t kShakeSlot      = 0x1457F7D70;
constexpr std::uintptr_t kShake          = 0x140E52890;  // UEndBattleCameraShakeNotify::Notify
constexpr std::uintptr_t kShakeBeginSlot = 0x1459075E8;
constexpr std::uintptr_t kShakeBegin     = 0x1418AFB54;  // UEndBattleCameraShakeNotifyState::NotifyBegin
constexpr std::uintptr_t kShakeEndSlot   = 0x1459075F8;
constexpr std::uintptr_t kShakeEnd       = 0x1418AFCE8;  // UEndBattleCameraShakeNotifyState::NotifyEnd
// StealCamera NotifyBegin, and the game's own pair test it uses: Character_SharesActionKeyWith(a, b), both characters'
// action key (char+0x638 -> attack state +0xE8/+0xF0) the same and not null.
constexpr std::uintptr_t kStealBeginSlot = 0x145886240;
constexpr std::uintptr_t kStealBegin     = 0x141812908;
constexpr std::uintptr_t kSharesKey      = 0x141812A08;
constexpr std::uint8_t   kSharesKeyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8};

constexpr std::uint32_t kComponentOwner = 0xA0;  // UActorComponent::OwnerPrivate (the mesh's character)
constexpr auto          kStatusEvery    = std::chrono::seconds(10);
constexpr int           kMaxLines       = 200;

using GuardFn     = char (*)(void* component, const void* location);
using Notify3Fn   = void (*)(void* notify, void* mesh, void* animation);
using Notify4Fn   = void (*)(void* notify, void* mesh, void* animation, float duration);
using SharesKeyFn = char (*)(const void* character, const void* other);

// Set once at install.
platform::VTableEntry g_guardSlot, g_shakeSlot, g_shakeBeginSlot, g_shakeEndSlot, g_stealSlot;
SharesKeyFn           g_sharesKey = nullptr;

// Game thread (the off-thread count is atomic).
struct Counts
{
    std::uint32_t guards = 0, shakes = 0, shakeStates = 0, steals = 0, refused = 0;
};
Counts                     g_counts;
std::atomic<std::uint32_t> g_offThread{0};
Clock::time_point          g_statusAt{};
int                        g_lines = 0;

bool Ready() noexcept
{
    if (!generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return generation::Active() && generation::Member() != nullptr;
}

bool Line() noexcept
{
    return platform::Enabled(platform::Level::Debug) && g_lines < kMaxLines && ++g_lines > 0;
}

/// Player 2's leader view (leader, leader type, and with `primary` the primary controller) for one call; nothing held if
/// any part refuses.
class PlayerTwoView
{
public:
    PlayerTwoView(void* member, bool primary) noexcept
    {
        const auto type = party_member::PlayerTypeOf(member);
        const auto weak = leader_scope::WeakOf(member);
        if (!type || weak == 0)
        {
            return;
        }
        if (primary)
        {
            primary_.emplace();
            if (!primary_->Held())
            {
                primary_.reset();
                return;
            }
        }
        leader_.emplace(weak);
        type_.emplace(*type);
        if (!leader_->Held() || !type_->Held())
        {
            type_.reset();
            leader_.reset();
            primary_.reset();
            return;
        }
        held_ = true;
    }

    bool Held() const noexcept { return held_; }

private:
    std::optional<primary_controller::Scope>     primary_;  // destroyed last (opened first)
    std::optional<leader_scope::LeaderScope>     leader_;
    std::optional<leader_scope::LeaderTypeScope> type_;
    bool                                         held_ = false;
};

void* OwnerOf(const void* mesh) noexcept
{
    return ReadPointer(mesh, kComponentOwner);
}

// ---------------------------------------------------------------------------------------------
// Guard effect
// ---------------------------------------------------------------------------------------------

char Guard(void* component, const void* location) noexcept
{
    const auto original = reinterpret_cast<GuardFn>(g_guardSlot.Original());
    if (!Ready())
    {
        return original(component, location);
    }
    void*      member = generation::Member();
    const auto type   = party_member::PlayerTypeOf(member);
    if (!type || member_movement::ComponentPlayerType(component) != *type)
    {
        return original(component, location);
    }
    const PlayerTwoView view(member, true);
    if (!view.Held())
    {
        ++g_counts.refused;
        return original(component, location);
    }
    ++g_counts.guards;
    const char played = original(component, location);
    if (Line())
    {
        platform::LogDebug(L"camera effects: player 2's member's guard camera effect {} player 2's camera",
                           played != 0 ? L"on" : L"closed by the game's gate for");
    }
    return played;
}

// ---------------------------------------------------------------------------------------------
// Shake notifies
// ---------------------------------------------------------------------------------------------

/// Whether a notify on `mesh` is player 2's member's (the shake then runs in player 2's view).
bool MembersMesh(const void* mesh) noexcept
{
    return Ready() && mesh != nullptr && OwnerOf(mesh) == generation::Member();
}

void Shake(void* notify, void* mesh, void* animation) noexcept
{
    const auto original = reinterpret_cast<Notify3Fn>(g_shakeSlot.Original());
    if (!MembersMesh(mesh))
    {
        original(notify, mesh, animation);
        return;
    }
    const PlayerTwoView view(generation::Member(), true);
    g_counts.shakes += view.Held() ? 1 : 0;
    g_counts.refused += view.Held() ? 0 : 1;
    original(notify, mesh, animation);
}

void ShakeBegin(void* notify, void* mesh, void* animation, float duration) noexcept
{
    const auto original = reinterpret_cast<Notify4Fn>(g_shakeBeginSlot.Original());
    if (!MembersMesh(mesh))
    {
        original(notify, mesh, animation, duration);
        return;
    }
    const PlayerTwoView view(generation::Member(), true);
    g_counts.shakeStates += view.Held() ? 1 : 0;
    g_counts.refused += view.Held() ? 0 : 1;
    original(notify, mesh, animation, duration);
}

/// The End stops the component's shake handle on the primary camera: the same camera as its Begin (both keyed on the
/// mesh's owner).
void ShakeEnd(void* notify, void* mesh, void* animation) noexcept
{
    const auto original = reinterpret_cast<Notify3Fn>(g_shakeEndSlot.Original());
    if (!MembersMesh(mesh))
    {
        original(notify, mesh, animation);
        return;
    }
    const PlayerTwoView view(generation::Member(), true);
    g_counts.refused += view.Held() ? 0 : 1;
    original(notify, mesh, animation);
}

// ---------------------------------------------------------------------------------------------
// StealCamera
// ---------------------------------------------------------------------------------------------

void StealBegin(void* notify, void* mesh, void* animation, float duration) noexcept
{
    const auto original = reinterpret_cast<Notify4Fn>(g_stealSlot.Original());
    if (!Ready() || mesh == nullptr)
    {
        original(notify, mesh, animation, duration);
        return;
    }
    void*       member = generation::Member();
    const void* owner  = OwnerOf(mesh);
    // Another party character (the game's own Begin wants an EndPlayerCharacter; the pair test reads character fields),
    // in a chain pair with player 2's member by the game's own test.
    if (owner == nullptr || owner == member || !party_member::PlayerTypeOf(owner) || g_sharesKey(owner, member) == 0)
    {
        original(notify, mesh, animation, duration);
        return;
    }
    const PlayerTwoView view(member, false);  // the stealer is the leader by type; no camera call inside Begin
    if (!view.Held())
    {
        ++g_counts.refused;
        original(notify, mesh, animation, duration);
        return;
    }
    ++g_counts.steals;
    original(notify, mesh, animation, duration);
    if (Line())
    {
        platform::LogDebug(L"camera effects: a chain move by {} with player 2's member: player 2's camera looks between "
                           L"player 2's member and its partner (StealCamera with player 2 as the leader)",
                           owner == ReadPointer(generation::PlayerOneController(), facts::kControllerPawn) ? L"player 1's character"
                                                                                          : L"a party ally");
    }
}

bool Replace(const platform::GameImage& image, platform::VTableEntry& entry, std::uintptr_t slot, std::uintptr_t function,
             std::uintptr_t replacement) noexcept
{
    const auto at = image.Rebase(slot, sizeof(void*));
    const auto fn = image.Rebase(function);
    return at && fn && entry.Replace(*at, *fn, replacement);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const bool guard = Replace(image, g_guardSlot, kGuardSlot, kGuard, reinterpret_cast<std::uintptr_t>(&Guard));
    // The shakes as one item: a Begin played on player 2's camera must be stopped there by its End.
    bool shakes = Replace(image, g_shakeBeginSlot, kShakeBeginSlot, kShakeBegin, reinterpret_cast<std::uintptr_t>(&ShakeBegin));
    shakes      = shakes && Replace(image, g_shakeEndSlot, kShakeEndSlot, kShakeEnd, reinterpret_cast<std::uintptr_t>(&ShakeEnd));
    shakes      = shakes && Replace(image, g_shakeSlot, kShakeSlot, kShake, reinterpret_cast<std::uintptr_t>(&Shake));
    if (!shakes)
    {
        g_shakeSlot.Restore();
        g_shakeEndSlot.Restore();
        g_shakeBeginSlot.Restore();
    }
    const auto shares = image.Rebase(kSharesKey);
    bool       steal  = false;
    if (shares && image.BytesMatch(kSharesKey, kSharesKeyBytes))
    {
        g_sharesKey = reinterpret_cast<SharesKeyFn>(*shares);
        steal = Replace(image, g_stealSlot, kStealBeginSlot, kStealBegin, reinterpret_cast<std::uintptr_t>(&StealBegin));
    }
    platform::Log(L"camera effects: player 2's member's guard effect {}; its camera-shake notifies {}; chain-move camera "
                  L"pulls (StealCamera) {}",
                  guard ? L"on player 2's camera" : L"UNCHANGED (slot not what the research read)",
                  shakes ? L"on player 2's camera" : L"UNCHANGED (a slot is not what the research read)",
                  steal ? L"on player 2's camera for pairs with player 2's member"
                        : L"UNCHANGED (a slot or the pair test is not what the research read)");
}

void Tick() noexcept
{
    const auto now = Clock::now();
    if (now - g_statusAt < kStatusEvery)
    {
        return;
    }
    g_statusAt          = now;
    const Counts c      = g_counts;
    g_counts            = {};
    const auto offThread = g_offThread.exchange(0, std::memory_order_relaxed);
    if (c.guards + c.shakes + c.shakeStates + c.steals + c.refused + offThread == 0)
    {
        return;
    }
    platform::LogDebug(L"camera effects (10 s): on player 2's camera: guard effects {}, shake notifies {}, shake notify "
                       L"states {}, chain-move pulls {}; player 2's view refused {}; calls off the game thread (the game's "
                       L"own) {}",
                       c.guards, c.shakes, c.shakeStates, c.steals, c.refused, offThread);
}

}  // namespace lss::player_two::camera_effects_two
