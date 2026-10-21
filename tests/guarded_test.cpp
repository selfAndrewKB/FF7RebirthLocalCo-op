// Host-side tests for platform/guarded, run without the game.
//
// The guards around engine memory and engine calls: a copy from readable memory succeeds and one from a page nothing
// may touch, or from null, fails without taking the process down; a guarded call that faults reports false and
// records what faulted (the code, the kind of access, the address) for the log; the filter passes a stack overflow on.
// The Read family every module reads engine memory with: by address or by object and offset, false or zero for a
// null or unreadable source, the value otherwise; WriteAt says whether a write landed.

#include "platform/guarded.h"

#include "exec_page.h"
#include "expect.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using lss::platform::CallGuarded;
using lss::platform::CopyGuarded;
using lss::platform::DescribeFault;
using lss::platform::Fault;
using lss::platform::GuardFilter;
using lss::platform::LastFault;
using lss::platform::Read;
using lss::platform::ReadAt;
using lss::platform::ReadField;
using lss::platform::ReadPointer;
using lss::platform::ReadPointerAt;
using lss::platform::WriteAt;
using lss::platform::WriteGuarded;

int ReadsThrough(const int* pointer)
{
    return *pointer;
}
void WritesThrough(int* pointer)
{
    *pointer = 1;
}

/// A page that may be read but not written.
class ReadOnlyPage
{
public:
    ReadOnlyPage()
        : memory_(static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
    {
        std::memset(memory_, 0x5A, 0x1000);
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_READONLY, &previous);
    }
    ~ReadOnlyPage()
    {
        VirtualFree(memory_, 0, MEM_RELEASE);
    }
    std::uint8_t* Pointer() const
    {
        return memory_;
    }

private:
    std::uint8_t* memory_;
};

bool Contains(const wchar_t* text, const wchar_t* part)
{
    return std::wstring(text).find(part) != std::wstring::npos;
}

