#include "camera/seat_camera_effects.h"

#include "facts/controller.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/leader_answer.h"
#include "scopes/leader_scope.h"
#include "scopes/primary_controller.h"
#include "scopes/seat_ladder.h"
#include "session/party_member.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace lss::camera::seat_camera_effects {

namespace seat_ladder = scopes::seat_ladder;

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
constexpr std::uint8_t   kSharesKeyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kSharesKey{L"the two characters' action keys shared", 0x141812A08, kSharesKeyBytes};

constexpr std::uint32_t kComponentOwner = 0xA0;  // UActorComponent::OwnerPrivate (the mesh's character)
constexpr auto          kStatusEvery    = std::chrono::seconds(10);

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
std::array<Counts, players::kMaxPlayers> g_counts{};  // by seat; index 0 unused
// The meshes whose shake NotifyState Begin ran on a seat's camera, with the seat: their End stops it there (the handle
// is per component, so an End on another camera could stop an unrelated shake; S-ATKCAMVERIFY 4b).
struct ShakeMesh
{
    const void* mesh = nullptr;
    int         seat = 0;
};
std::array<ShakeMesh, 16>  g_shakeMeshes{};
std::atomic<std::uint32_t> g_offThread{0};
platform::Every g_status{kStatusEvery};

Counts& CountsOf(int seat) noexcept
{
    return g_counts[static_cast<std::size_t>(seat)];
}

/// The seat's member while its player is in, or null.
void* SeatMember(int seat) noexcept
{
    return players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
}

/// On the game thread (counted otherwise) with a built seat's player in with a member.
bool Ready() noexcept
{
    if (!players::generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (SeatMember(seat) != nullptr)
        {
            return true;
        }
    }
    return false;
}

bool Line() noexcept
{
    return platform::Enabled(platform::Level::Debug);
}

/// A seat's leader view (leader, leader type, and with `primary` the primary controller) for one call; nothing held if
/// any part refuses (the ladder's all-or-nothing rule).
class PlayerTwoView
{
public:
    PlayerTwoView(int seat, void* member, bool primary) noexcept
    {
        seat_ladder::Args args;
        args.seat = seat;
        if (seat_ladder::FillMember(args, member))
        {
            scopes_.emplace((primary ? seat_ladder::Primary : 0u) | seat_ladder::Leader | seat_ladder::LeaderType,
                            args);
        }
    }

    bool Held() const noexcept { return scopes_ && scopes_->Held(); }

private:
    std::optional<seat_ladder::P2Scope> scopes_;
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
    // The seat whose member's player type the component's is (the first in seat order).
    int   seat   = 0;
    void* member = nullptr;
    for (int s = 1; s <= players::kSeatsBuilt && seat == 0; ++s)
    {
        void*      candidate = SeatMember(s);
        const auto type      = candidate != nullptr ? session::party_member::PlayerTypeOf(candidate) : std::nullopt;
        if (type && scopes::leader_answer::ComponentPlayerType(component) == *type)
        {
            seat   = s;
            member = candidate;
        }
    }
    if (seat == 0)
    {
        return original(component, location);
    }
    const PlayerTwoView view(seat, member, true);
    if (!view.Held())
    {
        ++CountsOf(seat).refused;
        return original(component, location);
    }
    ++CountsOf(seat).guards;
    const char played = original(component, location);
    if (Line())
    {
        platform::LogDebug(L"camera effects: {}'s member's guard camera effect {} {}'s camera", players::SeatName(seat),
                           played != 0 ? L"on" : L"closed by the game's gate for", players::SeatName(seat));
    }
    return played;
}

// ---------------------------------------------------------------------------------------------
// Shake notifies
// ---------------------------------------------------------------------------------------------

/// The seat whose member owns `mesh` (a shake then runs in that seat's view), or 0.
int MeshSeat(const void* mesh) noexcept
{
    if (!Ready() || mesh == nullptr)
    {
        return 0;
    }
    const void* owner = OwnerOf(mesh);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (owner != nullptr && owner == SeatMember(seat))
        {
            return seat;
        }
    }
    return 0;
}

void Shake(void* notify, void* mesh, void* animation) noexcept
{
    const auto original = reinterpret_cast<Notify3Fn>(g_shakeSlot.Original());
    const int  seat     = MeshSeat(mesh);
    if (seat == 0)
    {
        original(notify, mesh, animation);
        return;
    }
    const PlayerTwoView view(seat, players::generation::MemberOf(seat), true);
    CountsOf(seat).shakes += view.Held() ? 1 : 0;
    CountsOf(seat).refused += view.Held() ? 0 : 1;
    original(notify, mesh, animation);
}

void RememberShake(const void* mesh, int seat) noexcept
{
    for (ShakeMesh& slot : g_shakeMeshes)
    {
        if (slot.mesh == mesh)
        {
            slot.seat = seat;
            return;
        }
    }
    for (ShakeMesh& slot : g_shakeMeshes)
    {
        if (slot.mesh == nullptr)
        {
            slot = {mesh, seat};
            return;
        }
    }
    g_shakeMeshes[0] = {mesh, seat};  // full (an End never came): one entry goes
}

/// Forgets `mesh`; the seat on whose camera its Begin ran, or 0.
int ForgetShake(const void* mesh) noexcept
{
    for (ShakeMesh& slot : g_shakeMeshes)
    {
        if (slot.mesh == mesh)
        {
            const int seat = slot.seat;
            slot           = {};
            return seat;
        }
    }
    return 0;
}

