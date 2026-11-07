#pragma once

// A seat's own copy of each minimap label's text and layout, one book per seat (RE-p2-minimap-cost.md §7).
//
// A minimap icon's distance label is one Square Enix text block (SEndTextBlock) shared by every view. The game sets its
// text through SetText, which compares the new text with the block's current text and lays the text out again only when
// it differs. With one block and each player's own distance, every pass changed the text: a relayout per pass each
// frame (~0.95 ms each: the measurer copies the font's whole glyph table per line). Here the block keeps one state per
// player instead:
// - in the seat's holder run, just before the game's SetText on a block, the seat's last state is swapped into it
//   (player 1's is set aside). The game's own compare then finds the seat's text and skips the relayout, or, when
//   the seat's value changed, the game's relayout writes the seat's new state;
// - after the seat's paint, player 1's state goes back in (the seat's is set aside until the next pass).
// The bytes are the game's own relayout output, moved bitwise as the game itself moves these headers; nothing is laid out
// or drawn by the mod. A copy set aside is freed with the game's own destructor helpers.
//
// This header is the bookkeeping only (records, the bootstrap, the font-reload reset, dead widgets), reached through
// LabelAccess so it is tested on the host. Game thread only: the block's reference counts are not thread-safe.

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::hud::label_states {

// The swapped ranges of an SEndTextBlock (§7.1); everything else stays in the widget.
inline constexpr std::uint32_t kTextOffset    = 0x2C8;  // FString text
inline constexpr std::uint32_t kTextSize      = 0x10;
inline constexpr std::uint32_t kRunsOffset    = 0x530;  // run arrays, desired size, last allotted size and wrap width
inline constexpr std::uint32_t kRunsSize      = 0x34;
inline constexpr std::uint32_t kFlagsOffset   = 0x565;  // relayout pending, glyph-font text, button text
inline constexpr std::uint32_t kFlagsSize     = 3;
inline constexpr std::uint32_t kQuadsOffset   = 0x570;  // four quad arrays and the cached shadow offset
inline constexpr std::uint32_t kQuadsSize     = 0x48;
inline constexpr std::uint32_t kShadowInQuads = 0x40;   // +0x5B0/+0x5B4: 1.0f, 1.0f in an empty block

/// One state set aside, laid out range by range (each array header 8-aligned, as in the widget).
struct alignas(8) Parked
{
    alignas(8) std::byte text[kTextSize];
    alignas(8) std::byte runs[kRunsSize];
    alignas(8) std::byte flags[kFlagsSize];
    alignas(8) std::byte quads[kQuadsSize];
};

/// The state the block's constructor writes: what the game's first relayout starts from (§7.8 A).
Parked EmptyState() noexcept;

/// The engine side. Every function is called on the game thread.
struct LabelAccess
{
    bool (*usable)(void* widget) noexcept;              // the class, its controller, alive, unlocked, no bound getter
    bool (*alive)(void* controller) noexcept;           // the shared count above 0 (the mod holds a weak reference)
    bool (*swap)(void* widget, Parked& parked) noexcept;  // exchanges the ranges; false if it faulted (nothing changed)
    void (*invalidate)(void* widget) noexcept;          // the game's SWidget::Invalidate, as its SetText calls it
    std::uint64_t (*stamp)(void* widget) noexcept;      // changes when the language or a font changes
    bool (*holdWeak)(void* controller) noexcept;        // keeps the block's memory readable after the widget dies
    void (*releaseWeak)(void* controller) noexcept;     // the game's own release (frees the block at zero)
    void (*freeParked)(Parked& parked) noexcept;        // the game's destructor helpers on a set-aside state
};

struct LabelCounts
{
    std::uint64_t swapsIn = 0, swapsOut = 0, created = 0, fontResets = 0, refused = 0, full = 0, faults = 0;
    std::uint64_t diedInside = 0, diedAside = 0, released = 0;
};

class LabelBook
{
public:
    static constexpr std::size_t kMaxLabels = 64;

    explicit LabelBook(LabelAccess access) noexcept : access_(access) {}

    /// In the seat's holder run, before the game's SetText on `widget` (its controller block is `controller`): puts
    /// the seat's state in. False only if the swap faulted (the caller then stops the feature).
    bool BeforeSetText(void* widget, void* controller) noexcept;

    /// After the seat's paint: player 1's state back into every block that has the seat's.
    void SwapBack() noexcept;

    /// Outside the passes: drops the records of widgets that died (their set-aside state freed).
    void Sweep() noexcept;

    /// The seat's minimap off, or the mod leaving: player 1's state in every block, every record freed.
    void ReleaseAll() noexcept;

    std::size_t        Count() const noexcept { return count_; }
    bool               AnyInside() const noexcept;
    /// The seat's state is in the block whose controller is `controller` (logging).
    bool               Holds(const void* controller) const noexcept;
    const LabelCounts& Counts() const noexcept { return counts_; }

private:
    struct Record
    {
        void*         widget     = nullptr;
        void*         controller = nullptr;
        std::uint64_t stamp      = 0;
        bool          inside     = false;  // the seat's state is in the widget, player 1's in `parked`
        Parked        parked{};
    };

    Record* Find(const void* controller) noexcept;
    void    Drop(std::size_t index) noexcept;  // frees the set-aside state and the weak reference

    LabelAccess                       access_;
    std::array<Record, kMaxLabels>    records_{};
    std::size_t                       count_ = 0;
    LabelCounts                       counts_;
};

}  // namespace lss::hud::label_states
