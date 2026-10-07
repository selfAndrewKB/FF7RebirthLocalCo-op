#pragma once

// The RHI command-list and context layout the family boundary relies on (family_boundary.h;
// RE-p2-main-menu-crash.md, pass S-CRASH3 §S3.1, §S3.4). Pure logic over the game's memory, with no
// engine or UE4SS dependency, so it is tested on the host (tests/family_boundary_list_test.cpp).
//
// FRHICommandListBase (stock 4.26 roles, this build's offsets):
//   +0x00 Root            the first node
//   +0x08 CommandLink     the address of the last node's `next` field (Root at first)
//   +0x10 bExecuting      set while the list is being walked
//   +0x14 NumCommands
//   +0x20 Context         the command context the walk executes on
//   +0x30 / +0x38 / +0x40 the arena: cursor, end, block head
// A node is {vtable, next, payload...}; the walk reads `next`, then calls vtable[0](node, list, debug).
//
// The game appends a node inline in every command (there is no callable "append"); AppendNode is
// that sequence, written out once, byte for byte as the game's own 16-byte SubmitCommandsHint append
// (TRDGLambdaPass_SubmitCommandsHint 0x1425138B0): align the cursor to 8, grow the arena through the
// game's ME_CmdBuf_GrowAlloc when the node does not fit (it is passed the node size + 8, as the game
// passes it), then advance the cursor, count the command, link the node and fill it.

#include "platform/build_value.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::renderer::boundary {

namespace list {
inline constexpr std::size_t kCommandLink = 0x08;
inline constexpr std::size_t kExecuting   = 0x10;
inline constexpr std::size_t kNumCommands = 0x14;
inline constexpr std::size_t kContext     = 0x20;
inline constexpr std::size_t kArena       = 0x30;  // cursor; the end follows at +8
}  // namespace list

namespace context {
inline constexpr std::size_t kIsAsyncCompute = 0x146;  // bIsAsyncComputeContext (byte)
inline constexpr std::size_t kDevice         = 0x148;  // FD3D12Device*
// device: the direct contexts' array; [0] is the default.
// 1.005: FD3D12Device grew; ME_RenderDevice_EndFrame 0x140A4974C reads [device+13D8h] at 0x140A4980A, then
// [[..]] -> ClearState (1.004 0x140D0126F [rsi+0F70h]); the async array moved 0xFB8 -> 0x1420 likewise.
inline constexpr platform::PerBuild<std::size_t> kDeviceDirect = {0xF70, 0x13D8};
}  // namespace context

inline constexpr std::size_t   kNodeSize     = 0x10;  // {vtable, next}
inline constexpr std::int32_t  kGrowRequest  = kNodeSize + 8;

/// ME_CmdBuf_GrowAlloc(&arena, bytes): gives the arena a new block, rewriting cursor, end and head.
using GrowFn = void (*)(void* arena, std::int32_t bytes);

template <typename T>
T Read(const std::byte* at, std::size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, at + offset, sizeof(value));
    return value;
}

template <typename T>
void Write(std::byte* at, std::size_t offset, T value) noexcept
{
    std::memcpy(at + offset, &value, sizeof(value));
}

inline bool IsExecuting(const std::byte* commandList) noexcept
{
    return Read<std::uint8_t>(commandList, list::kExecuting) != 0;
}

/// Appends a node holding `vtable` to the end of `commandList`; returns the node's address.
inline std::uintptr_t AppendNode(std::byte* commandList, const void* vtable, GrowFn grow) noexcept
{
    const auto fits = [commandList](std::uint64_t node) {
        return node != 0 && node + kNodeSize <= Read<std::uint64_t>(commandList, list::kArena + 8);
    };
    std::uint64_t node = (Read<std::uint64_t>(commandList, list::kArena) + 7) & ~std::uint64_t{7};
    if (!fits(node))
    {
        grow(commandList + list::kArena, kGrowRequest);
        node = (Read<std::uint64_t>(commandList, list::kArena) + 7) & ~std::uint64_t{7};
    }
    Write<std::uint64_t>(commandList, list::kArena, node + kNodeSize);
    Write<std::uint32_t>(commandList, list::kNumCommands, Read<std::uint32_t>(commandList, list::kNumCommands) + 1);
    auto* const link = reinterpret_cast<std::byte*>(Read<std::uint64_t>(commandList, list::kCommandLink));
    Write<std::uint64_t>(link, 0, node);
    Write<std::uint64_t>(commandList, list::kCommandLink, node + 8);
    auto* const bytes = reinterpret_cast<std::byte*>(node);
    Write<std::uint64_t>(bytes, 8, 0);  // next
    Write<const void*>(bytes, 0, vtable);
    return node;
}

/// True if `ctx` is the device's default direct context (the one RHIEndFrame clears): an
/// FD3D12CommandContext (`contextVtable`, not the multi-GPU redirector), not async compute, and the
/// first of its device's direct contexts.
inline bool IsDefaultDirectContext(const std::byte* ctx, std::uintptr_t contextVtable) noexcept
{
    if (ctx == nullptr || Read<std::uintptr_t>(ctx, 0) != contextVtable ||
        Read<std::uint8_t>(ctx, context::kIsAsyncCompute) != 0)
    {
        return false;
    }
    const auto* device = reinterpret_cast<const std::byte*>(Read<std::uintptr_t>(ctx, context::kDevice));
    const auto* direct = device != nullptr ? reinterpret_cast<const std::byte*>(
                                                 Read<std::uintptr_t>(device, context::kDeviceDirect))
                                           : nullptr;
    return direct != nullptr && Read<const std::byte*>(direct, 0) == ctx;
}

}  // namespace lss::renderer::boundary
