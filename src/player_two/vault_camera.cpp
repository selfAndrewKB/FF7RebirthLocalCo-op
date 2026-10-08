#include "player_two/vault_camera.h"

#include "platform/call_redirect.h"
#include "platform/code_memory.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/chocobo_ride.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace lss::player_two::vault_camera {

namespace {

// FEndBehaviorVaultExtraAction::Begin (vtable +0x98; 1.004 0x1416E8E50, 1.005 0x1415B9E84): `call IsUCPC_Core` at
// +0x102, E8 at +0x2 of its block (1.005 0x1415B9F86, +0x6). RE-p2-chocobo-jump-camera.md §8.
constexpr std::uintptr_t kCoreCall = 0x1416E8F52;
using CoreFn = std::uint8_t (*)(const void* character);

// FEndBehaviorVaultExtraAction::End (vtable +0x120; 1.004 0x1416EA120, 1.005 0x1415BBD8C), on the animation worker:
// `mov cl, 0Fh; call PrimaryPCM_FieldOp_DeactivateModule`, the action in rbx (mov rbx, rcx at entry). One block in
// both builds (1.005 0x1415BBF4A). RE-p2-chocobo-jump-camera.md §7 L2.
constexpr std::uintptr_t kEndDeactivateCall = 0x1416EA304;
constexpr std::uint32_t  kActionCharacter   = 0x450;   // the action's character, an FWeakObjectPtr {index, serial}
constexpr std::uint32_t  kFieldOperator     = 0x2CB0;  // a camera manager's Field operator (a pointer to it)
constexpr std::uint32_t  kOperatorModules   = 0x6E0;   // an operator's module table (a pointer to it), 16 bytes a slot
constexpr std::uint32_t  kModuleSlot15      = 15 * 16;

inline constexpr std::uint8_t kPrimaryDeactivateBytes[] = {
    0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xBC, 0x66, 0x9C, 0x00,
    0x48, 0x2B, 0xE0, 0x8A, 0xD9, 0xE8, 0xD2, 0x83, 0x37, 0xFF, 0x48, 0x85};
inline constexpr std::uint8_t kOperatorDeactivateBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
    0x00, 0xB3, 0xEA, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x81, 0xE0, 0x06};
/// void(u8 slot): the primary camera manager's Field operator, DeactivateModule(slot).
inline constexpr platform::FunctionFact kPrimaryDeactivate{L"PrimaryPCM_FieldOp_DeactivateModule", 0x1416E8868,
                                                           kPrimaryDeactivateBytes};
/// (operator, u8 slot): module = (*(op+0x6E0))[slot]; module->Deactivate(). The call the primary one makes, on any operator.
inline constexpr platform::FunctionFact kOperatorDeactivate{L"FEndModularCameraOperator::DeactivateModule", 0x141203C20,
                                                            kOperatorDeactivateBytes};
using PrimaryDeactivateFn  = void (*)(std::uint8_t slot);
using OperatorDeactivateFn = void (*)(void* op, std::uint8_t slot);

// Set once at install.
platform::CallRedirect g_coreCall;
platform::CallRedirect g_endCall;
OperatorDeactivateFn   g_operatorDeactivate = nullptr;
std::optional<platform::GameImage> g_image;

// Written on the game thread each tick, read by the vault's End on the animation worker: player 2's member and chocobo
// as weak values (compared with the action's, never resolved there) and player 2's Field operator.
std::atomic<std::uint64_t> g_memberWeak{0};
std::atomic<std::uint64_t> g_chocoboWeak{0};
std::atomic<void*>         g_operatorTwo{nullptr};

const wchar_t* ScopeText() noexcept
{
    return primary_controller::Is(generation::PlayerTwoController()) ? L"in player 2's primary scope"
                                                                      : L"outside player 2's scope";
}

