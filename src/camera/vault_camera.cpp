#include "camera/vault_camera.h"

#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/move_state.h"
#include "input/member_movement.h"
#include "platform/call_redirect.h"
#include "platform/code_memory.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/actor_fields.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "rides/chocobo_ride.h"
#include "scopes/primary_controller.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>

namespace lss::camera::vault_camera {

namespace {

// FEndBehaviorVaultExtraAction::Begin (vtable +0x98): `call IsUCPC_Core` at +0x102, E8 at +0x2 of its block (1.005:
// +0x6). research/RE-p2-chocobo-jump-camera.md.
constexpr std::uintptr_t kCoreCall = 0x1415B9F86;
using CoreFn = std::uint8_t (*)(const void* character);

// FEndBehaviorVaultExtraAction::Begin itself, through its vtable slot: called by PushAction from the vault state's
// Enter, on the game thread. The same first bytes in both builds.
constexpr std::uint8_t kBeginBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                        0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D};
constexpr platform::SlotFact kBeginSlot{
    L"the vault action's Begin slot", 0x145B38EA8,
    {L"FEndBehaviorVaultExtraAction::Begin", 0x1415B9E84, kBeginBytes}};
using BeginFn = std::uint8_t (*)(void* action);

// FEndBehaviorVaultExtraAction::End (vtable +0x120), on the animation worker:
// `mov cl, 0Fh; call PrimaryPCM_FieldOp_DeactivateModule`, the action in rbx (mov rbx, rcx at entry). One block in
// both builds. research/RE-p2-chocobo-jump-camera.md.
constexpr std::uintptr_t kEndDeactivateCall = 0x1415BBF4A;
constexpr std::uint32_t  kActionCharacter   = 0x450;   // the action's character, an FWeakObjectPtr {index, serial}
constexpr std::uint32_t  kFieldOperator     = 0x2CB0;  // a camera manager's Field operator (a pointer to it)
using facts::kOperatorModules;
constexpr std::uint32_t  kModuleSlot15      = 15;

inline constexpr std::uint8_t kPrimaryDeactivateBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0x9C, 0xBD, 0x00,
    0x48, 0x2B, 0xE0, 0x8A, 0xD9, 0xE8, 0xC6, 0x24, 0x1D, 0xFF, 0x48, 0x85};
inline constexpr std::uint8_t kOperatorDeactivateBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
    0x20, 0x70, 0xD3, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x81, 0xE0, 0x06};
/// void(u8 slot): the primary camera manager's Field operator, DeactivateModule(slot).
inline constexpr platform::FunctionFact kPrimaryDeactivate{L"PrimaryPCM_FieldOp_DeactivateModule", 0x141591F0C,
                                                           kPrimaryDeactivateBytes};
/// (operator, u8 slot): module = (*(op+0x6E0))[slot]; module->Deactivate(). The call the primary one makes, on any operator.
inline constexpr platform::FunctionFact kOperatorDeactivate{L"FEndModularCameraOperator::DeactivateModule", 0x141434B50,
                                                            kOperatorDeactivateBytes};
using PrimaryDeactivateFn  = void (*)(std::uint8_t slot);
using OperatorDeactivateFn = void (*)(void* op, std::uint8_t slot);

// Set once at install.
platform::CallRedirect g_coreCall;
platform::CallRedirect g_endCall;
platform::VTableEntry  g_begin;
OperatorDeactivateFn   g_operatorDeactivate = nullptr;
std::optional<platform::GameImage> g_image;

// Written on the game thread each tick, read by the vault's End on the animation worker: each seat's Field operator
// (the members' and the chocobos' weaks it compares the action's with are generation's). By seat; index 0 unused.
std::array<std::atomic<void*>, players::kMaxPlayers> g_seatOperator{};

std::wstring ScopeText(int seat)
{
    const wchar_t* name = players::SeatName(seat);
    return scopes::primary_controller::Is(players::generation::ControllerOf(seat)) ? std::format(L"in {}'s primary scope", name)
                                                                  : std::format(L"outside {}'s scope", name);
}

/// Whose primary scope is open now, for player 1's vault line: a seat's, or none.
std::wstring OpenScopeText()
{
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (scopes::primary_controller::Is(players::generation::ControllerOf(seat)))
        {
            return std::format(L"in {}'s primary scope", players::SeatName(seat));
        }
    }
    return L"outside players 2 and 3's scopes";
}

