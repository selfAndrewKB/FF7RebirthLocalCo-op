#include "input/pad_sticks.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "input/pad_buttons.h"
#include "input/stick_select.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "scopes/primary_controller.h"
#include "scopes/scope_leaks.h"
#include "scopes/seat_ladder.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
#include <atomic>
#include <cstring>
#include <optional>

namespace lss::input::pad_sticks {

namespace seat_ladder = scopes::seat_ladder;

namespace {

/// AEndPlayerController::ProcessPlayerInput(DeltaTime, bGamePaused).
using ProcessInputFn = void (*)(void* controller, float deltaTime, bool gamePaused);

// Set once at install.
platform::VTableEntry     g_inputSlot;
ProcessInputFn            g_engineProcessInput = nullptr;
const std::byte* const*   g_inputOwner         = nullptr;  // the global that holds Square's input object
const std::uint8_t*       g_directInputOnly    = nullptr;  // nonzero: the game merges only DirectInput

std::atomic<bool>     g_enabled{false};
std::atomic<bool>     g_stoodDown{false};  // SetStoodDown
std::atomic<unsigned> g_mask{kPartPlayerTwoInput | kPartPlayerOneMove | kPartMemberTick | kPartPlayerOneDetect};
bool                  g_scopeOpen = false;  // game thread only

// Per controller id, set by control scripts; game thread only.
constexpr std::int32_t kOverridePads = 4;
std::array<std::optional<Sticks>, kOverridePads> g_overrides;

players::StageCounters g_inputCounters;
players::StageCounters g_moveCounters;

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
    // The seat the controller is (player 1's is 0 only while a seat's player is in; a seat's while recorded).
    const int  seat   = players::generation::SeatOfController(controller);
    const bool isOne  = seat == 0;
    const bool isSeat = players::SeatBuilt(seat) && players::generation::Active(seat);
    const scopes::scope_leaks::InputScope leaks(isSeat);  // a seat's input writes the leader transform; put back after
    if (!Enabled() || (!isOne && !isSeat) || (isSeat && !PartActs(kPartPlayerTwoInput)) ||
        !players::generation::OnGameThread())
    {
        g_inputCounters.Pass();
        g_engineProcessInput(controller, deltaTime, gamePaused);
        return;
    }
    // The pad's own sticks; for a seat also the primary controller (its input sends camera events to "the primary"
    // camera manager) and its pad's own buttons (ResetCamera). The call runs with whatever opened; a refused stick or
    // primary scope is counted.
    seat_ladder::Args args;
    args.seat  = isSeat ? seat : players::kPlayerTwoSeat;  // player 1's call opens neither Primary nor Buttons
    args.padId = ControllerIdOf(controller);
    const unsigned which = isSeat ? seat_ladder::Sticks | seat_ladder::Primary | seat_ladder::Buttons
                                  : seat_ladder::Sticks;
    const seat_ladder::P2Scope scopes(which, which, args);
    const bool acted = scopes.Holds<seat_ladder::SticksPart>() && (!isSeat || scopes.Holds<seat_ladder::PrimaryPart>());
    acted ? g_inputCounters.Act() : g_inputCounters.Refuse();
    g_engineProcessInput(controller, deltaTime, gamePaused);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto owner    = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    const auto selector = image.Rebase(facts::kDirectInputOnlyFlag);
    const auto engineFn = platform::FindFunction(image, facts::kProcessPlayerInput.target);
    if (!owner || !selector || !engineFn || !scopes::primary_controller::Installed())
    {
        return false;
    }
    g_inputOwner         = reinterpret_cast<const std::byte* const*>(*owner);
    g_directInputOnly    = reinterpret_cast<const std::uint8_t*>(*selector);
    g_engineProcessInput = reinterpret_cast<ProcessInputFn>(*engineFn);
    const bool replaced = platform::ReplaceSlot(image, facts::kProcessPlayerInput,
                                                reinterpret_cast<std::uintptr_t>(&ProcessPlayerInput), g_inputSlot);
    g_enabled.store(replaced, std::memory_order_relaxed);  // on from the start; Ctrl+F9 switches it for testing
    return replaced;
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
    return g_enabled.load(std::memory_order_relaxed) && !g_stoodDown.load(std::memory_order_relaxed);
}

bool SwitchedOn() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

void SetStoodDown(bool on) noexcept
{
    if (g_stoodDown.exchange(on, std::memory_order_relaxed) == on)
    {
        return;
    }
    platform::Log(L"per-pad input {}{}", on ? L"stood down: every pad reaches player 1 (the game's own merge)"
                                            : L"back: each pad drives its own player",
                  g_enabled.load(std::memory_order_relaxed) ? L"" : L" (the per-pad switch is off, so nothing changes now)");
}

void SetMask(unsigned mask) noexcept
{
    g_mask.store(mask, std::memory_order_relaxed);
    platform::Log(L"per-pad sticks: mask {} (player 2's input as primary {}, player 1's move state on pad 1 {}, "
                  L"player 2's member ticked {}, player 1's detection and ridden chocobo on pad 1 {})",
                  mask, PartActs(kPartPlayerTwoInput), PartActs(kPartPlayerOneMove), PartActs(kPartMemberTick),
                  PartActs(kPartPlayerOneDetect));
}

unsigned Mask() noexcept
{
    return g_mask.load(std::memory_order_relaxed);
}

#if LSS_DEV
bool PadTriggers(std::int32_t controllerId, float& left, float& right) noexcept
{
    left = right     = 0.0f;
    std::byte* input = InputObject();
    if (input == nullptr || g_directInputOnly == nullptr || controllerId < 0 || !players::generation::OnGameThread())
    {
        return false;
    }
    std::size_t count = 0;
    const auto  banks = MergedBanks(input, count);
    bool        found = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        float record[6]{};  // LX, LY, RX, RY, LT, RT
        if (banks[i].records == nullptr || controllerId >= banks[i].count ||
            !platform::CopyGuarded(record, banks[i].records + static_cast<std::ptrdiff_t>(controllerId) * 6, sizeof(record)))
        {
            continue;
        }
        left  = record[4] > left ? record[4] : left;
        right = record[5] > right ? record[5] : right;
        found = true;
    }
    return found;
}
#endif

