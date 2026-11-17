// Host-side tests for renderer/family_boundary_list.h, run without the game: a fake command list
// laid out with the game's offsets, a fake arena grow, and a fake context and device.

#include "renderer/family_boundary_list.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

namespace boundary = lss::renderer::boundary;

const int kVtable = 0;  // a node vtable stand-in (only its address matters)

/// A command list with the game's field offsets; its arena is `arena`, cursor at `start`.
struct FakeList
{
    alignas(16) std::array<std::byte, 0x48> bytes{};

    FakeList(std::byte* start, std::byte* end)
    {
        boundary::Write<std::uint64_t>(bytes.data(), boundary::list::kCommandLink,
                                       reinterpret_cast<std::uint64_t>(bytes.data()));  // Root
        boundary::Write<std::uint64_t>(bytes.data(), boundary::list::kArena, reinterpret_cast<std::uint64_t>(start));
        boundary::Write<std::uint64_t>(bytes.data(), boundary::list::kArena + 8, reinterpret_cast<std::uint64_t>(end));
    }
    std::uint64_t Field(std::size_t offset) const { return boundary::Load<std::uint64_t>(bytes.data(), offset); }
    std::uint32_t Commands() const { return boundary::Load<std::uint32_t>(bytes.data(), boundary::list::kNumCommands); }
};

std::uint64_t Qword(std::uint64_t at)
{
    std::uint64_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof(value));
    return value;
}

alignas(16) std::array<std::byte, 0x100> g_grown{};
void*        g_growArena = nullptr;
std::int32_t g_growBytes = 0;

/// Moves the arena to g_grown, its cursor deliberately unaligned.
void FakeGrow(void* arena, std::int32_t bytes)
{
    g_growArena = arena;
    g_growBytes = bytes;
    boundary::Write<std::uint64_t>(static_cast<std::byte*>(arena), 0, reinterpret_cast<std::uint64_t>(g_grown.data() + 3));
    boundary::Write<std::uint64_t>(static_cast<std::byte*>(arena), 8,
                                   reinterpret_cast<std::uint64_t>(g_grown.data() + g_grown.size()));
}

void TestAppendLinksNodes()
{
    std::puts("append: nodes are placed at the aligned cursor, counted and linked in order");
    alignas(16) std::array<std::byte, 0x80> arena{};
    FakeList list(arena.data() + 1, arena.data() + arena.size());  // an unaligned cursor

    const auto first = boundary::AppendNode(list.bytes.data(), &kVtable, &FakeGrow);
    EXPECT(first == reinterpret_cast<std::uint64_t>(arena.data() + 8));
    EXPECT(list.Field(0) == first);  // Root
    EXPECT(list.Field(boundary::list::kCommandLink) == first + 8);
    EXPECT(list.Field(boundary::list::kArena) == first + 0x10);
    EXPECT(list.Commands() == 1);
    EXPECT(Qword(first) == reinterpret_cast<std::uint64_t>(&kVtable));
    EXPECT(Qword(first + 8) == 0);

    const auto second = boundary::AppendNode(list.bytes.data(), &kVtable, &FakeGrow);
    EXPECT(second == first + 0x10);
    EXPECT(Qword(first + 8) == second);  // first.next
    EXPECT(list.Field(boundary::list::kCommandLink) == second + 8);
    EXPECT(list.Commands() == 2);
    EXPECT(g_growArena == nullptr);  // it fitted
}

void TestAppendGrowsTheArena()
{
    std::puts("append: an arena without room is grown through the game's function, asked for the node + 8");
    alignas(16) std::array<std::byte, 0x20> arena{};
    FakeList list(arena.data() + 0x18, arena.data() + 0x20);  // 8 bytes left: too few

    g_growArena     = nullptr;
    const auto node = boundary::AppendNode(list.bytes.data(), &kVtable, &FakeGrow);
    EXPECT(g_growArena == list.bytes.data() + boundary::list::kArena);
    EXPECT(g_growBytes == 0x18);
    EXPECT(node == reinterpret_cast<std::uint64_t>(g_grown.data() + 8));  // aligned in the new block
    EXPECT(list.Field(0) == node);
    EXPECT(list.Field(boundary::list::kArena) == node + 0x10);

    FakeList empty(nullptr, nullptr);  // no block yet: cursor 0
    g_growArena = nullptr;
    EXPECT(boundary::AppendNode(empty.bytes.data(), &kVtable, &FakeGrow) ==
           reinterpret_cast<std::uint64_t>(g_grown.data() + 8));
    EXPECT(g_growArena != nullptr);
}

void TestExecuting()
{
    std::puts("executing: read from the list's byte");
    FakeList list(nullptr, nullptr);
    EXPECT(!boundary::IsExecuting(list.bytes.data()));
    list.bytes[boundary::list::kExecuting] = std::byte{1};
    EXPECT(boundary::IsExecuting(list.bytes.data()));
}

void TestDefaultDirectContext()
{
    std::puts("context guard: only the device's first direct context, of the expected class, not async compute");
    constexpr std::uintptr_t kContextVtable = 0x14697BFA8;
    alignas(16) std::array<std::byte, 0x200>  context{};
    alignas(16) std::array<std::byte, 0x200>  other{};
    alignas(16) std::array<std::byte, 0x1800> device{};  // room for either build's direct-contexts field
    const std::byte*                          directs[2] = {context.data(), other.data()};

    boundary::Write<std::uintptr_t>(device.data(), boundary::context::kDeviceDirect,
                                    reinterpret_cast<std::uintptr_t>(directs));
    for (auto* ctx : {context.data(), other.data()})
    {
        boundary::Write<std::uintptr_t>(ctx, 0, kContextVtable);
        boundary::Write<std::uintptr_t>(ctx, boundary::context::kDevice, reinterpret_cast<std::uintptr_t>(device.data()));
    }

    EXPECT(boundary::IsDefaultDirectContext(context.data(), kContextVtable));
    EXPECT(!boundary::IsDefaultDirectContext(other.data(), kContextVtable));  // not the first
    EXPECT(!boundary::IsDefaultDirectContext(context.data(), 0x145F04308));   // another class
    EXPECT(!boundary::IsDefaultDirectContext(nullptr, kContextVtable));
    context[boundary::context::kIsAsyncCompute] = std::byte{1};
    EXPECT(!boundary::IsDefaultDirectContext(context.data(), kContextVtable));
    context[boundary::context::kIsAsyncCompute] = std::byte{0};
    boundary::Write<std::uintptr_t>(context.data(), boundary::context::kDevice, 0);
    EXPECT(!boundary::IsDefaultDirectContext(context.data(), kContextVtable));  // no device
}

}  // namespace

int main()
{
    TestAppendLinksNodes();
    TestAppendGrowsTheArena();
    TestExecuting();
    TestDefaultDirectContext();

    return lss::test::Report();
}