/// The built seat whose member is `character` while its player is in, or 0.
int MemberSeat(const void* character) noexcept
{
    const int seat = players::generation::SeatOfMember(character);
    return players::SeatBuilt(seat) && players::generation::Active(seat) ? seat : 0;
}

// The game's own call at this site, passed through unguarded on purpose (no CallGuarded): the game makes it unguarded
// too. CallGuarded is for the extra calls the mod adds (member_movement's RiddenTest, PlayerCoreAsSeat).
std::uint8_t VaultCoreTest(const void* character) noexcept
{
    const auto engine = reinterpret_cast<CoreFn>(g_coreCall.Original());
    if (!players::generation::OnGameThread() || !players::generation::AnyActive() || character == nullptr)
    {
        return engine(character);
    }
    const int  rider   = rides::chocobo_ride::SeatDriving(character);  // seated or getting off
    const bool chocobo = rider >= 1 && players::generation::Active(rider);
    const int  member  = MemberSeat(character);
    if (!chocobo)
    {
        const auto answer = engine(character);
        try
        {
            if (member != 0)
            {
                const wchar_t* name = players::SeatName(member);
                platform::Log(L"vault camera: {}'s member vaults on foot; the game's player-core test answers {} ({}){}",
                              name, answer, ScopeText(member),
                              answer != 0 ? std::format(L": a vault camera goes on {}'s camera; its end deactivates it "
                                                        L"there (lever L2)",
                                                        name)
                                          : std::wstring());
            }
            else if (character == players::actor_fields::Pawn(players::generation::ControllerOf(0)))
            {
                platform::LogDebug(L"vault camera: player 1's pawn vaults; the game's player-core test answers {} ({})",
                                   answer, OpenScopeText());
            }
        }
        catch (...)
        {
        }
        return answer;
    }
    const auto answer = input::member_movement::WithoutChocoboAnswer(engine, character);
    try
    {
        platform::Log(L"vault camera: {}'s chocobo vaults ({}); the game's player-core test answers {} without the "
                      L"mod's ride answer for its input component: {}",
                      players::SeatName(rider), ScopeText(rider), answer,
                      answer == 0 ? L"no vault camera, as for player 1's chocobo; both ride framings stay"
                                  : L"a vault camera STILL goes on the primary camera (the ride framings may be lost)");
    }
    catch (...)
    {
    }
    return answer;
}

struct BeginCall
{
    BeginFn      begin;
    void*        action;
    std::uint8_t result = 0;
};

/// The vault's Begin. A vault the game chains straight after another (a cart's second step) is begun outside the
/// member's tick, where the player-core test answers no for a seat's member and the install would go to player 1's
/// camera. A seat's member's vault begun outside that seat's scope runs in it, as the member's own vaults do: its vault
/// camera then goes on the seat's camera, as player 1's chained vault gets his. Inside another seat's scope the seat's
/// is refused (seats never nest) and the Begin runs as called, logged. Every other Begin is the game's as called.
std::uint8_t Begin(void* action) noexcept
{
    const auto original = reinterpret_cast<BeginFn>(g_begin.Original());
    if (!players::generation::OnGameThread() || !players::generation::AnyActive())
    {
        return original(action);
    }
    const auto weak = ReadField<std::uint64_t>(action, kActionCharacter);
    const int  seat = weak != 0 ? players::generation::SeatOfMemberWeak(weak) : -1;
    if (!players::SeatBuilt(seat) || !players::generation::Active(seat) ||
        scopes::primary_controller::Is(players::generation::ControllerOf(seat)))
    {
        return original(action);
    }
    BeginCall call{original, action};
    const bool scoped = input::member_movement::InPrimaryScope(
        seat, players::generation::MemberOf(seat),
        [](void* context) noexcept {
            auto& begun  = *static_cast<BeginCall*>(context);
            begun.result = begun.begin(begun.action);
        },
        &call);
    if (!scoped)
    {
        call.result = original(action);
    }
    try
    {
        platform::LogDebug(L"vault camera: {}'s member's vault begins outside their scope (a vault chained after another): {}",
                           players::SeatName(seat),
                           scoped ? L"begun in their primary scope, as their own vaults are"
                                  : L"their scope could not be held; begun as the game called it (no vault camera)");
    }
    catch (...)
    {
    }
    return call.result;
}

