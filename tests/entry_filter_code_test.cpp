// Host-side tests for the player-2 entry buffers and the per-family draw gate, run without the game.
//
// The bytes the mod writes near the game (hud/entry_filter_code.h) are EXECUTED here, in small
// harnesses that recreate their surroundings in the game's consume loop:
//   buffers: r9 = the entry, rdi = the renderer (pool 2's vertex buffer at +0x90), the index buffer
//            and SRM slot arguments already stored at [rsp+28h] and [rsp+30h];
//   gate:    rbx = the entry, r9 = its batch array (count at +8), followed by the stock jle.

#include "hud/entry_filter_code.h"
#include "platform/x64_jump.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

using lss::hud::EntryBuffersCode;
using lss::hud::EntryDrawGateCode;
using lss::hud::EntryFilterFields;

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

/// Appends `code` and, where the redirect appends it, a jump to the next instruction.
void AppendWithJumpBack(std::vector<std::uint8_t>& out, std::uintptr_t base, const std::uint8_t* code, std::size_t size)
{
    out.insert(out.end(), code, code + size);
    const auto from = base + out.size();
    const auto back = lss::platform::EncodeJump(from, from + lss::platform::kJumpLength);
    out.insert(out.end(), back->begin(), back->end());
}

struct Arguments
{
    std::uint64_t vertexBuffer = 0;
    std::uint64_t indexBuffer  = 0;
    std::uint64_t srmSlot      = 0;
};

/// void harness(const void* entry /*rcx*/, const void* renderer /*rdx*/, Arguments* out /*r8*/)
using BuffersHarness = void (*)(const void* entry, const void* renderer, Arguments* out);

std::vector<std::uint8_t> BuffersHarnessCode(std::uintptr_t base, const EntryFilterFields& fields)
{
    const std::uint8_t prologue[] = {
        0x57,                                            // push rdi
        0x48, 0x83, 0xEC, 0x40,                          // sub  rsp, 40h
        0x48, 0xC7, 0x44, 0x24, 0x28, 0x11, 0, 0, 0,     // mov  qword [rsp+28h], 11h   stock index buffer
        0x48, 0xC7, 0x44, 0x24, 0x30, 0x22, 0, 0, 0,     // mov  qword [rsp+30h], 22h   stock SRM slot
        0x49, 0x89, 0xC9,                                // mov  r9, rcx                the entry
        0x48, 0x89, 0xD7};                               // mov  rdi, rdx               the renderer
    const std::uint8_t epilogue[] = {
        0x49, 0x89, 0x00,                                // mov  [r8], rax              the vertex buffer
        0x48, 0x8B, 0x44, 0x24, 0x28,                    // mov  rax, [rsp+28h]
        0x49, 0x89, 0x40, 0x08,                          // mov  [r8+8], rax
        0x48, 0x8B, 0x44, 0x24, 0x30,                    // mov  rax, [rsp+30h]
        0x49, 0x89, 0x40, 0x10,                          // mov  [r8+10h], rax
        0x48, 0x83, 0xC4, 0x40,                          // add  rsp, 40h
        0x5F,                                            // pop  rdi
        0xC3};                                           // ret
    std::vector<std::uint8_t> code(std::begin(prologue), std::end(prologue));
    const auto site = EntryBuffersCode(fields);
    AppendWithJumpBack(code, base, site.data(), site.size());
    code.insert(code.end(), std::begin(epilogue), std::end(epilogue));
    return code;
}

/// int harness(const void* entry /*rcx*/, const void* batch /*rdx*/): 1 if the draw would run.
using GateHarness = int (*)(const void* entry, const void* batch);

std::vector<std::uint8_t> GateHarnessCode(std::uintptr_t base, const EntryFilterFields& fields)
{
    const std::uint8_t prologue[] = {
        0x53,                          // push rbx
        0x48, 0x89, 0xCB,              // mov  rbx, rcx     the entry
        0x49, 0x89, 0xD1};             // mov  r9, rdx      the batch array
    const std::uint8_t epilogue[] = {
        0x0F, 0x8E, 0x07, 0, 0, 0,     // jle  skip         the game's own gate
        0xB8, 0x01, 0, 0, 0,           // mov  eax, 1       drawn
        0xEB, 0x05,                    // jmp  end
        0xB8, 0x00, 0, 0, 0,           // skip: mov eax, 0
        0x5B,                          // end: pop rbx
        0xC3};                         // ret
    std::vector<std::uint8_t> code(std::begin(prologue), std::end(prologue));
    const auto site = EntryDrawGateCode(fields);
    AppendWithJumpBack(code, base, site.data(), site.size());
    code.insert(code.end(), std::begin(epilogue), std::end(epilogue));
    return code;
}

