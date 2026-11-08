#include "hud/passes/shared_slot.h"

#include "hud/passes/overlay_pass.h"
#include "platform/log.h"
#include "players/generation.h"

namespace lss::hud {

void SharedLayerSlot::Install(std::uintptr_t vertexVtable, std::uintptr_t indexVtable, BeginInitResourceFn beginInit,
                              SubmitLayerSlotFn submit) noexcept
{
    vertexVtable_ = vertexVtable;
    indexVtable_  = indexVtable;
    beginInit_    = beginInit;
    submit_       = submit;
}

void SharedLayerSlot::Unpublish() noexcept
{
    fields_.familyName = 0;
    for (EntryFilterFields::ColumnEntry& entry : fields_.columns)
    {
        entry.name = 0;
    }
}

bool SharedLayerSlot::MakeNames(std::uint64_t stockName) noexcept
{
    try
    {
        if (EngineNameValue(setup_.stockName, RC::Unreal::FNAME_Find) != stockName)
        {
            return false;
        }
        std::uint64_t names[kFurtherColumns] = {};
        for (int i = 0; i < kFurtherColumns; ++i)
        {
            const std::uint64_t name = EngineNameValue(setup_.columnNames[i], RC::Unreal::FNAME_Add);
            if (name == 0 || name == stockName || (i > 0 && name == names[0]))
            {
                return false;
            }
            names[i] = name;
        }
        fields_.stockName = stockName;  // before any family name can be set from it
        for (int i = 0; i < kFurtherColumns; ++i)
        {
            columnNames_[i] = names[i];
        }
        namesReady_ = true;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

SharedLayerSlot::Buffers SharedLayerSlot::BuffersUsable(int column) noexcept
{
    if (wrongKind_)
    {
        return Buffers::WrongKind;
    }
    if (column < 1 || column > kFurtherColumns)
    {
        return Buffers::Waiting;  // not a further column: no entry of its own
    }
    const int      i       = column - 1;
    MenuBufferSet& buffers = buffers_[i];
    if (!buffersQueued_[i])
    {
        if (renderer_ == nullptr)  // no acquire seen yet
        {
            return Buffers::Waiting;
        }
        if (!BuildMenuBuffers(renderer_, setup_.vertexBuffer, setup_.indexBuffer, vertexVtable_, indexVtable_,
                              beginInit_, buffers))
        {
            wrongKind_ = true;
            return Buffers::WrongKind;
        }
        buffersQueued_[i] = true;
        platform::Log(L"{}: column {}'s vertex and index buffers created (capacity {} / {}); waiting for the render "
                      L"thread to initialise them",
                      setup_.label, column + 1, kMenuVertexCapacity, kMenuIndexCapacity);
        return Buffers::Waiting;
    }
    if (!MenuBuffersInitialised(buffers))  // every frame (hud/passes/menu_buffers.h)
    {
        return Buffers::Waiting;
    }
    EntryFilterFields::ColumnEntry& entry = fields_.Column(column);
    if (entry.name == 0)
    {
        entry.vertexBuffer = reinterpret_cast<std::uint64_t>(&buffers.vertex);
        entry.indexBuffer  = reinterpret_cast<std::uint64_t>(&buffers.index);
        entry.srmSlot      = reinterpret_cast<std::uint64_t>(&buffers.srm);
        std::atomic_thread_fence(std::memory_order_release);
        entry.name = columnNames_[i];  // last: the render-thread code switches on this
        platform::Log(L"{}: column {}'s buffers are initialised; rendering its entry", setup_.label, column + 1);
    }
    return Buffers::Usable;
}

void SharedLayerSlot::BeginFrame() noexcept
{
    active_       = false;
    held_         = nullptr;
    takenBy_      = -1;
    entryColumns_ = 0;
}

void* SharedLayerSlot::Acquire(void* renderer, std::uintptr_t pool, AcquireLayerSlotFn original) noexcept
{
    if (!players::generation::OnGameThread())
    {
        offThread_.fetch_add(1, std::memory_order_relaxed);
        return original(renderer, pool);
    }
    renderer_        = renderer;
    const int column = overlay_pass::Column();
    switch (overlay_pass::AcquireAction(column, active_, held_ != nullptr, held_ != nullptr && takenBy_ == column))
    {
        case overlay_pass::SlotAction::TakeBack:
        {
            void* slot = held_;
            held_      = nullptr;
            takenBy_   = 0;
            ++counts_.takenBack;
            return slot;
        }
        case overlay_pass::SlotAction::TakeHeld:
            takenBy_ = column;
            ++counts_.takenHeld;
            return held_;
        case overlay_pass::SlotAction::AcquireAndHold:
            held_    = original(renderer, pool);
            takenBy_ = column;
            ++counts_.held;
            return held_;
        case overlay_pass::SlotAction::Acquire: break;
    }
    return original(renderer, pool);
}

std::uintptr_t SharedLayerSlot::NameFor(const void* slot, std::uintptr_t stockName) noexcept
{
    const int column = overlay_pass::Column();
    if (!overlay_pass::IsColumnEntry(column, slot != nullptr && slot == held_))
    {
        return stockName;
    }
    ++counts_.named;
    entryColumns_ |= 1u << column;
    return static_cast<std::uintptr_t>(columnNames_[column - 1]);
}

bool SharedLayerSlot::SkipSubmit(const void* slot) noexcept
{
    const int column = overlay_pass::Column();
    if (!overlay_pass::IsColumnEntry(column, slot != nullptr && slot == held_))
    {
        return false;
    }
    ++counts_.submitSkipped;
    return true;
}

bool SharedLayerSlot::SubmitIfLeft() noexcept
{
    void* slot = held_;
    held_      = nullptr;
    takenBy_   = -1;
    if (slot == nullptr || renderer_ == nullptr || submit_ == nullptr)
    {
        return false;
    }
    submit_(renderer_, setup_.pool, slot, 0);
    ++counts_.safetySubmits;
    return true;
}

}  // namespace lss::hud