// The game's own call at this site, passed through unguarded on purpose (no CallGuarded): the game makes it unguarded
// too. CallGuarded is for the extra calls the mod adds (member_movement's RiddenTest, PlayerCoreAsPlayerTwo).
std::uint8_t VaultCoreTest(const void* character) noexcept
{
    const auto engine = reinterpret_cast<CoreFn>(g_coreCall.Original());
    if (!generation::OnGameThread() || !generation::Active() || character == nullptr)
    {
        return engine(character);
    }
    const bool chocobo = character == chocobo_ride::DrivenChocobo() || character == chocobo_ride::RiddenChocobo();
    const bool member  = character == generation::Member();
    if (!chocobo)
    {
        const auto answer = engine(character);
        if (member)
        {
            platform::Log(L"vault camera: player 2's member vaults on foot; the game's player-core test answers {} ({}){}",
                          answer, ScopeText(),
                          answer != 0 ? L": a vault camera goes on player 2's camera; its end deactivates it there (lever L2)"
                                      : L"");
        }
        else if (character == ReadPointer(generation::PlayerOneController(), facts::kControllerPawn))
        {
            platform::LogDebug(L"vault camera: player 1's pawn vaults; the game's player-core test answers {} ({})", answer,
                               ScopeText());
        }
        return answer;
    }
    const auto answer = member_movement::WithoutChocoboAnswer(engine, character);
    platform::Log(L"vault camera: player 2's chocobo vaults ({}); the game's player-core test answers {} without the "
                  L"mod's ride answer for its input component: {}",
                  ScopeText(), answer,
                  answer == 0 ? L"no vault camera, as for player 1's chocobo; both ride framings stay"
                              : L"a vault camera STILL goes on the primary camera (the ride framings may be lost)");
    return answer;
}

std::uintptr_t Research(std::uintptr_t live) noexcept
{
    return g_image ? g_image->ResearchAddress(live) : live;
}

std::uintptr_t Slot15Vtable(const void* op) noexcept
{
    const void* modules = ReadPointer(op, kOperatorModules);
    return modules != nullptr ? Research(VTableOf(ReadPointer(modules, kModuleSlot15))) : 0;
}

