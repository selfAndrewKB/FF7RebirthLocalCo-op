#include "player_two/pad_sticks.h"

#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"
#include "player_two/scope_leaks.h"
#include "player_two/stick_select.h"

#include <array>
#include <atomic>
#include <cstring>
#include <optional>

namespace lss::player_two::pad_sticks {

namespace {

/// AEndPlayerController::ProcessPlayerInput(DeltaTime, bGamePaused).
using ProcessInputFn = void (*)(void* controller, float deltaTime, bool gamePaused);

// Set once at install.
platform::VTableEntry     g_inputSlot;
ProcessInputFn            g_engineProcessInput = nullptr;
const std::byte* const*   g_inputOwner         = nullptr;  // the global that holds Square's input object
const std::uint8_t*       g_directInputOnly    = nullptr;  // nonzero: the game merges only DirectInput

std::atomic<bool>     g_enabled{false};
std::atomic<unsigned> g_mask{kPartPlayerTwoInput | kPartPlayerOneMove | kPartMemberTick};
bool                  g_scopeOpen = false;  // game thread only

// Per controller id, set by control scripts; game thread only.
constexpr std::int32_t kOverridePads = 4;
std::array<std::optional<Sticks>, kOverridePads> g_overrides;

StageCounters g_inputCounters;
StageCounters g_moveCounters;

std::byte* InputObject() noexcept
{
    return g_inputOwner == nullptr ? nullptr : const_cast<std::byte*>(*g_inputOwner);
}

/// The banks the game's merge reads: DirectInput alone, or every other bank.
std::array<StickBank, 3> MergedBanks(const std::byte* input, std::size_t& count) noexcept
{
    const auto bank = [input](std::uint32_t index) {
        const auto* header = input + facts::kInputDeviceBanks + index * facts::kInputBankHeaderSize;
        return StickBank{ReadField<const float*>(header, 0), ReadField<std::int32_t>(header, sizeof(void*))};
    };
    if (*g_directInputOnly != 0)
    {
        count = 1;
        return {bank(facts::kDirectInputBank)};
    }
    count = 3;
    return {bank(0), bank(facts::kXInputBank), bank(3)};
}

void ProcessPlayerInput(void* controller, float deltaTime, bool gamePaused)
{
    const bool isOne = controller == generation::PlayerOneController();
    const bool isTwo = controller == generation::PlayerTwoController();
    const scope_leaks::InputScope leaks(isTwo);  // player 2's input writes the leader transform; put back after
    if (!g_enabled.load(std::memory_order_relaxed) || !generation::Active() || (!isOne && !isTwo) ||
        (isTwo && !PartActs(kPartPlayerTwoInput)) || !generation::OnGameThread())
    {
        g_inputCounters.Pass();
        g_engineProcessInput(controller, deltaTime, gamePaused);
        return;
    }
    const StickScope sticks(ControllerIdOf(controller));
    if (!isTwo)
    {
        sticks.Held() ? g_inputCounters.Act() : g_inputCounters.Refuse();
        g_engineProcessInput(controller, deltaTime, gamePaused);
        return;
    }
    // Player 2's input also sends camera events to "the primary" camera manager.
    const primary_controller::Scope primary;
    sticks.Held() && primary.Held() ? g_inputCounters.Act() : g_inputCounters.Refuse();
    g_engineProcessInput(controller, deltaTime, gamePaused);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto owner    = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    const auto selector = image.Rebase(facts::kDirectInputOnlyFlag);
    const auto engineFn = platform::FindFunction(image, facts::kProcessPlayerInput.target);
    if (!owner || !selector || !engineFn || !primary_controller::Installed())
    {
        return false;
    }
    g_inputOwner         = reinterpret_cast<const std::byte* const*>(*owner);
    g_directInputOnly    = reinterpret_cast<const std::uint8_t*>(*selector);
    g_engineProcessInput = reinterpret_cast<ProcessInputFn>(*engineFn);
    return platform::ReplaceSlot(image, facts::kProcessPlayerInput,
                                 reinterpret_cast<std::uintptr_t>(&ProcessPlayerInput), g_inputSlot);
}

bool Installed() noexcept
{
    return g_engineProcessInput != nullptr;
}

void Remove() noexcept
{
    if (!g_inputSlot.Restore())
    {
        platform::Log(L"could not restore the controller-input slot; the replacement stays in place");
    }
}

bool Toggle() noexcept
{
    const bool on = Installed() && !g_enabled.load(std::memory_order_relaxed);
    g_enabled.store(on, std::memory_order_relaxed);
    return on;
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

void SetMask(unsigned mask) noexcept
{
    g_mask.store(mask, std::memory_order_relaxed);
    platform::Log(L"per-pad sticks: mask {} (player 2's input as primary {}, player 1's move state on pad 1 {}, "
                  L"player 2's member ticked {})",
                  mask, PartActs(kPartPlayerTwoInput), PartActs(kPartPlayerOneMove), PartActs(kPartMemberTick));
}

bool PartActs(unsigned part) noexcept
{
    return (g_mask.load(std::memory_order_relaxed) & part) != 0;
}

std::int32_t ControllerIdOf(const void* controller) noexcept
{
    const void* localPlayer = ReadPointer(controller, facts::kControllerPlayer);
    return localPlayer == nullptr ? -1 : ReadField<std::int32_t>(localPlayer, facts::kLocalPlayerControllerId);
}

void SetStickOverride(std::int32_t controllerId, const Sticks* sticks) noexcept
{
    if (controllerId < 0 || controllerId >= kOverridePads)
    {
        return;
    }
    auto& slot = g_overrides[static_cast<std::size_t>(controllerId)];
    slot       = sticks == nullptr ? std::nullopt : std::optional<Sticks>(*sticks);
}

StickScope::StickScope(std::int32_t controllerId) noexcept
{
    std::byte* input = InputObject();
    if (g_scopeOpen || input == nullptr || controllerId < 0 || !generation::OnGameThread())
    {
        return;
    }
    std::memcpy(saved_, input + facts::kInputMergedSticks, sizeof(saved_));
    Sticks own{};
    if (controllerId < kOverridePads && g_overrides[static_cast<std::size_t>(controllerId)])
    {
        own = *g_overrides[static_cast<std::size_t>(controllerId)];
    }
    else
    {
        std::size_t count = 0;
        const auto  banks = MergedBanks(input, count);
        const Sticks merged{saved_[0], saved_[1], saved_[2], saved_[3]};
        own = SelectPadSticks(std::span<const StickBank>(banks.data(), count), controllerId, merged);
    }
    std::memcpy(input + facts::kInputMergedSticks, own.data(), sizeof(saved_));
    g_scopeOpen = true;
    held_       = true;
}

StickScope::~StickScope()
{
    if (!held_)
    {
        return;
    }
    std::memcpy(InputObject() + facts::kInputMergedSticks, saved_, sizeof(saved_));
    g_scopeOpen = false;
}

StageCounters& InputCounters() noexcept
{
    return g_inputCounters;
}

StageCounters& MoveCounters() noexcept
{
    return g_moveCounters;
}

}  // namespace lss::player_two::pad_sticks
