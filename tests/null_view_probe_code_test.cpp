// Host-side tests for the dead-view probe (renderer/null_view_probe_code.h), run without the game.
//
// The bytes the mod writes near the game are EXECUTED here, installed through platform::CodeRedirect
// over a stand-in laid out like the resource-array bind (0x140816AEB..0x140816AFA in the game): it
// sets the loop's state (rsi the view, r8 the context, r15 the table, the stage at [rsp+0C0h], rbx
// something else, r12/rbp the slot and element, rdx = element * 8), loads the view's handle into rax
// and stores it into the table, then runs the site's `cmp byte [rsi+80h], 0` inside an aligned
// block. Its paths return 1 (the game's path for a view, byte clear), 3 (byte set) and, apart, 2 (the
// game's next element after an empty one, where the probe goes on).

#include "platform/code_redirect.h"
#include "renderer/null_view_probe_code.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

constexpr std::uint64_t kStage   = 5;  // [rsp+0C0h]: the pixel stage
constexpr std::uint64_t kSlot    = 3;  // r12
constexpr std::uint64_t kElement = 2;  // rbp

/// Run(view in rcx, context in rdx, table in r8): see the file comment.
class BindLike
{
public:
    static constexpr std::size_t kSiteOffset   = 0x43;
    static constexpr std::size_t kResumeOffset = 0x4A;
    static constexpr std::size_t kEmptyOffset  = 0x70;

    BindLike()
    {
        std::uint8_t code[0x80];
        std::memset(code, 0x90, sizeof(code));
        const std::uint8_t entry[] = {
            0x56, 0x41, 0x57, 0x53, 0x41, 0x54, 0x55,        // 0x00: push rsi, r15, rbx, r12, rbp
            0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00,        // 0x07: sub  rsp, 0xC8
            0x48, 0x89, 0xCE,                                // 0x0E: mov  rsi, rcx
            0x4D, 0x89, 0xC7,                                // 0x11: mov  r15, r8
            0x49, 0x89, 0xD0,                                // 0x14: mov  r8, rdx
            0x48, 0xC7, 0x84, 0x24, 0xC0, 0x00, 0x00, 0x00,  // 0x17: mov  qword [rsp+0C0h], stage
            kStage, 0x00, 0x00, 0x00,
            0xBB, 0x34, 0x12, 0x00, 0x00,                    // 0x23: mov  ebx, 0x1234 (the loop reuses rbx)
            0x41, 0xBC, kSlot, 0x00, 0x00, 0x00,             // 0x28: mov  r12d, slot
            0xBD, kElement, 0x00, 0x00, 0x00,                // 0x2E: mov  ebp, element
            0x48, 0x8D, 0x14, 0xED, 0x00, 0x00, 0x00, 0x00,  // 0x33: lea  rdx, [rbp*8]
            0x48, 0x8B, 0x46, 0x10,                          // 0x3B: mov  rax, [rsi+10h]
            0x4A, 0x89, 0x04, 0x3A,                          // 0x3F: mov  [rdx+r15], rax
            0x80, 0xBE, 0x80, 0x00, 0x00, 0x00, 0x00,        // 0x43: cmp  byte [rsi+80h], 0 (the site)
            0x75, 0x0C,                                      // 0x4A: jne  0x58
            0xB8, 0x01, 0x00, 0x00, 0x00,                    // 0x4C: mov  eax, 1
            0xEB, 0x0D,                                      // 0x51: jmp  0x60
        };
        const std::uint8_t flagged[] = {0xB8, 0x03, 0x00, 0x00, 0x00,  // 0x58: mov eax, 3
                                        0xEB, 0x01};                   // 0x5D: jmp 0x60
        const std::uint8_t done[]    = {0x48, 0x81, 0xC4, 0xC8, 0x00, 0x00, 0x00,  // 0x60: add rsp, 0xC8
                                        0x5D, 0x41, 0x5C, 0x5B, 0x41, 0x5F, 0x5E,  // 0x67: pop rbp, r12, rbx, r15, rsi
                                        0xC3};                                     // 0x6E: ret
        const std::uint8_t empty[]   = {0xB8, 0x02, 0x00, 0x00, 0x00,  // 0x70: mov eax, 2
                                        0xEB, 0xE9};                   // 0x75: jmp 0x60
        static_assert(sizeof(entry) == 0x53);
        std::memcpy(code, entry, sizeof(entry));
        std::memcpy(code + 0x58, flagged, sizeof(flagged));
        std::memcpy(code + 0x60, done, sizeof(done));
        std::memcpy(code + kEmptyOffset, empty, sizeof(empty));
        memory_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        std::memcpy(memory_, code, sizeof(code));
        DWORD previous = 0;
        VirtualProtect(memory_, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), memory_, 0x1000);
    }
    ~BindLike() { VirtualFree(memory_, 0, MEM_RELEASE); }

    std::uintptr_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(memory_) + offset; }
    std::uint64_t  Run(void* view, void* context, std::uint64_t* table) const
    {
        using Fn = std::uint64_t (*)(void*, void*, std::uint64_t*);
        return reinterpret_cast<Fn>(At(0))(view, context, table);
    }

