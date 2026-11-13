#pragma once

// A second set of the menu renderer's vertex and index buffers, for a layer entry drawn beside the game's own in one
// slot (hud/passes/column_hud_images.h: the overlay's pool 2; hud/minimap/minimap.h: the minimap's pool 0). Built as
// FEndMenuRenderer's ctor builds each pool's (research/RE-p2-hud-second-image.md, RE-p2-own-minimap.md): a 0x20-byte
// FRenderResource with the pool's own vtable, not listed yet, the ctor's word and the capacity (facts/hud.h's layout),
// then queued for initialisation on the render thread. The SRM slot is a zeroed 0x20-byte block. Also the FName value helper both
// use for entry names. Game thread, except that the render thread initialises the buffers.

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/NameTypes.hpp>

#include "facts/hud.h"
#include "platform/guarded.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace lss::hud {

/// A render resource of the menu renderer's buffer kind, laid out as its ctor lays it out.
struct alignas(16) MenuBuffer
{
    std::uint8_t bytes[facts::kMenuBufferSize] = {};
};

/// One extra entry's buffer set. Never freed: the renderer may hold them until the process ends.
struct MenuBufferSet
{
    MenuBuffer vertex;
    MenuBuffer index;
    MenuBuffer srm;
};

inline constexpr std::uint32_t kMenuVertexCapacity = 0x400;  // the same for every pool
inline constexpr std::uint32_t kMenuIndexCapacity  = 0x600;

using BeginInitResourceFn = void (*)(void* resource);

/// Fills `set` like the pool whose buffer objects are at `renderer + vertexOffset / indexOffset`, and queues their
/// initialisation. False (nothing queued) if the pool's buffers are not of the kind the research read.
inline bool BuildMenuBuffers(const void* renderer, std::uint32_t vertexOffset, std::uint32_t indexOffset,
                             std::uintptr_t vertexVtable, std::uintptr_t indexVtable, BeginInitResourceFn beginInit,
                             MenuBufferSet& set) noexcept
{
    const auto*    base       = static_cast<const std::uint8_t*>(renderer);
    std::uintptr_t buffers[2] = {};
    std::uintptr_t vtables[2] = {};
    if (renderer == nullptr || !platform::CopyGuarded(&buffers[0], base + vertexOffset, sizeof(std::uintptr_t)) ||
        !platform::CopyGuarded(&buffers[1], base + indexOffset, sizeof(std::uintptr_t)) || buffers[0] == 0 ||
        buffers[1] == 0 ||
        !platform::CopyGuarded(&vtables[0], reinterpret_cast<const void*>(buffers[0]), sizeof(std::uintptr_t)) ||
        !platform::CopyGuarded(&vtables[1], reinterpret_cast<const void*>(buffers[1]), sizeof(std::uintptr_t)) ||
        vtables[0] != vertexVtable || vtables[1] != indexVtable)
    {
        return false;
    }
    auto build = [](MenuBuffer& buffer, std::uintptr_t vtable, std::uint32_t capacity) {
        const std::int32_t  unlisted = -1;
        const std::uint16_t two      = 2;
        std::memcpy(buffer.bytes + facts::kMenuBufferVtable, &vtable, sizeof(vtable));
        std::memcpy(buffer.bytes + facts::kMenuBufferListIndex, &unlisted, sizeof(unlisted));
        std::memcpy(buffer.bytes + facts::kMenuBufferCtorWord, &two, sizeof(two));
        std::memcpy(buffer.bytes + facts::kMenuBufferCapacity, &capacity, sizeof(capacity));
    };
    build(set.vertex, vertexVtable, kMenuVertexCapacity);
    build(set.index, indexVtable, kMenuIndexCapacity);
    beginInit(&set.vertex);  // enqueued: runs on the render thread after the commands already queued
    beginInit(&set.index);
    return true;
}

/// True once the render thread has initialised both buffers: InitResource has listed them (the list index is no longer
/// -1) and each holds its RHI buffer. Check every frame: an RHI device reset releases and re-creates every listed
/// resource, and a frame drawn in between would find no buffer.
inline bool MenuBuffersInitialised(const MenuBufferSet& set) noexcept
{
    for (const MenuBuffer* buffer : {&set.vertex, &set.index})
    {
        std::int32_t  listIndex = -1;
        std::uint64_t rhi       = 0;
        std::memcpy(&listIndex, buffer->bytes + facts::kMenuBufferListIndex, sizeof(listIndex));
        std::memcpy(&rhi, buffer->bytes + facts::kMenuBufferRhi, sizeof(rhi));
        if (listIndex == -1 || rhi == 0)
        {
            return false;
        }
    }
    return true;
}

/// An FName as the engine stores it: {ComparisonIndex, Number}. May throw (UE4SS name table).
inline std::uint64_t EngineNameValue(const wchar_t* text, RC::Unreal::EFindName find)
{
    const RC::Unreal::FName name(text, find);
    const std::uint64_t index  = name.GetComparisonIndex();
    const std::uint64_t number = static_cast<std::uint32_t>(name.GetNumber());
    return index | (number << 32);
}

}  // namespace lss::hud
