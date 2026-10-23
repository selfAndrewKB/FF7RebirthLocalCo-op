#pragma once

// Player 2's layer entry beside the game's own in one menu-renderer slot: the root overlay's pool 2 (hud/passes/
// second_image.h) and the navimap circle's pool 0 (hud/minimap/minimap.h), one instance each. The engine keeps one
// image per slot, so the mod reuses the slot across the two passes of the overlay render (hud/passes/overlay_pass.h):
//   - player 2's pass acquires the slot and holds it; the entry it adds is named for player 2; its submit is skipped;
//   - player 1's pass gets the held slot back instead of a second acquire, adds the stock entry and submits both;
//   - a slot still held after player 1's pass (its render returned early) is submitted on its own, as the game's
//     pass would have: an acquired slot left unsubmitted hangs the next acquire.
// Player 2's entry is drawn from its own vertex and index buffers (hud/passes/menu_buffers.h), built as the renderer's
// ctor builds the pool's and published to the render-thread code (hud/passes/entry_filter_code.h) once the render
// thread has initialised them: that thread runs up to a frame behind, and a slot holding player 2's entry must not be
// submitted before (seen: "Tried to call DrawIndexedPrimitive with null IndexBuffer!"). The names are built once from
// the game's own and checked against the engine's layout.
//
// The decisions are hud_rules (SlotActionFor, PlayerTwoEntry), tested on the host. Game thread, except that the
// render thread reads Fields() through its address (this DLL is pinned by its users).

#include "hud/passes/entry_filter_code.h"
#include "hud/passes/menu_buffers.h"
#include "hud/passes/overlay_pass.h"

#include <atomic>
#include <cstdint>

namespace lss::hud {

using AcquireLayerSlotFn = void* (*)(void* renderer, std::uintptr_t pool);
using SubmitLayerSlotFn  = void (*)(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused);

/// What the slot did, for a status line (hud/minimap/minimap.cpp logs its deltas).
struct SlotCounts
{
    std::uint64_t held          = 0;  // player 2's pass held its acquire
    std::uint64_t takenBack     = 0;  // player 1's pass took the held slot
    std::uint64_t named         = 0;  // entries named for player 2
    std::uint64_t submitSkipped = 0;  // submits left to player 1's pass
    std::uint64_t safetySubmits = 0;  // held slots submitted on their own
};

class SharedLayerSlot
{
public:
    struct Setup
    {
        const wchar_t* label;          // the log's prefix ("HUD image", "minimap")
        std::uintptr_t pool;           // the renderer's pool the widget paints into
        std::uint32_t  vertexBuffer;   // the pool's buffer objects in the renderer (hud/passes/menu_buffers.h)
        std::uint32_t  indexBuffer;
        const wchar_t* stockName;      // the game's entry name
        const wchar_t* playerTwoName;  // player 2's
    };

    explicit SharedLayerSlot(Setup setup) noexcept : setup_(setup) {}

    /// At install: the buffer vtables the pool's own buffers carry, the queue to the render thread, and the submit
    /// for a slot left held.
    void Install(std::uintptr_t vertexVtable, std::uintptr_t indexVtable, BeginInitResourceFn beginInit,
                 SubmitLayerSlotFn submit) noexcept;

    /// At removal: the render-thread code sees no player-2 entry any more.
    void Unpublish() noexcept;

    /// Builds player 2's name and checks both against the engine's layout, once the game's name value is known.
    /// False: an FName built by the mod does not match the engine's, or could not be made (the caller refuses).
    bool MakeNames(std::uint64_t stockName) noexcept;
    bool NamesReady() const noexcept { return playerTwoName_ != 0; }

    enum class Buffers
    {
        Waiting,    // queued, or not yet initialised by the render thread (checked every frame)
        Usable,     // initialised and published
        WrongKind,  // the pool's buffers are not the renderer's buffer kind the research read (the caller refuses)
    };
    /// Builds player 2's buffers once the renderer is known (from any acquire), then waits for the render thread.
    Buffers BuffersUsable() noexcept;

    /// Before the overlay render: whether this slot takes part in player 2's pass. Nothing is held.
    void BeginFrame(bool active) noexcept;

    /// The widget's acquire, in any pass (hud_rules::SlotActionFor; off the game thread: the game's own).
    void* Acquire(void* renderer, std::uintptr_t pool, AcquireLayerSlotFn original) noexcept;

    /// The widget's entry name for `slot`: player 2's for the held slot in player 2's pass, else `stockName`.
    std::uintptr_t NameFor(const void* slot, std::uintptr_t stockName) noexcept;

    /// Whether the widget's submit of `slot` is left to player 1's pass.
    bool SkipSubmit(const void* slot) noexcept;

    /// After player 1's pass: submits a slot it did not take, with player 2's entry alone. True if it did.
    bool SubmitIfLeft() noexcept;

    bool               Held() const noexcept { return held_ != nullptr; }
    EntryFilterFields& Fields() noexcept { return fields_; }
    const SlotCounts&  Counts() const noexcept { return counts_; }
    std::uint64_t      OffThreadAcquires() const noexcept { return offThread_.load(std::memory_order_relaxed); }

private:
    Setup setup_;

    // Read by the render-thread code through their addresses.
    EntryFilterFields fields_;
    MenuBufferSet     buffers_;  // player 2's

    std::uintptr_t      vertexVtable_ = 0;
    std::uintptr_t      indexVtable_  = 0;
    BeginInitResourceFn beginInit_    = nullptr;
    SubmitLayerSlotFn   submit_       = nullptr;

    std::uint64_t playerTwoName_ = 0;
    bool          buffersQueued_ = false;
    bool          wrongKind_     = false;
    bool          active_        = false;
    void*         renderer_      = nullptr;  // from any acquire; the renderer is a long-lived singleton
    void*         held_          = nullptr;  // player 2's pass's slot, for player 1's pass
    SlotCounts    counts_;
    std::atomic<std::uint64_t> offThread_{0};
};

}  // namespace lss::hud