bool PartActs(unsigned part) noexcept
{
    return (g_mask.load(std::memory_order_relaxed) & part) != 0;
}

std::int32_t ControllerIdOf(const void* controller) noexcept
{
    const void* localPlayer = ReadPointer(controller, facts::kControllerPlayer);
    // Read directly, not through ReadField: 0 is player 1's id, so an unreadable field must give -1.
    std::int32_t id = -1;
    if (localPlayer == nullptr ||
        !platform::CopyGuarded(&id, static_cast<const std::byte*>(localPlayer) + facts::kLocalPlayerControllerId, sizeof(id)))
    {
        return -1;
    }
    return id;
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
    Sticks     merged{};
    if (g_scopeOpen || input == nullptr || controllerId < 0 || !players::generation::OnGameThread() ||
        !platform::CopyGuarded(merged.data(), input + facts::kInputMergedSticks, sizeof(merged)))
    {
        return;
    }
    Sticks own{};
    if (controllerId < kOverridePads && g_overrides[static_cast<std::size_t>(controllerId)])
    {
        own = *g_overrides[static_cast<std::size_t>(controllerId)];
    }
    else
    {
        std::size_t count = 0;
        const auto  banks = MergedBanks(input, count);
        own = SelectPadSticks(std::span<const StickBank>(banks.data(), count), controllerId, merged);
    }
    const platform::ByteContext::Range range{reinterpret_cast<std::uintptr_t>(input + facts::kInputMergedSticks),
                                             sizeof(Sticks), platform::ByteContext::Kind::Set};
    if (!context_.Define(std::span<const platform::ByteContext::Range>(&range, 1)))
    {
        return;
    }
    std::memcpy(context_.Value(0).data(), own.data(), sizeof(own));
    held_       = context_.Open();
    g_scopeOpen = held_;
}

StickScope::~StickScope()
{
    if (!held_)
    {
        return;
    }
    if (context_.Close() != 0)
    {
        platform::LogDebug(L"per-pad sticks: the game wrote the merged sticks inside a pad's scope; its values kept");
    }
    g_scopeOpen = false;
}

players::StageCounters& InputCounters() noexcept
{
    return g_inputCounters;
}

players::StageCounters& MoveCounters() noexcept
{
    return g_moveCounters;
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"sticks-mask",
                       .shape = control::Shape::Integer,
                       .max   = 15,
                       .help  = L"diagnosis: which parts of per-pad sticks act (1 player 2's input as the primary "
                                L"player's, 2 player 1's move state on pad 1, 4 player 2's member ticked, 8 player 1's "
                                L"input update and leader-action tick on pad 1); all at start",
                       .set   = [](const Value& v) {
                           SetMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"sticks mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{Mask()}}; }});
}

#endif

}  // namespace lss::input::pad_sticks
