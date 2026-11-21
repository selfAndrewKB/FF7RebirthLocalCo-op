#pragma once

// Square Enix's text block (SEndTextBlock), the Slate widget behind every game text, as the mod's per-player states
// read and move it (research/RE-p2-minimap-cost.md, RE-per-player-icons.md).
//
// A block lays its text out once (its relayout: lines, runs and the laid-out quads, button tags turned into the art
// of its font's device bytes) and paints the quads until the next relayout. hud/label_states keeps a player's own
// laid-out state of a shared block; this module is that book's engine side: which widgets can carry such a state, the
// exchange of the state's ranges, the game's own destructor helpers and reference counts, the game's relayout, and
// the game's set of every live block. Found once (Install, idempotent); every call on the game thread (the blocks'
// reference counts are not thread-safe).

#include "hud/label_states.h"
#include "platform/guarded_read.h"
#include "platform/sparse_array.h"

#include <cstddef>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::hud::text_blocks {

// The block's fields read outside the swapped ranges.
inline constexpr std::uint32_t kText          = label_states::kTextOffset;  // FString {TCHAR* data, int32 Num, int32 Max}
inline constexpr std::uint32_t kTextNum       = kText + 8;
inline constexpr std::uint32_t kFont          = 0x2F0;  // UEndFont*: the button tags read its device bytes
inline constexpr std::uint32_t kButtonFont    = 0x2F8;
inline constexpr std::uint32_t kGetterBound   = 0x2E8;  // the text attribute's getter (0: unbound)
inline constexpr std::uint32_t kAllowMacro    = 0x4B0;  // TAttribute<bool>: tags are resolved only when set
inline constexpr std::uint32_t kAutoButtons   = 0x4F8;  // TAttribute<bool>: re-laid out on the game's device change
inline constexpr std::uint32_t kWrapWidth     = 0x560;  // the width the layout was made for
inline constexpr std::uint32_t kRelayoutLock  = 0x564;  // set: a relayout only marks itself pending
inline constexpr std::uint32_t kPending       = label_states::kFlagsOffset;      // a relayout waits for the block's Tick
inline constexpr std::uint32_t kButtonText    = label_states::kFlagsOffset + 2;  // the text held a button tag
inline constexpr std::uint32_t kLanguageStamp = 0x568;  // FName copied from a global (the language)

/// Finds the block's class, its destructor helpers, the reference release, Invalidate, the relayout and the live set.
/// False (logged by the caller) if any is not what the research read. Idempotent.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// The block's MakeShared reference controller (just before the widget).
void* ControllerOf(void* block) noexcept;

/// Why `block` cannot carry a player's own state (not an SEndTextBlock, not in its own MakeShared block, no shared
/// reference left), or null when it can.
const wchar_t* Unswappable(void* block) noexcept;

/// The block whose controller this is still has a shared reference (the mod holds a weak one).
bool Alive(void* controller) noexcept;

/// Exchanges the block's text and layout with `parked` (SwapState), or its layout alone, the text left (SwapLayout).
/// Every read comes before any write: false (nothing changed) if one faulted.
bool SwapState(void* block, label_states::Parked& parked) noexcept;
bool SwapLayout(void* block, label_states::Parked& parked) noexcept;

/// The game's SWidget::Invalidate with the reason its own text setters pass after a relayout.
void Invalidate(void* block) noexcept;

/// What a set-aside layout depends on outside the swapped ranges: the language stamp and both fonts.
std::uint64_t Stamp(void* block) noexcept;

/// The mod's weak reference on the block's controller: the block's memory stays readable after it dies.
bool HoldWeak(void* controller) noexcept;
void ReleaseWeak(void* controller) noexcept;  // the game's own release (frees the block at zero)

/// The destructor's own helpers on a set-aside state; then it is the empty state again.
void FreeParked(label_states::Parked& parked) noexcept;

/// The game's relayout of `block` (then Invalidate, as its setters do). False if it faulted.
bool Relayout(void* block) noexcept;

/// The game's set of every live block: a block is in it from the end of its constructor to the start of its
/// destructor, so each visited pointer is a live block at the visit. 0 before Install.
std::uintptr_t LiveSet() noexcept;
inline constexpr std::uint32_t kLiveSetElement = 16;  // the block's pointer first

/// Calls `visit(block)` for every live block until it returns false. May throw what `visit` throws.
template <class Visit>
void ForEachLive(Visit&& visit)
{
    platform::ForEachSparseElement(LiveSet(), kLiveSetElement, [&](std::uintptr_t element) {
        void* block = nullptr;
        return !platform::Read(reinterpret_cast<const void*>(element), 0, block) || block == nullptr || visit(block);
    });
}

}  // namespace lss::hud::text_blocks