private:
    std::uint8_t* memory_ = nullptr;
};

/// A view laid out as the bind reads it: +0 vtable, +0x10 handle, +0x40 resource, +0x80 a byte.
struct alignas(16) View
{
    std::uint8_t bytes[0x90] = {};

    void Set(std::size_t at, std::uint64_t value) { std::memcpy(bytes + at, &value, sizeof(value)); }
};

void TestProbe()
{
    std::puts("dead-view probe: a view with a handle binds as the game's; a view with none is recorded and bound as "
              "the game's empty element; restored");
    const BindLike bind;

    // The context: [ctx+0x20] points at a block whose +8 is the null descriptor.
    std::uint64_t nullBlock[2] = {0, 0xD00D};
    std::uint64_t context[5]   = {};
    context[4]                 = reinterpret_cast<std::uint64_t>(nullBlock);
    std::uint64_t resource[1]  = {0x5E5};

    View live;
    live.Set(0x00, 0x7AB1E);
    live.Set(0x10, 0xABCD);
    live.Set(0x40, reinterpret_cast<std::uint64_t>(resource));
    View dead = live;
    dead.Set(0x10, 0);

    std::uint64_t table[4] = {};
    EXPECT(bind.Run(&live, context, table) == 1);
    EXPECT(table[kElement] == 0xABCD);
    EXPECT(bind.Run(&dead, context, table) == 1);  // stock: the 0 handle goes on to the driver
    EXPECT(table[kElement] == 0);

    static lss::renderer::NullViewProbe probe;
    const auto code = lss::renderer::NullViewProbeCode(probe, bind.At(BindLike::kEmptyOffset));
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(bind.At(BindLike::kSiteOffset), lss::renderer::kNullViewProbeSiteBytes, code,
                            bind.At(BindLike::kResumeOffset)));

    EXPECT(bind.Run(&live, context, table) == 1);  // a handle: the game's path, the replaced compare included
    EXPECT(table[kElement] == 0xABCD);
    live.bytes[0x80] = 1;
    EXPECT(bind.Run(&live, context, table) == 3);
    live.bytes[0x80] = 0;
    EXPECT(probe.hits == 0);

    EXPECT(bind.Run(&dead, context, table) == 2);  // no handle: the empty element's path
    EXPECT(table[kElement] == 0xD00D);             // the game's null descriptor
    EXPECT(probe.hits == 1);
    EXPECT(probe.view == reinterpret_cast<std::uint64_t>(&dead));
    EXPECT(probe.resource == reinterpret_cast<std::uint64_t>(resource));
    EXPECT(probe.vtable == 0x7AB1E);
    EXPECT(probe.stage == kStage);
    EXPECT(probe.slot == kSlot);
    EXPECT(probe.element == kElement);
    EXPECT(bind.Run(&dead, context, table) == 2);
    EXPECT(probe.hits == 2);

    EXPECT(redirect.Restore());
    EXPECT(bind.Run(&dead, context, table) == 1);  // restored: stock again
    EXPECT(probe.hits == 2);
}

}  // namespace

int main()
{
    TestProbe();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
