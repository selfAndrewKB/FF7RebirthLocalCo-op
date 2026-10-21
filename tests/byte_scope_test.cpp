// Host-side tests for platform/byte_scope, run without the game: ByteContext and ByteScope on plain memory.
//
// A Set range is written in and put back, unless the game changed it inside (then kept and reported); an
// Exchange range swaps the player's copy with the live bytes both ways; a Keep range is put back whatever the
// game wrote inside (reported). A store is fixed: past its capacity, a definition is refused. An unreadable
// range opens nothing and leaves the readable ones as they were. Retarget moves a range, its value kept.

#include "platform/byte_scope.h"

#include "expect.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

using lss::platform::ByteContext;
using lss::platform::ByteScope;
using lss::platform::ByteStore;
using Kind = ByteContext::Kind;

std::uintptr_t At(const void* object) noexcept
{
    return reinterpret_cast<std::uintptr_t>(object);
}

void TestOpenAndClose()
{
    std::puts("byte scope: Set written in and put back, Exchange swapped both ways, Keep put back after a write");
    std::uint64_t              primary = 0x1111;         // a Set slot (a pointer)
    std::uint8_t               type    = 3;              // a Set slot (a byte)
    std::array<std::uint8_t, 4> focus{1, 2, 3, 4};        // an Exchange slot (state the game keeps)
    std::array<std::uint8_t, 3> block{7, 7, 7};           // a Keep slot (a global the call must not change)

    ByteStore<4, 16> context;
    EXPECT(!context.Defined() && !context.Open());
    const ByteContext::Range ranges[] = {
        {At(&primary), sizeof(primary), Kind::Set},
        {At(&type), 1, Kind::Set},
        {At(focus.data()), 4, Kind::Exchange},
        {At(block.data()), 3, Kind::Keep},
    };
    EXPECT(context.Define(ranges) && context.Count() == 4);
    const std::uint64_t two = 0x2222;
    std::memcpy(context.Value(0).data(), &two, sizeof(two));
    context.Value(1)[0] = 9;
    context.Value(2)[0] = 7;  // the player's copy starts with a 7 in its first byte
    EXPECT(context.Value(3).size() == 3 && context.Value(4).empty());

    {
        ByteScope scope(context);
        EXPECT(scope.Held() && context.IsOpen());
        EXPECT(primary == 0x2222 && type == 9);
        EXPECT(focus[0] == 7 && focus[1] == 0 && focus[2] == 0 && focus[3] == 0);
        EXPECT(block[0] == 7 && block[1] == 7 && block[2] == 7);  // Keep writes nothing in
        EXPECT(!context.Open());                                   // no nesting
        focus[1] = 8;                                              // the game writes the player's state inside
        block[2] = 1;                                              // the game writes the global inside
    }
    EXPECT(!context.IsOpen());
    EXPECT(primary == 0x1111 && type == 3);
    EXPECT(focus[0] == 1 && focus[1] == 2 && focus[2] == 3 && focus[3] == 4);  // the game's bytes back
    EXPECT(context.Value(2)[0] == 7 && context.Value(2)[1] == 8);               // the copy kept the write
    EXPECT(block[2] == 7);                                                       // the Keep range put back

    // A Set slot the game changed inside is left as the game wrote it, and reported with the Keep write.
    EXPECT(context.Open());
    primary = 0x3333;
    block[0] = 0;
    EXPECT(context.Close() == 2);
    EXPECT(primary == 0x3333 && type == 3 && block[0] == 7);

    // Release closes early and the destructor leaves it closed.
    {
        ByteScope scope(context);
        EXPECT(scope.Held() && primary == 0x2222);
        EXPECT(scope.Release() == 0 && !scope.Held() && !context.IsOpen() && primary == 0x3333);
        EXPECT(scope.Release() == 0);
        primary = 0x4444;  // nothing is open: the destructor must not touch it
    }
    EXPECT(primary == 0x4444);
}

void TestRefusals()
{
    std::puts("byte scope: Define and Clear refused while open; a faulting range opens nothing; capacity and sizes");
    std::uint64_t    primary = 0x1111;
    ByteStore<2, 16> context;
    const ByteContext::Range one[] = {{At(&primary), sizeof(primary), Kind::Set}};
    EXPECT(context.Define(one));
    EXPECT(context.Open());
    EXPECT(!context.Define(one) && !context.Clear() && !context.Retarget(0, At(&primary)));
    context.Close();
    EXPECT(context.Clear() && !context.Defined() && !context.Open());

    // An unreadable range opens nothing and leaves the readable ones as they were.
    const std::uint64_t      two   = 0x2222;
    const ByteContext::Range bad[] = {
        {At(&primary), sizeof(primary), Kind::Set},
        {0x10, 8, Kind::Set},
    };
    EXPECT(context.Define(bad));
    std::memcpy(context.Value(0).data(), &two, sizeof(two));
    EXPECT(!context.Open() && primary == 0x1111 && !context.IsOpen());

    // A zero-sized or zero-addressed range refuses the definition; so does one past the store.
    const ByteContext::Range empty[] = {{At(&primary), 0, Kind::Set}};
    EXPECT(!context.Define(empty) && !context.Defined());
    const ByteContext::Range nowhere[] = {{0, 8, Kind::Set}};
    EXPECT(!context.Define(nowhere) && !context.Defined());
    std::array<std::uint8_t, 3> small{};
    const ByteContext::Range   three[] = {{At(&small[0]), 1, Kind::Set}, {At(&small[1]), 1, Kind::Set},
                                          {At(&small[2]), 1, Kind::Set}};
    EXPECT(!context.Define(three) && !context.Defined());  // three ranges in a store of two
    std::array<std::uint8_t, 17> wide{};
    const ByteContext::Range     tooWide[] = {{At(wide.data()), 17, Kind::Keep}};
    EXPECT(!context.Define(tooWide) && !context.Defined());  // seventeen bytes in a store of sixteen
    const ByteContext::Range     justFits[] = {{At(wide.data()), 16, Kind::Keep}};
    EXPECT(context.Define(justFits) && context.Defined());
}

void TestRetarget()
{
    std::puts("byte scope: Retarget moves a range to another address with its value kept");
    std::uint32_t    a = 1, b = 2;
    ByteStore<1, 4>  context;
    const ByteContext::Range one[] = {{At(&a), sizeof(a), Kind::Exchange}};
    EXPECT(context.Define(one));
    const std::uint32_t copy = 9;
    std::memcpy(context.Value(0).data(), &copy, sizeof(copy));
    {
        const ByteScope scope(context);
        EXPECT(scope.Held() && a == 9 && b == 2);
        a = 10;  // the game writes the player's state
    }
    EXPECT(a == 1);
    EXPECT(context.Retarget(0, At(&b)) && context.RangeAt(0).live == At(&b));
    EXPECT(!context.Retarget(1, At(&b)) && !context.Retarget(0, 0));
    {
        const ByteScope scope(context);
        EXPECT(scope.Held() && a == 1 && b == 10);  // the copy carried the game's write to the new slot
    }
    EXPECT(a == 1 && b == 2);
}

}  // namespace

int main()
{
    TestOpenAndClose();
    TestRefusals();
    TestRetarget();

    return lss::test::Report();
}