/// Builds `make`'s code for a page, then executes it with `run`.
template <class Make, class Run>
void WithPage(const EntryFilterFields& fields, Make&& make, Run&& run)
{
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const auto code = make(reinterpret_cast<std::uintptr_t>(memory), fields);
    std::memcpy(memory, code.data(), code.size());
    DWORD previous = 0;
    VirtualProtect(memory, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), memory, 0x1000);
    run(static_cast<void*>(memory));
    VirtualFree(memory, 0, MEM_RELEASE);
}

Arguments RunBuffers(void* page, std::uint64_t entryName)
{
    std::uint8_t entry[0x20]{};
    std::memcpy(entry, &entryName, sizeof(entryName));
    std::uint8_t renderer[0x100]{};
    const std::uint64_t stockVertexBuffer = 0x33;
    std::memcpy(renderer + 0x90, &stockVertexBuffer, sizeof(stockVertexBuffer));
    Arguments out;
    reinterpret_cast<BuffersHarness>(page)(entry, renderer, &out);
    return out;
}

int RunGate(void* page, std::uint64_t entryName, std::int32_t batchCount)
{
    std::uint8_t entry[0x20]{};
    std::memcpy(entry, &entryName, sizeof(entryName));
    std::uint8_t batch[0x10]{};
    std::memcpy(batch + 8, &batchCount, sizeof(batchCount));
    return reinterpret_cast<GateHarness>(page)(entry, batch);
}

constexpr std::uint64_t kStockName     = 0x0000000000001234;
constexpr std::uint64_t kPlayerTwoName = 0x0000000100001234;  // same text index, another number

void TestBuffers()
{
    std::puts("entry buffers: player 2's entry gets player 2's buffer set, every other entry the pool's");
    EntryFilterFields fields;
    WithPage(fields, BuffersHarnessCode, [](void* page) {
        const auto stock = RunBuffers(page, kPlayerTwoName);  // no player-2 name set yet
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });

    fields.vertexBuffer  = 0xAA;
    fields.indexBuffer   = 0xBB;
    fields.srmSlot       = 0xCC;
    fields.playerTwoName = kPlayerTwoName;
    WithPage(fields, BuffersHarnessCode, [](void* page) {
        const auto own = RunBuffers(page, kPlayerTwoName);
        EXPECT(own.vertexBuffer == 0xAA && own.indexBuffer == 0xBB && own.srmSlot == 0xCC);
        const auto stock = RunBuffers(page, kStockName);
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });
}

void TestDrawGate()
{
    std::puts("draw gate: a family draws only its own entry; with no family set, the game's gate alone");
    EntryFilterFields fields;
    WithPage(fields, GateHarnessCode, [](void* page) {
        EXPECT(RunGate(page, kStockName, 3) == 1);      // no filter: drawn
        EXPECT(RunGate(page, kPlayerTwoName, 3) == 1);
        EXPECT(RunGate(page, kStockName, 0) == 0);      // the game's own gate still applies
    });

    fields.familyName = kStockName;  // player 1's family
    WithPage(fields, GateHarnessCode, [](void* page) {
        EXPECT(RunGate(page, kStockName, 3) == 1);
        EXPECT(RunGate(page, kPlayerTwoName, 3) == 0);  // player 2's entry skipped
        EXPECT(RunGate(page, kStockName, 0) == 0);
    });

    fields.familyName = kPlayerTwoName;  // player 2's family
    WithPage(fields, GateHarnessCode, [](void* page) {
        EXPECT(RunGate(page, kPlayerTwoName, 3) == 1);
        EXPECT(RunGate(page, kStockName, 3) == 0);
    });
}

void TestSizes()
{
    std::puts("code: sizes as declared, and each ends with the instruction it replaces");
    const EntryFilterFields fields;
    const auto buffers = EntryBuffersCode(fields);
    const auto gate    = EntryDrawGateCode(fields);
    EXPECT(std::memcmp(buffers.data() + buffers.size() - sizeof(lss::hud::kLoadVertexBufferBytes),
                       lss::hud::kLoadVertexBufferBytes, sizeof(lss::hud::kLoadVertexBufferBytes)) == 0);
    EXPECT(std::memcmp(gate.data() + gate.size() - sizeof(lss::hud::kDrawGateBytes), lss::hud::kDrawGateBytes,
                       sizeof(lss::hud::kDrawGateBytes)) == 0);
}

}  // namespace

int main()
{
    TestBuffers();
    TestDrawGate();
    TestSizes();

    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