std::uintptr_t AsSource(std::uintptr_t live) noexcept
{
    return g_image ? g_image->SourceAddress(live) : live;
}

std::uintptr_t Slot15Vtable(const void* op) noexcept
{
    const void* module = camera_access::ModuleAt(op, kModuleSlot15);
    return module != nullptr ? AsSource(VTableOf(module)) : 0;
}

/// The vault End's deactivate (lever L2): the stub hands over the action from rbx. Any thread; the game's own End runs
/// here on the animation worker. A seat's member's or chocobo's vault camera is ended on that seat's camera, where its
/// Begin put it (in the seat's primary scope); every other vault ends as the game ends it, on the primary camera.
void EndDeactivate(std::uint8_t slot, const void* action) noexcept
{
    const auto primary = reinterpret_cast<PrimaryDeactivateFn>(g_endCall.Original());
    const auto weak    = ReadField<std::uint64_t>(action, kActionCharacter);
    int        seat    = weak != 0 ? players::generation::SeatOfMemberWeak(weak) : -1;
    const bool member  = players::SeatBuilt(seat);
    bool       chocobo = false;
    for (int s = 1; !member && weak != 0 && s <= players::kSeatsBuilt; ++s)
    {
        if (weak == players::generation::ChocoboWeakOf(s))
        {
            seat    = s;
            chocobo = true;
            break;
        }
    }
    // Active(seat) first: a map-load leave withdraws the generation before its teardown, in the same game-thread call,
    // ahead of the next Tick that clears the cached operator. A camera manager destroyed by that teardown is only freed
    // by a later garbage collection, which runs on the game thread outside the frame's animation work.
    void* seatOperator = (member || chocobo) && players::generation::Active(seat)
                             ? g_seatOperator[static_cast<std::size_t>(seat)].load(std::memory_order_acquire)
                             : nullptr;
    if ((!member && !chocobo) || seatOperator == nullptr || g_operatorDeactivate == nullptr)
    {
        primary(slot);
        try
        {
            platform::LogDebug(L"vault camera: a vault ends ({} thread): the game's deactivate of the primary camera's "
                               L"slot {}",
                               players::generation::OnGameThread() ? L"game" : L"animation", slot);
        }
        catch (...)
        {
        }
        return;
    }
    // The operator was cached at the last tick, on the game thread; the game's own deactivate resolves it in the same
    // call. So the module table is read again here (guarded), and the game's call, an extra call the mod adds on this
    // operator, is guarded: a table gone since the tick skips the deactivate, logged, instead of ending the game.
    void* table = nullptr;
    const bool tableHeld = platform::CopyGuarded(&table, static_cast<const std::byte*>(seatOperator) + kOperatorModules,
                                                 sizeof(table)) &&
                           table != nullptr;
    const auto before = tableHeld ? Slot15Vtable(seatOperator) : 0;
    const bool done   = tableHeld && platform::CallGuarded(g_operatorDeactivate, seatOperator, slot);
    try
    {
        const wchar_t* name = players::SeatName(seat);
        if (done)
        {
            platform::Log(L"vault camera: {}'s {} vault ends ({} thread): {}'s camera's slot {} deactivated "
                          L"(module vt 0x{:X}), where its Begin put it; player 1's camera untouched",
                          name, member ? L"member's" : L"chocobo's", players::generation::OnGameThread() ? L"game" : L"animation",
                          name, slot, before);
        }
        else
        {
            platform::Log(L"vault camera: {}'s {} vault ends ({} thread): {}'s camera's slot {} NOT "
                          L"deactivated: {}",
                          name, member ? L"member's" : L"chocobo's", players::generation::OnGameThread() ? L"game" : L"animation",
                          name, slot,
                          tableHeld ? platform::DescribeFault(platform::LastFault()).text
                                    : L"its operator's module table is gone since the last tick");
        }
    }
    catch (...)
    {
    }
}