void ShakeBegin(void* notify, void* mesh, void* animation, float duration) noexcept
{
    const auto original = reinterpret_cast<Notify4Fn>(g_shakeBeginSlot.Original());
    const int  seat     = MeshSeat(mesh);
    if (seat == 0)
    {
        original(notify, mesh, animation, duration);
        return;
    }
    const PlayerTwoView view(seat, players::generation::MemberOf(seat), true);
    CountsOf(seat).shakeStates += view.Held() ? 1 : 0;
    CountsOf(seat).refused += view.Held() ? 0 : 1;
    if (view.Held())
    {
        RememberShake(mesh, seat);
    }
    original(notify, mesh, animation, duration);
}

/// The End stops the component's shake handle on the primary camera: on the seat's camera exactly when its Begin ran
/// there.
void ShakeEnd(void* notify, void* mesh, void* animation) noexcept
{
    const auto original = reinterpret_cast<Notify3Fn>(g_shakeEndSlot.Original());
    const int  seat     = players::generation::OnGameThread() ? ForgetShake(mesh) : 0;
    if (seat == 0)
    {
        original(notify, mesh, animation);
        return;
    }
    if (SeatMember(seat) == nullptr)
    {
        return;  // the seat's player left: the shake went with its camera manager; nothing of it to stop on player 1's
    }
    const PlayerTwoView view(seat, players::generation::MemberOf(seat), true);
    CountsOf(seat).refused += view.Held() ? 0 : 1;
    if (view.Held())
    {
        original(notify, mesh, animation);
    }
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
    const void* owner = OwnerOf(mesh);
    // Another party character (the game's own Begin wants an EndPlayerCharacter; the pair test reads character fields),
    // in a chain pair with a seat's member by the game's own test: the first seat in seat order.
    int   seat      = 0;
    void* member    = nullptr;
    bool  typeKnown = false;
    for (int s = 1; owner != nullptr && s <= players::kSeatsBuilt && seat == 0; ++s)
    {
        void* candidate = SeatMember(s);
        char  shares    = 0;
        if (candidate == nullptr || owner == candidate)
        {
            continue;  // the seat's member's own notify stays the game's
        }
        if (!typeKnown && !session::party_member::PlayerTypeOf(owner))
        {
            break;
        }
        typeKnown = true;
        if (platform::CallGuarded(g_sharesKey, shares, owner, static_cast<const void*>(candidate)) && shares != 0)
        {
            seat   = s;
            member = candidate;
        }
    }
    if (seat == 0)
    {
        original(notify, mesh, animation, duration);
        return;
    }
    const PlayerTwoView view(seat, member, false);  // the stealer is the leader by type; no camera call inside Begin
    if (!view.Held())
    {
        ++CountsOf(seat).refused;
        original(notify, mesh, animation, duration);
        return;
    }
    ++CountsOf(seat).steals;
    original(notify, mesh, animation, duration);
    if (Line())
    {
        const wchar_t* name = players::SeatName(seat);
        platform::LogDebug(L"camera effects: a chain move by {} with {}'s member: {}'s camera looks between "
                           L"{}'s member and its partner (StealCamera with {} as the leader)",
                           owner == ReadPointer(players::generation::ControllerOf(0), facts::kControllerPawn)
                               ? L"player 1's character"
                               : L"a party ally",
                           name, name, name, name);
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const bool guard = g_guardSlot.Replace(image, kGuardSlot, kGuard, reinterpret_cast<std::uintptr_t>(&Guard));
    // The shakes as one item: a Begin played on player 2's camera must be stopped there by its End.
    bool shakes = g_shakeBeginSlot.Replace(image, kShakeBeginSlot, kShakeBegin,
                                           reinterpret_cast<std::uintptr_t>(&ShakeBegin));
    shakes      = shakes && g_shakeEndSlot.Replace(image, kShakeEndSlot, kShakeEnd,
                                                   reinterpret_cast<std::uintptr_t>(&ShakeEnd));
    shakes      = shakes && g_shakeSlot.Replace(image, kShakeSlot, kShake, reinterpret_cast<std::uintptr_t>(&Shake));
    if (!shakes)
    {
        g_shakeSlot.Restore();
        g_shakeEndSlot.Restore();
        g_shakeBeginSlot.Restore();
    }
    const auto shares = platform::FindFunction(image, kSharesKey);
    bool       steal  = false;
    if (shares)
    {
        g_sharesKey = reinterpret_cast<SharesKeyFn>(*shares);
        steal = g_stealSlot.Replace(image, kStealBeginSlot, kStealBegin, reinterpret_cast<std::uintptr_t>(&StealBegin));
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
    if (!g_status.Due(now))
    {
        return;
    }
    const auto counts    = std::exchange(g_counts, {});
    const auto offThread = platform::Take(g_offThread);
    std::uint32_t total  = offThread;
    for (const Counts& c : counts)
    {
        total += c.guards + c.shakes + c.shakeStates + c.steals + c.refused;
    }
    if (total == 0)
    {
        return;
    }
    try
    {
        std::wstring seats;  // per built seat, as the line named player 2 alone
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            const Counts&  c    = counts[static_cast<std::size_t>(seat)];
            const wchar_t* name = players::SeatName(seat);
            seats += std::format(L"on {}'s camera: guard effects {}, shake notifies {}, shake notify states {}, "
                                 L"chain-move pulls {}; {}'s view refused {}; ",
                                 name, c.guards, c.shakes, c.shakeStates, c.steals, name, c.refused);
        }
        platform::LogDebug(L"camera effects (10 s): {}calls off the game thread (the game's own) {}", seats, offThread);
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    for (platform::VTableEntry* slot : {&g_stealSlot, &g_shakeSlot, &g_shakeEndSlot, &g_shakeBeginSlot, &g_guardSlot})
    {
        if (!slot->Restore())
        {
            platform::Log(L"camera effects: a slot was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::camera::seat_camera_effects
