#include "player_two/focus_scope.h"

#include "facts/hud.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::player_two::focus_scope {

namespace {

using facts::kUICommandController;  // the UI system's slots (facts/hud.h)
using facts::kUIInput;
using facts::kUIKeyGuide;

// Facts read from the database (RE-p2-command-menu-input.md §2, §3.3, §10; this build).
// The input object's focus context, as {offset, size} copied in this order: the focus path
// (FWeakWidgetPath), the focus layers (TArray), the last focus cause, and the capture map (a TSet
// keyed by action: the widget that took a held action's press gets its release; the command menu
// adds one for each held Attack, Unique or Guard, so it is per-player state like the path), then the
// menu owner byte (+0x64: written by the member selecter's tick in the field top states, read by
// SquareInput_Owner64Gate 0x143A1D738 in the key-down mapper; per-pad input state tied to that player's
// menu, RE-p2-command-menu-levers.md §5.2).
struct Region
{
    std::uint32_t offset, size;
};
constexpr std::array<Region, 5> kContext{{{0x08, 32}, {0x40, 16}, {0x78, 1}, {0xA8, 0x50}, {0x64, 1}}};
constexpr std::uint32_t kDelegate = 0x1A0;  // the menu-core delegate, run when no widget takes an action

constexpr std::size_t kContextSize = 32 + 16 + 1 + 0x50 + 1;
constexpr std::size_t kCaptureAt   = 32 + 16 + 1;  // the capture map's place in the copy
// An empty TSet as the game constructs one: all zero but its sparse array's FirstFreeIndex (+0x30), -1.
constexpr std::size_t kCaptureFirstFree = 0x30;

/// Player 1's live capture map read against the stock TSet layout the empty copy assumes: sparse array
/// Data {ptr, Num +0x08, Max +0x0C}, allocation bits {inline, secondary, NumBits +0x28, MaxBits +0x2C},
/// FirstFreeIndex +0x30, NumFreeIndices +0x34, HashSize +0x48. False if any count disagrees.
bool CaptureLayoutHolds(std::uintptr_t input) noexcept
{
    struct Counts
    {
        std::int32_t num = 0, max = 0, bits = 0, firstFree = 0, numFree = 0, hashSize = 0;
    } c;
    const auto map  = input + kContext[3].offset;
    const auto read = [map](std::uint32_t at, std::int32_t& out) {
        return platform::CopyGuarded(&out, reinterpret_cast<const void*>(map + at), sizeof(out));
    };
    if (!read(0x08, c.num) || !read(0x0C, c.max) || !read(0x28, c.bits) || !read(0x30, c.firstFree) ||
        !read(0x34, c.numFree) || !read(0x48, c.hashSize))
    {
        return false;
    }
    const bool freeList = c.firstFree == -1 ? c.numFree == 0 : c.firstFree >= 0 && c.firstFree < c.num && c.numFree > 0;
    return c.num >= 0 && c.num <= c.max && c.bits == c.num && c.numFree >= 0 && c.numFree <= c.num && freeList &&
           c.hashSize >= 0 && (c.hashSize & (c.hashSize - 1)) == 0;
}

std::array<std::uint8_t, kContextSize> EmptyContext() noexcept
{
    std::array<std::uint8_t, kContextSize> context{};
    const std::int32_t none = -1;
    std::memcpy(&context[kCaptureAt + kCaptureFirstFree], &none, sizeof(none));
    return context;
}
// The focus layers (TArray of 40 bytes, sorted by priority; int32 priority at +0x20). The command
// menu's own layers sit at -1000 (RE-p2-command-menu-input.md §11).
constexpr std::uint32_t kLayerSize         = 40;
constexpr std::uint32_t kLayerPriority     = 0x20;
constexpr std::int32_t  kCommandMenuLayer  = -1000;

// Game thread.
std::uintptr_t                         g_endUISystem = 0;
std::array<std::uint8_t, kContextSize> g_playerTwo = EmptyContext();  // player 2's context between scopes
bool                                   g_inside      = false;
std::uintptr_t                         g_keyGuide    = 0;  // player 2's key guide, or 0
int                                    g_layout      = 0;  // player 1's capture map checked: 0 not yet, 1 holds, -1 does not

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



bool Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(facts::kEndUISystem, kUIKeyGuide + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    return system.has_value();
}

void SetKeyGuide(void* keyGuide) noexcept
{
    g_keyGuide = reinterpret_cast<std::uintptr_t>(keyGuide);
}

Summary PlayerTwoContext() noexcept
{
    Summary summary;
    std::memcpy(&summary.pathCount, &g_playerTwo[8], sizeof(std::int32_t));        // path: Widgets.Num
    std::memcpy(&summary.layerCount, &g_playerTwo[32 + 8], sizeof(std::int32_t));  // layers: Num
    return summary;
}

std::int32_t LivePathCount() noexcept
{
    std::uintptr_t input = 0;
    std::int32_t   count = 0;
    return g_endUISystem != 0 && Read(g_endUISystem + kUIInput, input) && input != 0 &&
                   Read(input + kContext[0].offset + 8, count)
               ? count
               : 0;
}

bool PlayerOneTopIsCommandMenu() noexcept
{
    std::uintptr_t input = 0, layers = 0;
    std::int32_t   count = 0, priority = 0;
    return !g_inside && Read(g_endUISystem + kUIInput, input) && input != 0 &&
           Read(input + kContext[1].offset, layers) &&
           Read(input + kContext[1].offset + 8, count) && layers != 0 && count > 0 &&
           Read(layers + static_cast<std::uintptr_t>(count - 1) * kLayerSize + kLayerPriority, priority) &&
           priority == kCommandMenuLayer;
}

FocusScope::FocusScope(void* controller) noexcept
{
    if (g_inside || !generation::OnGameThread())
    {
        refusal_ = L"inside another focus scope, or off the game thread";
    }
    else if (g_endUISystem == 0 || !Read(g_endUISystem + kUIInput, input_) || input_ == 0)
    {
        refusal_ = L"no input object";
    }
    else if (g_layout == 0 && (g_layout = CaptureLayoutHolds(input_) ? 1 : -1) < 0)
    {
        platform::Log(L"focus scope: player 1's capture map does not have the TSet layout player 2's copy assumes; "
                      L"player 2's UI stays off");
    }
    if (refusal_ == nullptr && g_layout < 0)
    {
        refusal_ = L"the capture map's layout is not the one assumed";
    }
    else if (refusal_ == nullptr &&
             (!CopyContext(input_, savedContext_.data()) ||
             !platform::CopyGuarded(savedDelegate_.data(), reinterpret_cast<const void*>(input_ + kDelegate),
                                    savedDelegate_.size()) ||
             !Read(g_endUISystem + kUICommandController, savedController_) ||
             !Read(g_endUISystem + kUIKeyGuide, savedKeyGuide_)))
    {
        refusal_ = L"player 1's context could not be read";
    }
    if (refusal_ != nullptr)
    {
        return;
    }
    const std::array<std::uint8_t, kDelegateSize> unbound{};
    const auto                                    slot = reinterpret_cast<void*>(g_endUISystem + kUICommandController);
    if (!WriteContext(input_, g_playerTwo.data()) ||
        !platform::WriteGuarded(reinterpret_cast<void*>(input_ + kDelegate), unbound.data(), unbound.size()) ||
        !platform::WriteGuarded(slot, &controller, sizeof(controller)) ||
        (g_keyGuide != 0 &&
         !platform::WriteGuarded(reinterpret_cast<void*>(g_endUISystem + kUIKeyGuide), &g_keyGuide,
                                 sizeof(g_keyGuide))))
    {
        // Put back whatever was written: the bytes were just read from the same places.
        const bool back =
            WriteContext(input_, savedContext_.data()) &&
            platform::WriteGuarded(reinterpret_cast<void*>(input_ + kDelegate), savedDelegate_.data(), savedDelegate_.size()) &&
            platform::WriteGuarded(slot, &savedController_, sizeof(savedController_)) &&
            platform::WriteGuarded(reinterpret_cast<void*>(g_endUISystem + kUIKeyGuide), &savedKeyGuide_,
                                   sizeof(savedKeyGuide_));
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
        g_playerTwo = EmptyContext();
        platform::Log(L"focus scope: player 2's context abandoned after a fault (reset to empty)");
    }
    else
    {
        kept = CopyContext(input_, g_playerTwo.data());
    }
    const bool back = WriteContext(input_, savedContext_.data()) &&
                      platform::WriteGuarded(reinterpret_cast<void*>(input_ + kDelegate), savedDelegate_.data(),
                                             savedDelegate_.size()) &&
                      platform::WriteGuarded(reinterpret_cast<void*>(g_endUISystem + kUICommandController),
                                             &savedController_, sizeof(savedController_)) &&
                      platform::WriteGuarded(reinterpret_cast<void*>(g_endUISystem + kUIKeyGuide), &savedKeyGuide_,
                                             sizeof(savedKeyGuide_));
    if (!kept || !back)
    {
        platform::Log(L"focus scope: FAILED to {} (player 1's UI focus may be wrong)",
                      !back ? L"restore player 1's context" : L"keep player 2's context");
    }
}

}  // namespace lss::player_two::focus_scope