/// The redirect's target: `mov rdx, rbx` (the action), then a jump to EndDeactivate. No call, no stack use.
std::uintptr_t BuildEndStub(std::uintptr_t site) noexcept
{
    static constexpr std::uint8_t kMove[] = {0x48, 0x89, 0xDA};  // mov rdx, rbx
    return platform::PlaceMoveAndJump(site, kMove, reinterpret_cast<std::uintptr_t>(&EndDeactivate));
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image.emplace(image);
    const auto site = image.Rebase(kCoreCall, 5);
    const auto core = platform::FindFunction(image, facts::kIsPlayerCore);
    const bool done = site && core && g_coreCall.Install(*site, *core, reinterpret_cast<std::uintptr_t>(&VaultCoreTest));
    platform::Log(L"vault camera: {}", done ? L"installed (player 2's or 3's chocobo vaults without a vault camera, as player 1's "
                                              L"does; each such vault logged)"
                                            : L"UNAVAILABLE (the vault's call differs; player 2's or 3's chocobo vault can still "
                                              L"take both ride framings)");

    // L2: the End's call, checked to be followed by `and byte [rbx+4A0h], 0FDh` (rbx is the action there).
    static constexpr std::uint8_t kAfterCall[] = {0x80, 0xA3, 0xA0, 0x04, 0x00, 0x00, 0xFD};
    const auto end      = image.Rebase(kEndDeactivateCall, 5);
    const auto primary  = platform::FindFunction(image, kPrimaryDeactivate);
    const auto op       = platform::FindFunction(image, kOperatorDeactivate);
    std::uint8_t after[sizeof(kAfterCall)]{};
    const bool   rbxHolds = end && platform::CopyGuarded(after, reinterpret_cast<const void*>(*end + 5), sizeof(after)) &&
                          std::memcmp(after, kAfterCall, sizeof(kAfterCall)) == 0;
    const auto stub     = end && primary && op && rbxHolds ? BuildEndStub(*end) : 0;
    if (stub != 0)
    {
        g_operatorDeactivate = reinterpret_cast<OperatorDeactivateFn>(*op);
    }
    const bool ended = stub != 0 && g_endCall.Install(*end, *primary, stub);
    platform::Log(L"vault camera: {}",
                  ended ? L"the vault's end installed (player 2's or 3's vault cameras end on that player's camera, as the game "
                          L"ends player 1's on his; each logged)"
                        : L"the vault's end UNAVAILABLE (its call or the action register differs; player 2's or 3's on-foot "
                          L"vault camera stays on that player's camera, and its end clears player 1's slot 15)");

    // The chained vault's Begin needs the End above: its camera goes on the seat's camera, which only L2 ends.
    const bool begun = ended && platform::ReplaceSlot(image, kBeginSlot, reinterpret_cast<std::uintptr_t>(&Begin), g_begin);
    platform::Log(L"vault camera: {}",
                  begun ? L"a vault chained after another (a cart's second step) begins in player 2's or 3's scope, as their "
                          L"own vaults do (their vault camera on their camera, as player 1's)"
                        : L"the chained vault's begin UNAVAILABLE (player 2's or 3's second vault in a row, as on a cart, runs "
                          L"on the follow camera)");
}

void Remove() noexcept
{
    if (!g_coreCall.Restore())
    {
        platform::Log(L"vault camera: the vault's call was changed after us; left in place (it passes through)");
    }
    if (!g_endCall.Restore())
    {
        platform::Log(L"vault camera: the vault end's call was changed after us; left in place (it passes through)");
    }
    if (g_begin.Original() != 0 && !g_begin.Restore())
    {
        platform::Log(L"vault camera: the vault's Begin slot was changed after us; left in place (it passes through)");
    }
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        auto& cached = g_seatOperator[static_cast<std::size_t>(seat)];
        if (!players::generation::Active(seat))
        {
            cached.store(nullptr, std::memory_order_release);
            continue;
        }
        // The operator as the game's PrimaryPCM_FieldOp_DeactivateModule loads it (*(PCM+0x2CB0)), and only with its
        // module table in place: the game's DeactivateModule reads the table unchecked.
        void* op = ReadPointer(players::actor_fields::CameraManager(players::generation::ControllerOf(seat)), kFieldOperator);
        cached.store(op != nullptr && ReadPointer(op, kOperatorModules) != nullptr ? op : nullptr,
                     std::memory_order_release);
    }
}

}  // namespace lss::camera::vault_camera
