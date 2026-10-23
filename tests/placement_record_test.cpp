// Host-side tests for hud::PlacementRecord, run without the game.
//
// The record's last-match lookup, the replaying guard, the frame's end, and the shape of the capture code's head:
// the capacity, the two short jumps to `done`, the scale and the replaced instruction where the composed tables
// put them. The composed bytes themselves are executed by popups_code_test and enemy_gauges_code_test.

#include "hud/battle/placement_record.h"

#include "expect.h"

#include <cstdio>

namespace {

struct Placement
{
    std::uint64_t key   = 0;
    std::uint32_t value = 0;
    std::uint8_t  pad[4]{};
};
using Record = lss::hud::PlacementRecord<Placement, 4>;
static_assert(offsetof(Record, replaying) == 4 && offsetof(Record, placements) == 16 && Record::kCapacity == 4);

std::uint64_t KeyOf(const Placement& placement) noexcept
{
    return placement.key;
}

void TestLastAndEndFrame()
{
    std::puts("record: the last placement with a key wins; a count past the capacity is clamped; EndFrame empties it");
    Record record;
    record.placements[0] = {7, 1};
    record.placements[1] = {8, 2};
    record.placements[2] = {7, 3};
    record.count         = 3;
    EXPECT(record.Last(7, &KeyOf) != nullptr && record.Last(7, &KeyOf)->value == 3);
    EXPECT(record.Last(8, &KeyOf) != nullptr && record.Last(8, &KeyOf)->value == 2);
    EXPECT(record.Last(9, &KeyOf) == nullptr);
    record.count = 2;
    EXPECT(record.Last(7, &KeyOf)->value == 1);  // the third entry is beyond the count
    record.count = 99;                           // the code never writes past the capacity; a stale count is clamped
    EXPECT(record.Last(8, &KeyOf)->value == 2);
    record.EndFrame();
    EXPECT(record.count == 0 && record.Last(7, &KeyOf) == nullptr);
}

void TestReplaying()
{
    std::puts("record: the replaying byte is set while the guard lives and cleared after");
    Record record;
    EXPECT(record.replaying == 0);
    {
        const lss::hud::Replaying<Record> guard(record);
        EXPECT(record.replaying == 1);
    }
    EXPECT(record.replaying == 0);
}

void TestCodeShape()
{
    std::puts("record code: the head carries the record's address and capacity, both jumps land on the replaced bytes");
    static Record record;
    constexpr std::uint8_t kPayload[7]  = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
    constexpr std::uint8_t kReplaced[5] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
    constexpr std::array<std::uint8_t, 4> kScale = {0x49, 0xC1, 0xE2, 0x04};  // shl r10, 4
    const auto code = lss::hud::RecordAppendCode(&record, Record::kCapacity, kScale, kPayload, kReplaced);
    EXPECT(code.size() == 32 + 4 + 3 + 7 + 5);
    std::uint64_t address = 0;
    std::memcpy(&address, &code[2], sizeof(address));
    EXPECT(address == reinterpret_cast<std::uint64_t>(&record));
    EXPECT(code[0] == 0x48 && code[1] == 0xB8);                 // mov rax, imm64
    EXPECT(code[10] == 0x80 && code[12] == 0x04);               // cmp byte [rax+4], 0
    EXPECT(code[22] == Record::kCapacity);                      // cmp r10d, capacity
    constexpr std::size_t kDone = 32 + 4 + 3 + 7;
    EXPECT(code[14] == 0x75 && 16 + code[15] == kDone);         // jne done
    EXPECT(code[23] == 0x73 && 25 + code[24] == kDone);         // jae done
    EXPECT(code[32] == 0x49 && code[35] == 0x04);               // the scale
    EXPECT(code[36] == 0x4C && code[37] == 0x01 && code[38] == 0xD0);  // add rax, r10
    EXPECT(std::memcmp(&code[39], kPayload, sizeof(kPayload)) == 0);
    EXPECT(std::memcmp(&code[kDone], kReplaced, sizeof(kReplaced)) == 0);
}

}  // namespace

int main()
{
    TestLastAndEndFrame();
    TestReplaying();
    TestCodeShape();

    return lss::test::Report();
}
