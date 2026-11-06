// Host-side tests for the seats' entry buffers and the per-family draw gate, run without the game.
//
// The bytes the mod writes near the game (hud/passes/entry_filter_code.h) are EXECUTED here, in small
// harnesses that recreate their surroundings in the game's consume loop:
//   buffers: r9 = the entry, rdi = the renderer (pool 2's vertex buffer at +0x90), the index buffer
//            and SRM slot arguments already stored at [rsp+28h] and [rsp+30h];
//   gate:    rbx = the entry, r9 = its batch array (count at +8), followed by the stock jle.

#include "hud/passes/entry_filter_code.h"
#include "platform/x64_jump.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace {

using lss::hud::EntryBuffersCode;
using lss::hud::EntryDrawGateCode;
using lss::hud::EntryFilterFields;

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

/// Pool 2's site has the renderer in rdi (its vertex buffer at +0x90); pool 0's in r13 (at +0x80).
template <bool PoolZero>
std::vector<std::uint8_t> BuffersHarnessCode(std::uintptr_t base, const EntryFilterFields& fields)
{
    const std::uint8_t save[]    = {0x57};              // push rdi
    const std::uint8_t saveR13[] = {0x41, 0x55};        // push r13
    const std::uint8_t body[] = {
        0x48, 0x83, 0xEC, 0x40,                          // sub  rsp, 40h
        0x48, 0xC7, 0x44, 0x24, 0x28, 0x11, 0, 0, 0,     // mov  qword [rsp+28h], 11h   stock index buffer
        0x48, 0xC7, 0x44, 0x24, 0x30, 0x22, 0, 0, 0,     // mov  qword [rsp+30h], 22h   stock SRM slot
        0x49, 0x89, 0xC9};                               // mov  r9, rcx                the entry
    const std::uint8_t renderer[]    = {0x48, 0x89, 0xD7};  // mov rdi, rdx   the renderer
    const std::uint8_t rendererR13[] = {0x49, 0x89, 0xD5};  // mov r13, rdx
    const std::uint8_t epilogue[] = {
        0x49, 0x89, 0x00,                                // mov  [r8], rax              the vertex buffer
        0x48, 0x8B, 0x44, 0x24, 0x28,                    // mov  rax, [rsp+28h]
        0x49, 0x89, 0x40, 0x08,                          // mov  [r8+8], rax
        0x48, 0x8B, 0x44, 0x24, 0x30,                    // mov  rax, [rsp+30h]
        0x49, 0x89, 0x40, 0x10,                          // mov  [r8+10h], rax
        0x48, 0x83, 0xC4, 0x40};                         // add  rsp, 40h
    const std::uint8_t restore[]    = {0x5F, 0xC3};        // pop rdi ; ret
    const std::uint8_t restoreR13[] = {0x41, 0x5D, 0xC3};  // pop r13 ; ret
    std::vector<std::uint8_t> code;
    auto append = [&code](const auto& bytes) { code.insert(code.end(), std::begin(bytes), std::end(bytes)); };
    PoolZero ? append(saveR13) : append(save);
    append(body);
    PoolZero ? append(rendererR13) : append(renderer);
    const auto site = PoolZero ? EntryBuffersCode(fields, lss::hud::kLoadPoolZeroVertexBufferBytes) : EntryBuffersCode(fields);
    AppendWithJumpBack(code, base, site.data(), site.size());
    append(epilogue);
    PoolZero ? append(restoreR13) : append(restore);
    return code;
}

/// Game version 1.005's pool 0 site: the store of the vertex-buffer argument, after the game's load into rax.
std::vector<std::uint8_t> BuffersStoreHarnessCode1005(std::uintptr_t base, const EntryFilterFields& fields)
{
    const std::uint8_t prologue[] = {
        0x41, 0x55,                                      // push r13
        0x48, 0x83, 0xEC, 0x40,                          // sub  rsp, 40h
        0x48, 0xC7, 0x44, 0x24, 0x28, 0x11, 0, 0, 0,     // mov  qword [rsp+28h], 11h   stock index buffer
        0x48, 0xC7, 0x44, 0x24, 0x30, 0x22, 0, 0, 0,     // mov  qword [rsp+30h], 22h   stock SRM slot
        0x48, 0xC7, 0x44, 0x24, 0x20, 0, 0, 0, 0,        // mov  qword [rsp+20h], 0     the argument the site stores
        0x49, 0x89, 0xC9,                                // mov  r9, rcx                the entry
        0x49, 0x89, 0xD5,                                // mov  r13, rdx               the renderer
        0x49, 0x8B, 0x85, 0x80, 0x00, 0x00, 0x00};       // mov  rax, [r13+80h]         the game's load before the site
    const std::uint8_t epilogue[] = {
        0x48, 0x8B, 0x44, 0x24, 0x20,                    // mov  rax, [rsp+20h]         the vertex buffer argument
        0x49, 0x89, 0x00,                                // mov  [r8], rax
        0x48, 0x8B, 0x44, 0x24, 0x28,                    // mov  rax, [rsp+28h]
        0x49, 0x89, 0x40, 0x08,                          // mov  [r8+8], rax
        0x48, 0x8B, 0x44, 0x24, 0x30,                    // mov  rax, [rsp+30h]
        0x49, 0x89, 0x40, 0x10,                          // mov  [r8+10h], rax
        0x48, 0x83, 0xC4, 0x40,                          // add  rsp, 40h
        0x41, 0x5D, 0xC3};                               // pop r13 ; ret
    std::vector<std::uint8_t> code(std::begin(prologue), std::end(prologue));
    const auto site = lss::hud::EntryBuffersStoreCode1005(fields);
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
    lss::test::ExecutablePage page;
    const auto code = make(page.At(), fields);
    page.Write(0, code);
    page.Seal();
    run(reinterpret_cast<void*>(page.At()));
}

