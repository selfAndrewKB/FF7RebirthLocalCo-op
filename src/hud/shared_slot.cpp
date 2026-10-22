#include "hud/shared_slot.h"

#include "hud/hud_rules.h"
#include "platform/log.h"
#include "player_two/generation.h"

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
    fields_.familyName    = 0;
    fields_.playerTwoName = 0;
}

bool SharedLayerSlot::MakeNames(std::uint64_t stockName) noexcept
{
    try
    {
        const std::uint64_t playerTwo = EngineNameValue(setup_.playerTwoName, RC::Unreal::FNAME_Add);
        if (EngineNameValue(setup_.stockName, RC::Unreal::FNAME_Find) != stockName || playerTwo == 0 ||
            playerTwo == stockName)
        {
            return false;
        }
        fields_.stockName = stockName;  // before any family name can be set from it
        playerTwoName_    = playerTwo;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

SharedLayerSlot::Buffers SharedLayerSlot::BuffersUsable() noexcept
{
    if (wrongKind_)
    {
        return Buffers::WrongKind;
    }
    if (!buffersQueued_)
    {
        if (renderer_ == nullptr)  // no acquire seen yet
        {
            return Buffers::Waiting;
        }
        if (!BuildMenuBuffers(renderer_, setup_.vertexBuffer, setup_.indexBuffer, vertexVtable_, indexVtable_,
                              beginInit_, buffers_))
        {
            wrongKind_ = true;
            return Buffers::WrongKind;
        }
        buffersQueued_ = true;
        platform::Log(L"{}: player 2's vertex and index buffers created (capacity {} / {}); waiting for the render "
                      L"thread to initialise them",
                      setup_.label, kMenuVertexCapacity, kMenuIndexCapacity);
        return Buffers::Waiting;
    }
    if (!MenuBuffersInitialised(buffers_))  // every frame (hud/menu_buffers.h)
    {
        return Buffers::Waiting;
    }
    if (fields_.playerTwoName == 0)
    {
        fields_.vertexBuffer = reinterpret_cast<std::uint64_t>(&buffers_.vertex);
        fields_.indexBuffer  = reinterpret_cast<std::uint64_t>(&buffers_.index);
        fields_.srmSlot      = reinterpret_cast<std::uint64_t>(&buffers_.srm);
        std::atomic_thread_fence(std::memory_order_release);
        fields_.playerTwoName = playerTwoName_;  // last: the render-thread code switches on this
        platform::Log(L"{}: player 2's buffers are initialised; rendering player 2's entry", setup_.label);
    }
    return Buffers::Usable;
}

void SharedLayerSlot::BeginFrame(bool active) noexcept
{
    active_ = active;
    held_   = nullptr;
}

void* SharedLayerSlot::Acquire(void* renderer, std::uintptr_t pool, AcquireLayerSlotFn original) noexcept
{
    if (!player_two::generation::OnGameThread())
    {
        offThread_.fetch_add(1, std::memory_order_relaxed);
        return original(renderer, pool);
    }
    renderer_ = renderer;
    switch (hud_rules::SlotActionFor(CurrentOverlayPass(), active_, held_ != nullptr))
    {
        case hud_rules::SlotAction::TakeBack:
        {
            void* slot = held_;
            held_      = nullptr;
            ++counts_.takenBack;
            return slot;
        }
        case hud_rules::SlotAction::AcquireAndHold:
            held_ = original(renderer, pool);
            ++counts_.held;
            return held_;
        case hud_rules::SlotAction::Acquire: break;
    }
    return original(renderer, pool);
}

std::uintptr_t SharedLayerSlot::NameFor(const void* slot, std::uintptr_t stockName) noexcept
{
    if (!hud_rules::PlayerTwoEntry(CurrentOverlayPass(), slot != nullptr && slot == held_))
    {
        return stockName;
    }
    ++counts_.named;
    return static_cast<std::uintptr_t>(playerTwoName_);
}

bool SharedLayerSlot::SkipSubmit(const void* slot) noexcept
{
    if (!hud_rules::PlayerTwoEntry(CurrentOverlayPass(), slot != nullptr && slot == held_))
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
    if (slot == nullptr || renderer_ == nullptr || submit_ == nullptr)
    {
        return false;
    }
    submit_(renderer_, setup_.pool, slot, 0);
    ++counts_.safetySubmits;
    return true;
}

}  // namespace lss::hud