void TestCopies()
{
    std::puts("guarded: a copy from readable memory succeeds; from a no-access page or null it fails, the output untouched");
    const std::uint8_t source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::uint8_t       out[8]    = {};
    EXPECT(CopyGuarded(out, source, sizeof(out)));
    EXPECT(std::memcmp(out, source, sizeof(out)) == 0);

    const lss::test::NoAccessPage noAccess;
    std::uint8_t                  untouched[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    EXPECT(!CopyGuarded(untouched, noAccess.Pointer(0x10), sizeof(untouched)));
    EXPECT(untouched[0] == 9 && untouched[7] == 9);
    EXPECT(!CopyGuarded(untouched, nullptr, sizeof(untouched)));
    EXPECT(untouched[0] == 9);
}

void TestWrites()
{
    std::puts("guarded: a write to writable memory succeeds; to a read-only page it fails, the page unchanged");
    const std::uint8_t value[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    std::uint8_t       target[4] = {};
    EXPECT(WriteGuarded(target, value, sizeof(value)));
    EXPECT(std::memcmp(target, value, sizeof(value)) == 0);

    const ReadOnlyPage readOnly;
    EXPECT(!WriteGuarded(readOnly.Pointer() + 0x20, value, sizeof(value)));
    EXPECT(readOnly.Pointer()[0x20] == 0x5A);
}

void TestCallRecordsTheFault()
{
    std::puts("guarded: a call that faults reports false and records the access violation (kind and address) for the log");
    const lss::test::NoAccessPage noAccess;

    int value = 42;
    int out   = 7;
    EXPECT(CallGuarded(&ReadsThrough, out, static_cast<const int*>(&value)));
    EXPECT(out == 42);

    out = 7;
    EXPECT(!CallGuarded(&ReadsThrough, out, noAccess.Pointer<const int>(0x10)));
    EXPECT(out == 7);
    Fault fault = LastFault();
    EXPECT(fault.code == EXCEPTION_ACCESS_VIOLATION);
    EXPECT(fault.access == 0);  // a read
    EXPECT(fault.target == noAccess.At(0x10));
    EXPECT(fault.at != 0);
    auto text = DescribeFault(fault);
    EXPECT(Contains(text.text, L"exception 0xC0000005 at "));
    // The process's main module is "the game" to the describer (an IDA address); here that is this test.
    EXPECT(Contains(text.text, L"the game 0x14"));
    EXPECT(Contains(text.text, L" reading 0x"));

    EXPECT(!CallGuarded(&WritesThrough, noAccess.Pointer<int>(0x20)));
    fault = LastFault();
    EXPECT(fault.code == EXCEPTION_ACCESS_VIOLATION);
    EXPECT(fault.access == 1);  // a write
    EXPECT(fault.target == noAccess.At(0x20));
    text = DescribeFault(fault);
    EXPECT(Contains(text.text, L" writing 0x"));

    EXPECT(CallGuarded(&WritesThrough, &value));
    EXPECT(value == 1);
}

/// An engine object as the modules see one: fields at offsets read in IDA.
struct Object
{
    std::uint32_t head    = 0x11223344;
    void*         pointer = nullptr;
    std::uint8_t  flag    = 7;
};

void TestReads()
{
    std::puts("guarded: Read by address, by pointer and by object and offset; a null or no-access source reads false, "
              "the output untouched");
    int value = 42;
    int out   = 7;
    EXPECT(Read(reinterpret_cast<std::uintptr_t>(&value), out) && out == 42);
    out = 7;
    EXPECT(Read(static_cast<const void*>(&value), out) && out == 42);
    out = 7;
    EXPECT(!Read(std::uintptr_t{0}, out) && out == 7);
    EXPECT(!Read(static_cast<const void*>(nullptr), out) && out == 7);
    const lss::test::NoAccessPage noAccess;
    EXPECT(!Read(noAccess.At(0x10), out) && out == 7);
    EXPECT(!Read(noAccess.Pointer(0x10), out) && out == 7);

    Object object;
    object.pointer    = &value;
    std::uint8_t flag = 0;
    EXPECT(Read(&object, offsetof(Object, flag), flag) && flag == 7);
    EXPECT(!Read(static_cast<const void*>(nullptr), offsetof(Object, flag), flag));
    EXPECT(!Read(noAccess.Pointer(), 0x20, flag) && flag == 7);

    std::puts("guarded: ReadAt, ReadField, ReadPointer and ReadPointerAt give the value, or zero when unreadable");
    EXPECT(ReadAt<int>(reinterpret_cast<std::uintptr_t>(&value)) == 42);
    EXPECT(ReadAt<int>(0) == 0);
    EXPECT(ReadAt<int>(noAccess.At(0x10)) == 0);
    EXPECT(ReadField<std::uint32_t>(&object, offsetof(Object, head)) == 0x11223344);
    EXPECT(ReadField<std::uint32_t>(nullptr, offsetof(Object, head)) == 0);
    EXPECT(ReadField<std::uint32_t>(noAccess.Pointer(), 0) == 0);
    EXPECT(ReadPointer(&object, offsetof(Object, pointer)) == &value);
    EXPECT(ReadPointer(nullptr, offsetof(Object, pointer)) == nullptr);
    EXPECT(ReadPointer(noAccess.Pointer(), 0x10) == nullptr);
    void* const slot = &object;
    EXPECT(ReadPointerAt(reinterpret_cast<std::uintptr_t>(&slot)) == &object);
    EXPECT(ReadPointerAt(0) == nullptr);
    EXPECT(ReadPointerAt(noAccess.At(0x18)) == nullptr);
}

void TestWriteAt()
{
    std::puts("guarded: WriteAt lands in writable memory; a null address or a read-only page reads false, the page "
              "unchanged");
    int target = 0;
    EXPECT(WriteAt(reinterpret_cast<std::uintptr_t>(&target), 5) && target == 5);
    EXPECT(!WriteAt(std::uintptr_t{0}, 5));
    const ReadOnlyPage readOnly;
    EXPECT(!WriteAt(reinterpret_cast<std::uintptr_t>(readOnly.Pointer() + 0x20), std::uint8_t{1}));
    EXPECT(readOnly.Pointer()[0x20] == 0x5A);
}

void TestFilterAndText()
{
    std::puts("guarded: the filter handles everything but a stack overflow; a fault never recorded has its own text");
    EXPECT(GuardFilter(EXCEPTION_STACK_OVERFLOW) == EXCEPTION_CONTINUE_SEARCH);
    EXPECT(GuardFilter(EXCEPTION_ACCESS_VIOLATION) == EXCEPTION_EXECUTE_HANDLER);
    EXPECT(GuardFilter(EXCEPTION_ILLEGAL_INSTRUCTION) == EXCEPTION_EXECUTE_HANDLER);
    EXPECT(std::wstring(DescribeFault(Fault{}).text) == L"no exception recorded");
}

}  // namespace

int main()
{
    TestCopies();
    TestWrites();
    TestCallRecordsTheFault();
    TestReads();
    TestWriteAt();
    TestFilterAndText();

    return lss::test::Report();
}