Arguments RunBuffers(void* page, std::uint64_t entryName)
{
    std::uint8_t entry[0x20]{};
    std::memcpy(entry, &entryName, sizeof(entryName));
    std::uint8_t renderer[0x100]{};
    const std::uint64_t stockVertexBuffer = 0x33;
    std::memcpy(renderer + 0x90, &stockVertexBuffer, sizeof(stockVertexBuffer));  // pool 2's
    std::memcpy(renderer + 0x80, &stockVertexBuffer, sizeof(stockVertexBuffer));  // pool 0's
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
constexpr std::uint64_t kPlayerTwoName   = 0x0000000100001234;  // any other value: only equality is compared
constexpr std::uint64_t kPlayerThreeName = 0x0000000200001234;
constexpr std::uint64_t kMinimapName   = 0x0000000000005678;  // an entry of another pool ("SEndNaviMapCircle")

template <bool PoolZero>
void TestBuffers()
{
    std::puts(PoolZero ? "entry buffers (pool 0, the minimap's map): player 2's entry gets player 2's buffer set"
                       : "entry buffers: player 2's entry gets player 2's buffer set, every other entry the pool's");
    EntryFilterFields fields;
    WithPage(fields, BuffersHarnessCode<PoolZero>, [](void* page) {
        const auto stock = RunBuffers(page, kPlayerTwoName);  // no player-2 name set yet
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });

    fields.Seat(1) = {kPlayerTwoName, 0xAA, 0xBB, 0xCC};
    WithPage(fields, BuffersHarnessCode<PoolZero>, [](void* page) {
        const auto own = RunBuffers(page, kPlayerTwoName);
        EXPECT(own.vertexBuffer == 0xAA && own.indexBuffer == 0xBB && own.srmSlot == 0xCC);
        const auto stock = RunBuffers(page, kStockName);
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
        const auto unset = RunBuffers(page, kPlayerThreeName);  // seat 2 has no entry yet
        EXPECT(unset.vertexBuffer == 0x33 && unset.indexBuffer == 0x11 && unset.srmSlot == 0x22);
    });

    EntryFilterFields leftOne;  // player 2 left, player 3 stays: the walk passes the unset first entry
    leftOne.Seat(2) = {kPlayerThreeName, 0xA3, 0xB3, 0xC3};
    WithPage(leftOne, BuffersHarnessCode<PoolZero>, [](void* page) {
        const auto three = RunBuffers(page, kPlayerThreeName);
        EXPECT(three.vertexBuffer == 0xA3 && three.indexBuffer == 0xB3 && three.srmSlot == 0xC3);
        const auto stock = RunBuffers(page, kPlayerTwoName);
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });

    fields.Seat(2) = {kPlayerThreeName, 0xA3, 0xB3, 0xC3};  // both seats: each entry its own seat's set
    WithPage(fields, BuffersHarnessCode<PoolZero>, [](void* page) {
        const auto two = RunBuffers(page, kPlayerTwoName);
        EXPECT(two.vertexBuffer == 0xAA && two.indexBuffer == 0xBB && two.srmSlot == 0xCC);
        const auto three = RunBuffers(page, kPlayerThreeName);
        EXPECT(three.vertexBuffer == 0xA3 && three.indexBuffer == 0xB3 && three.srmSlot == 0xC3);
        const auto stock = RunBuffers(page, kStockName);
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });
}

void TestBuffersStore1005()
{
    std::puts("entry buffers (1.005, pool 0 at the argument store): player 2's entry gets player 2's buffer set");
    EntryFilterFields fields;
    WithPage(fields, BuffersStoreHarnessCode1005, [](void* page) {
        const auto stock = RunBuffers(page, kPlayerTwoName);  // no player-2 name set yet
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });

    fields.Seat(1) = {kPlayerTwoName, 0xAA, 0xBB, 0xCC};
    fields.Seat(2) = {kPlayerThreeName, 0xA3, 0xB3, 0xC3};
    WithPage(fields, BuffersStoreHarnessCode1005, [](void* page) {
        const auto own = RunBuffers(page, kPlayerTwoName);
        EXPECT(own.vertexBuffer == 0xAA && own.indexBuffer == 0xBB && own.srmSlot == 0xCC);
        const auto three = RunBuffers(page, kPlayerThreeName);
        EXPECT(three.vertexBuffer == 0xA3 && three.indexBuffer == 0xB3 && three.srmSlot == 0xC3);
        const auto stock = RunBuffers(page, kStockName);
        EXPECT(stock.vertexBuffer == 0x33 && stock.indexBuffer == 0x11 && stock.srmSlot == 0x22);
    });
}

void TestDrawGate()
{
    std::puts("draw gate: a family skips only the other player's overlay entry; other entries, the game's gate alone");
    EntryFilterFields fields;
    fields.stockName    = kStockName;
    fields.Seat(1).name = kPlayerTwoName;
    WithPage(fields, GateHarnessCode, [](void* page) {
        EXPECT(RunGate(page, kStockName, 3) == 1);      // no family set: drawn
        EXPECT(RunGate(page, kPlayerTwoName, 3) == 1);
        EXPECT(RunGate(page, kMinimapName, 3) == 1);
        EXPECT(RunGate(page, kStockName, 0) == 0);      // the game's own gate still applies
    });

    fields.familyName = kStockName;  // player 1's family
    WithPage(fields, GateHarnessCode, [](void* page) {
        EXPECT(RunGate(page, kStockName, 3) == 1);
        EXPECT(RunGate(page, kPlayerTwoName, 3) == 0);  // player 2's entry skipped
        EXPECT(RunGate(page, kStockName, 0) == 0);
        EXPECT(RunGate(page, kMinimapName, 3) == 1);    // another pool's entry: drawn
        EXPECT(RunGate(page, kMinimapName, 0) == 0);
    });

    fields.familyName = kPlayerTwoName;  // player 2's family
    WithPage(fields, GateHarnessCode, [](void* page) {
        EXPECT(RunGate(page, kPlayerTwoName, 3) == 1);
        EXPECT(RunGate(page, kStockName, 3) == 0);
        EXPECT(RunGate(page, kMinimapName, 3) == 1);
        EXPECT(RunGate(page, kPlayerThreeName, 3) == 1);  // seat 2 has no entry: not an overlay entry to this gate
    });

    std::puts("draw gate, player 2 left and player 3 stays: player 3's family skips player 1's entry, and back");
    {
        EntryFilterFields leftOne;
        leftOne.stockName    = kStockName;
        leftOne.Seat(2).name = kPlayerThreeName;
        leftOne.familyName   = kPlayerThreeName;
        WithPage(leftOne, GateHarnessCode, [](void* page) {
            EXPECT(RunGate(page, kPlayerThreeName, 3) == 1);
            EXPECT(RunGate(page, kStockName, 3) == 0);
            EXPECT(RunGate(page, kMinimapName, 3) == 1);
        });
        leftOne.familyName = kStockName;
        WithPage(leftOne, GateHarnessCode, [](void* page) {
            EXPECT(RunGate(page, kStockName, 3) == 1);
            EXPECT(RunGate(page, kPlayerThreeName, 3) == 0);
        });
    }

    std::puts("draw gate, three players: each family draws its own entry and skips the two others");
    fields.Seat(2).name = kPlayerThreeName;
    for (const std::uint64_t family : {kStockName, kPlayerTwoName, kPlayerThreeName})
    {
        fields.familyName = family;
        WithPage(fields, GateHarnessCode, [family](void* page) {
            for (const std::uint64_t entry : {kStockName, kPlayerTwoName, kPlayerThreeName})
            {
                EXPECT(RunGate(page, entry, 3) == (entry == family ? 1 : 0));
            }
            EXPECT(RunGate(page, kMinimapName, 3) == 1);
            EXPECT(RunGate(page, family, 0) == 0);  // the game's own gate still applies
        });
    }
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
    const auto store = lss::hud::EntryBuffersStoreCode1005(fields);
    EXPECT(std::memcmp(store.data() + store.size() - sizeof(lss::hud::kStoreVertexBufferArgBytes1005),
                       lss::hud::kStoreVertexBufferArgBytes1005, sizeof(lss::hud::kStoreVertexBufferArgBytes1005)) == 0);
}

}  // namespace

int main()
{
    TestBuffers<false>();
    TestBuffers<true>();
    TestBuffersStore1005();
    TestDrawGate();
    TestSizes();

    return lss::test::Report();
}