/// The vault End's deactivate (lever L2): the stub hands over the action from rbx. Any thread; the game's own End runs
/// here on the animation worker. Player 2's member's or chocobo's vault camera is ended on player 2's camera, where its
/// Begin put it (in player 2's primary scope); every other vault ends as the game ends it, on the primary camera.
void EndDeactivate(std::uint8_t slot, const void* action) noexcept
{
    const auto primary = reinterpret_cast<PrimaryDeactivateFn>(g_endCall.Original());
    const auto weak    = ReadField<std::uint64_t>(action, kActionCharacter);
    const bool member  = weak != 0 && weak == g_memberWeak.load(std::memory_order_acquire);
    const bool chocobo = !member && weak != 0 && weak == g_chocoboWeak.load(std::memory_order_acquire);
    // Active() first: a map-load leave withdraws the generation before its teardown, in the same game-thread call,
    // ahead of the next Tick that clears the cached operator. A camera manager destroyed by that teardown is only freed
    // by a later garbage collection, which runs on the game thread outside the frame's animation work.
    void* two = generation::Active() ? g_operatorTwo.load(std::memory_order_acquire) : nullptr;
    if ((!member && !chocobo) || two == nullptr || g_operatorDeactivate == nullptr)
    {
        primary(slot);
        try
        {
            platform::LogDebug(L"vault camera: a vault ends ({} thread): the game's deactivate of the primary camera's "
                               L"slot {}",
                               generation::OnGameThread() ? L"game" : L"animation", slot);
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
    const bool tableHeld = platform::CopyGuarded(&table, static_cast<const std::byte*>(two) + kOperatorModules,
                                                 sizeof(table)) &&
                           table != nullptr;
    const auto before = tableHeld ? Slot15Vtable(two) : 0;
    const bool done   = tableHeld && platform::CallGuarded(g_operatorDeactivate, two, slot);
    try
    {
        if (done)
        {
            platform::Log(L"vault camera: player 2's {} vault ends ({} thread): player 2's camera's slot {} deactivated "
                          L"(module vt 0x{:X}), where its Begin put it; player 1's camera untouched",
                          member ? L"member's" : L"chocobo's", generation::OnGameThread() ? L"game" : L"animation", slot,
                          before);
        }
        else
        {
            platform::Log(L"vault camera: player 2's {} vault ends ({} thread): player 2's camera's slot {} NOT "
                          L"deactivated: {}",
                          member ? L"member's" : L"chocobo's", generation::OnGameThread() ? L"game" : L"animation", slot,
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
    const auto handler = reinterpret_cast<std::uintptr_t>(&EndDeactivate);
    return platform::PlaceCodeNear(site, 3 + 6 + 8, [handler](std::uintptr_t, std::span<std::uint8_t> code) noexcept {
        static constexpr std::uint8_t kMoveAndJump[] = {0x48, 0x89, 0xDA,                     // mov rdx, rbx
                                                        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};  // jmp [rip+0]
        std::memcpy(code.data(), kMoveAndJump, sizeof(kMoveAndJump));
        std::memcpy(code.data() + sizeof(kMoveAndJump), &handler, sizeof(handler));
        return true;
    });
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    g_image.emplace(image);
    const auto site = image.Rebase(kCoreCall, 5);
    const auto core = platform::FindFunction(image, facts::kIsPlayerCore);
    const bool done = site && core && g_coreCall.Install(*site, *core, reinterpret_cast<std::uintptr_t>(&VaultCoreTest));
    platform::Log(L"vault camera: {}", done ? L"installed (player 2's chocobo vaults without a vault camera, as player 1's "
                                              L"does; each vault by player 2 logged)"
                                            : L"UNAVAILABLE (the vault's call differs; player 2's chocobo vault can still "
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
                  ended ? L"the vault's end installed (player 2's vault cameras end on player 2's camera, as the game "
                          L"ends player 1's on his; each logged)"
                        : L"the vault's end UNAVAILABLE (its call or the action register differs; player 2's on-foot "
                          L"vault camera stays on player 2's camera, and its end clears player 1's slot 15)");
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
}

void Tick() noexcept
{
    static const void* member  = nullptr;
    static const void* chocobo = nullptr;
    if (!generation::OnGameThread())
    {
        return;
    }
    if (!generation::Active())
    {
        g_operatorTwo.store(nullptr, std::memory_order_release);
        g_memberWeak.store(0, std::memory_order_release);
        g_chocoboWeak.store(0, std::memory_order_release);
        member = chocobo = nullptr;
        return;
    }
    const void* nowMember  = generation::Member();
    const void* nowChocobo = chocobo_ride::DrivenChocobo() != nullptr ? chocobo_ride::DrivenChocobo()
                                                                      : chocobo_ride::RiddenChocobo();
    if (nowMember != member)
    {
        member = nowMember;
        g_memberWeak.store(leader_scope::WeakOf(const_cast<void*>(nowMember)), std::memory_order_release);
    }
    if (nowChocobo != chocobo)
    {
        chocobo = nowChocobo;
        g_chocoboWeak.store(leader_scope::WeakOf(const_cast<void*>(nowChocobo)), std::memory_order_release);
    }
    // The operator as the game's PrimaryPCM_FieldOp_DeactivateModule loads it (*(PCM+0x2CB0)), and only with its module
    // table in place: the game's DeactivateModule reads the table unchecked.
    void* op = ReadPointer(ReadPointer(generation::PlayerTwoController(), facts::kControllerCameraManager), kFieldOperator);
    g_operatorTwo.store(op != nullptr && ReadPointer(op, kOperatorModules) != nullptr ? op : nullptr,
                        std::memory_order_release);
}

}  // namespace lss::player_two::vault_camera
