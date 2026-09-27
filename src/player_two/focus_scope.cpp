#include "player_two/focus_scope.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::player_two::focus_scope {

namespace {

// Facts read from the database (RE-p2-command-menu-input.md §2, §3.3, §10; this build).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kInput       = 0x10;  // EndUISystem[2]: Square's input object
constexpr std::uint32_t  kController  = 0xB8;  // EndUISystem[23]: the command controller
// The input object's focus context, as {offset, size} copied in this order: the focus path
// (FWeakWidgetPath), the focus layers (TArray), the last focus cause.
struct Region
{
    std::uint32_t offset, size;
};
constexpr std::array<Region, 3> kContext{{{0x08, 32}, {0x40, 16}, {0x78, 1}}};
constexpr std::uint32_t kDelegate = 0x1A0;  // the menu-core delegate, run when no widget takes an action
// The capture map (a TSet keyed by action): empty when these two counts are equal.
constexpr std::uint32_t kCaptureCount = 0xB0;
constexpr std::uint32_t kCaptureFree  = 0xDC;

constexpr std::size_t kContextSize = 49;

// Game thread.
std::uintptr_t                         g_endUISystem = 0;
std::array<std::uint8_t, kContextSize> g_playerTwo{};  // player 2's context between scopes (zero at first)
bool                                   g_inside      = false;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return address != 0 && platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

bool CopyContext(std::uintptr_t input, std::uint8_t* out) noexcept
{
    std::size_t at = 0;
    for (const auto& region : kContext)
    {
        if (!platform::CopyGuarded(out + at, reinterpret_cast<const void*>(input + region.offset), region.size))
        {
            return false;
        }
        at += region.size;
    }
    return true;
}

bool WriteContext(std::uintptr_t input, const std::uint8_t* from) noexcept
{
    std::size_t at = 0;
    for (const auto& region : kContext)
    {
        if (!platform::WriteGuarded(reinterpret_cast<void*>(input + region.offset), from + at, region.size))
        {
            return false;
        }
        at += region.size;
    }
    return true;
}

}  // namespace

static_assert(kContextSize == 32 + 16 + 1);

bool Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(kEndUISystem, kController + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    return system.has_value();
}

Summary PlayerTwoContext() noexcept
{
    Summary summary;
    std::memcpy(&summary.pathCount, &g_playerTwo[8], sizeof(std::int32_t));        // path: Widgets.Num
    std::memcpy(&summary.layerCount, &g_playerTwo[32 + 8], sizeof(std::int32_t));  // layers: Num
    return summary;
}

FocusScope::FocusScope(void* controller) noexcept
{
    std::int32_t captures = 0, free = 0;
    if (g_inside || !generation::OnGameThread())
    {
        refusal_ = L"inside another focus scope, or off the game thread";
    }
    else if (g_endUISystem == 0 || !Read(g_endUISystem + kInput, input_) || input_ == 0)
    {
        refusal_ = L"no input object";
    }
    else if (!Read(input_ + kCaptureCount, captures) || !Read(input_ + kCaptureFree, free) || captures != free)
    {
        refusal_ = L"player 1's capture map holds a prompt";
    }
    else if (!CopyContext(input_, savedContext_.data()) ||
             !platform::CopyGuarded(savedDelegate_.data(), reinterpret_cast<const void*>(input_ + kDelegate),
                                    savedDelegate_.size()) ||
             !Read(g_endUISystem + kController, savedController_))
    {
        refusal_ = L"player 1's context could not be read";
    }
    if (refusal_ != nullptr)
    {
        return;
    }
    const std::array<std::uint8_t, kDelegateSize> unbound{};
    const auto                                    slot = reinterpret_cast<void*>(g_endUISystem + kController);
    if (!WriteContext(input_, g_playerTwo.data()) ||
        !platform::WriteGuarded(reinterpret_cast<void*>(input_ + kDelegate), unbound.data(), unbound.size()) ||
        !platform::WriteGuarded(slot, &controller, sizeof(controller)))
    {
        // Put back whatever was written: the bytes were just read from the same places.
        const bool back =
            WriteContext(input_, savedContext_.data()) &&
            platform::WriteGuarded(reinterpret_cast<void*>(input_ + kDelegate), savedDelegate_.data(), savedDelegate_.size()) &&
            platform::WriteGuarded(slot, &savedController_, sizeof(savedController_));
        refusal_ = back ? L"player 2's context could not be written"
                        : L"player 2's context could not be written, NOR player 1's put back (player 1's UI focus may be wrong)";
        return;
    }
    entered_ = true;
    g_inside = true;
}

FocusScope::~FocusScope()
{
    if (!entered_)
    {
        return;
    }
    g_inside = false;
    bool kept = true;
    if (abandon_)
    {
        g_playerTwo.fill(0);
        platform::Log(L"focus scope: player 2's context abandoned after a fault (reset to empty)");
    }
    else
    {
        kept = CopyContext(input_, g_playerTwo.data());
    }
    const bool back = WriteContext(input_, savedContext_.data()) &&
                      platform::WriteGuarded(reinterpret_cast<void*>(input_ + kDelegate), savedDelegate_.data(),
                                             savedDelegate_.size()) &&
                      platform::WriteGuarded(reinterpret_cast<void*>(g_endUISystem + kController), &savedController_,
                                             sizeof(savedController_));
    if (!kept || !back)
    {
        platform::Log(L"focus scope: FAILED to {} (player 1's UI focus may be wrong)",
                      !back ? L"restore player 1's context" : L"keep player 2's context");
    }
}

}  // namespace lss::player_two::focus_scope
