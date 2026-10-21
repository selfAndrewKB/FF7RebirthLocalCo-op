// Host-side tests for platform::VTableEntry, run without the game.
//
// A read-only page of pointers stands in for an engine vtable. The entry is replaced only while it holds the function
// the research identified, put back only while it still holds ours, the page's protection is as before afterwards, and
// a forgotten restore is done by the destructor.

#include "platform/vtable_entry.h"

#include "platform/image.h"

#include "expect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using lss::platform::VTableEntry;

int Original()
{
    return 1;
}
int Replacement()
{
    return 2;
}
int Foreign()
{
    return 3;
}

std::uintptr_t Address(int (*fn)())
{
    return reinterpret_cast<std::uintptr_t>(fn);
}

/// Eight entries, every one Original, on a page made read-only as the game's vtables are.
class ReadOnlyTable
{
public:
    static constexpr std::size_t kEntries = 8;

    ReadOnlyTable()
        : memory_(static_cast<std::uintptr_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
    {
        for (std::size_t i = 0; i < kEntries; ++i)
        {
            memory_[i] = Address(&Original);
        }
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_READONLY, &previous);
    }
    ~ReadOnlyTable()
    {
        VirtualFree(memory_, 0, MEM_RELEASE);
    }

    std::uintptr_t Entry(std::size_t index) const
    {
        return reinterpret_cast<std::uintptr_t>(memory_ + index);
    }
    std::uintptr_t Holds(std::size_t index) const
    {
        return memory_[index];
    }
    /// A write by someone else (another mod, a game patch), outside VTableEntry.
    void WriteByHand(std::size_t index, std::uintptr_t value)
    {
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_READWRITE, &previous);
        memory_[index] = value;
        VirtualProtect(memory_, 0x1000, previous, &previous);
    }
    DWORD Protection() const
    {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(memory_, &info, sizeof(info));
        return info.Protect;
    }
    bool OthersUntouched() const
    {
        for (std::size_t i = 0; i < kEntries; ++i)
        {
            if (i != 1 && memory_[i] != Address(&Original))
            {
                return false;
            }
        }
        return true;
    }

private:
    std::uintptr_t* memory_;
};

void TestReplaceAndRestore()
{
    std::puts("vtable entry: replaced while it holds the expected function, put back, the page read-only throughout");
    const ReadOnlyTable table;
    EXPECT(table.Protection() == PAGE_READONLY);

    VTableEntry entry;
    EXPECT(entry.Original() == 0);
    EXPECT(entry.Replace(table.Entry(1), Address(&Original), Address(&Replacement)));
    EXPECT(table.Holds(1) == Address(&Replacement));
    EXPECT(entry.Original() == Address(&Original));
    EXPECT(table.Protection() == PAGE_READONLY);
    EXPECT(table.OthersUntouched());
    EXPECT(reinterpret_cast<int (*)()>(table.Holds(1))() == 2);

    EXPECT(entry.Restore());
    EXPECT(table.Holds(1) == Address(&Original));
    EXPECT(table.Protection() == PAGE_READONLY);
    EXPECT(entry.Restore());  // nothing to do: still true
    EXPECT(table.Holds(1) == Address(&Original));
}

void TestRefusals()
{
    std::puts("vtable entry: refuses another function in the entry, a misaligned entry, and a second replace");
    const ReadOnlyTable table;
    VTableEntry         entry;

    EXPECT(!entry.Replace(table.Entry(1), Address(&Foreign), Address(&Replacement)));
    EXPECT(table.Holds(1) == Address(&Original));
    EXPECT(entry.Original() == 0);

    EXPECT(!entry.Replace(table.Entry(1) + 4, Address(&Original), Address(&Replacement)));
    EXPECT(table.OthersUntouched() && table.Holds(1) == Address(&Original));

    EXPECT(entry.Replace(table.Entry(1), Address(&Original), Address(&Replacement)));
    EXPECT(!entry.Replace(table.Entry(2), Address(&Original), Address(&Replacement)));
    EXPECT(table.Holds(2) == Address(&Original));
    EXPECT(entry.Restore());
}

void TestForeignWriteAfterUs()
{
    std::puts("vtable entry: a restore is refused while someone else's function is in the entry, and done once ours is back");
    ReadOnlyTable table;
    VTableEntry   entry;
    EXPECT(entry.Replace(table.Entry(1), Address(&Original), Address(&Replacement)));

    table.WriteByHand(1, Address(&Foreign));
    EXPECT(!entry.Restore());
    EXPECT(table.Holds(1) == Address(&Foreign));  // the other party's function is left alone

    table.WriteByHand(1, Address(&Replacement));
    EXPECT(entry.Restore());
    EXPECT(table.Holds(1) == Address(&Original));
}

void TestDestructorRestores()
{
    std::puts("vtable entry: the destructor puts the entry back when the owner did not");
    const ReadOnlyTable table;
    {
        VTableEntry entry;
        EXPECT(entry.Replace(table.Entry(1), Address(&Original), Address(&Replacement)));
        EXPECT(table.Holds(1) == Address(&Replacement));
    }
    EXPECT(table.Holds(1) == Address(&Original));
}

void TestReplaceThroughTheImage()
{
    std::puts("vtable entry: an entry and its function given as research addresses are rebased through the image");
    const ReadOnlyTable table;
    constexpr std::uintptr_t kIdaBase = 0x140000000;
    // An image spanning the table's page and Original's code (a span is only numbers to Rebase), so both have a
    // research address: the entry's and the function's are their offsets from the lower of the two.
    const std::uintptr_t low  = std::min(table.Entry(0), Address(&Original));
    const std::uintptr_t high = std::max(table.Entry(0), Address(&Original));
    const auto           image = lss::platform::GameImage::At(low, high - low + 0x1000);
    const std::uintptr_t idaEntry    = kIdaBase + (table.Entry(1) - low);
    const std::uintptr_t idaOriginal = kIdaBase + (Address(&Original) - low);
    VTableEntry entry;
    EXPECT(!entry.Replace(image, kIdaBase + (high - low) + 0x2000, idaOriginal, Address(&Replacement)));  // outside
    EXPECT(!entry.Replace(image, idaEntry, kIdaBase + (high - low) + 0x2000, Address(&Replacement)));
    EXPECT(table.Holds(1) == Address(&Original));
    EXPECT(entry.Replace(image, idaEntry, idaOriginal, Address(&Replacement)));
    EXPECT(table.Holds(1) == Address(&Replacement) && table.OthersUntouched());
    EXPECT(entry.Restore());
    EXPECT(table.Holds(1) == Address(&Original));
}

}  // namespace

int main()
{
    TestReplaceAndRestore();
    TestRefusals();
    TestForeignWriteAfterUs();
    TestDestructorRestores();
    TestReplaceThroughTheImage();

    return lss::test::Report();
}
