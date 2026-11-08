#pragma once

// Each further column's layer entry beside the game's own in one menu-renderer slot: the root overlay's pool 2
// (hud/passes/column_hud_images.h) and the navimap circle's pool 0 (hud/minimap/minimap.h), one instance each. The engine
// keeps one image per slot, so the mod reuses the slot across the passes of the overlay render (hud/passes/
// overlay_pass.h, which holds the rule):
//   - the first further pass the slot takes part in acquires it and holds it; each later further pass takes the held
//     slot; each adds an entry named for its column, and their submits are skipped;
//   - column 0's pass gets the held slot back instead of a second acquire, adds the stock entry and submits them all;
//   - a slot still held after column 0's pass (its render returned early) is submitted on its own, as the game's pass
//     would have: an acquired slot left unsubmitted hangs the next acquire.
// Each further column's entry is drawn from its own vertex and index buffers (hud/passes/menu_buffers.h), built as the
// renderer's ctor builds the pool's and published to the render-thread code (hud/passes/entry_filter_code.h) once the
// render thread has initialised them: that thread runs up to a frame behind, and a slot holding a column's entry must
// not be submitted before (seen: "Tried to call DrawIndexedPrimitive with null IndexBuffer!"). The names are built once
// from the game's own and checked against the engine's layout; buffers are built for a column at its first pass.
//
// Game thread, except that the render thread reads Fields() through its address (this DLL is pinned by its users).

#include "hud/passes/entry_filter_code.h"
#include "hud/passes/menu_buffers.h"

#include <atomic>
#include <cstdint>

namespace lss::hud {

using AcquireLayerSlotFn = void* (*)(void* renderer, std::uintptr_t pool);
using SubmitLayerSlotFn  = void (*)(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused);

/// What the slot did, for a status line (hud/minimap/minimap.cpp logs its deltas).
struct SlotCounts
{
    std::uint64_t held          = 0;  // a further pass held its acquire
    std::uint64_t takenHeld     = 0;  // a later further pass took the held slot
    std::uint64_t takenBack     = 0;  // column 0's pass took the held slot
    std::uint64_t named         = 0;  // entries named for a further column
    std::uint64_t submitSkipped = 0;  // submits left to column 0's pass
    std::uint64_t safetySubmits = 0;  // held slots submitted on their own
};

class SharedLayerSlot
{
public:
    struct Setup
    {
        const wchar_t* label;                           // the log's prefix ("HUD image", "minimap")
        std::uintptr_t pool;                            // the renderer's pool the widget paints into
        std::uint32_t  vertexBuffer;                    // the pool's buffer objects in the renderer
        std::uint32_t  indexBuffer;                     //   (hud/passes/menu_buffers.h)
        const wchar_t* stockName;                         // the game's entry name
        const wchar_t* columnNames[kFurtherColumns];      // each further column's, column c at [c - 1]
    };

    explicit SharedLayerSlot(Setup setup) noexcept : setup_(setup) {}

    /// At install: the buffer vtables the pool's own buffers carry, the queue to the render thread, and the submit
    /// for a slot left held.
    void Install(std::uintptr_t vertexVtable, std::uintptr_t indexVtable, BeginInitResourceFn beginInit,
                 SubmitLayerSlotFn submit) noexcept;

    /// At removal: the render-thread code sees no further column's entry any more.
    void Unpublish() noexcept;

    /// Builds every further column's name and checks them and the game's against the engine's layout, once the game's
    /// name value is known. False: an FName built by the mod does not match the engine's, or could not be made (the caller
    /// refuses).
    bool MakeNames(std::uint64_t stockName) noexcept;
    bool NamesReady() const noexcept { return namesReady_; }

    enum class Buffers
    {
        Waiting,    // queued, or not yet initialised by the render thread (checked every frame)
        Usable,     // initialised and published
        WrongKind,  // the pool's buffers are not the renderer's buffer kind the research read (the caller refuses)
    };
    /// Builds the further column's buffers once the renderer is known (from any acquire), then waits for the render
    /// thread. `column` is a further column (1..kMaxColumns-1); any other waits.
    Buffers BuffersUsable(int column) noexcept;

    /// Before the overlay render: nothing held, no further column's entry in the slot.
    void BeginFrame() noexcept;

    /// Before each further pass: whether this slot takes part in it (its widget paints for the pass).
    void SetActive(bool active) noexcept { active_ = active; }

    /// The widget's acquire, in any pass (overlay_pass::AcquireAction; off the game thread: the game's own).
    void* Acquire(void* renderer, std::uintptr_t pool, AcquireLayerSlotFn original) noexcept;

    /// The widget's entry name for `slot`: the pass's column's for the held slot in a further pass, else `stockName`.
    std::uintptr_t NameFor(const void* slot, std::uintptr_t stockName) noexcept;

    /// Whether the widget's submit of `slot` is left to column 0's pass.
    bool SkipSubmit(const void* slot) noexcept;

    /// After column 0's pass: submits a slot it did not take, with the further columns' entries alone. True if it did.
    bool SubmitIfLeft() noexcept;

    bool Held() const noexcept { return held_ != nullptr; }
    /// The further columns whose entries the held slot carries this frame, bit `column` each (read before column 0's
    /// pass).
    unsigned           EntryColumns() const noexcept { return entryColumns_; }
    EntryFilterFields& Fields() noexcept { return fields_; }
    const SlotCounts&  Counts() const noexcept { return counts_; }
    std::uint64_t      OffThreadAcquires() const noexcept { return offThread_.load(std::memory_order_relaxed); }

private:
    Setup setup_;

    // Read by the render-thread code through their addresses.
    EntryFilterFields fields_;
    MenuBufferSet     buffers_[kFurtherColumns];  // column c at [c - 1]

    std::uintptr_t      vertexVtable_ = 0;
    std::uintptr_t      indexVtable_  = 0;
    BeginInitResourceFn beginInit_    = nullptr;
    SubmitLayerSlotFn   submit_       = nullptr;

    std::uint64_t columnNames_[kFurtherColumns]   = {};
    bool          namesReady_                     = false;
    bool          buffersQueued_[kFurtherColumns] = {};
    bool          wrongKind_     = false;
    bool          active_        = false;
    void*         renderer_      = nullptr;  // from any acquire; the renderer is a long-lived singleton
    void*         held_          = nullptr;  // the slot the further passes hold, for column 0's pass
    int           takenBy_       = -1;       // the column whose pass last had the held slot
    unsigned      entryColumns_  = 0;        // bit `column`: an entry named for the column this frame
    SlotCounts    counts_;
    std::atomic<std::uint64_t> offThread_{0};
};

}  // namespace lss::hud
