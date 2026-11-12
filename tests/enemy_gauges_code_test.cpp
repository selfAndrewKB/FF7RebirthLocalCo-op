// Host-side tests for the enemy gauges' capture code, run without the game.
//
// The bytes the mod writes near the game (hud/battle/enemy_gauges_code.h) are EXECUTED here, installed
// through platform::CodeRedirect over a stand-in laid out like EndUI__MF8_ProjectEntryByWidget's
// entry: its first instruction ends at a 16-byte boundary and `mov [rsp+18h], r8` starts 1 byte into
// the next block (as at the game's site).

#include "hud/battle/enemy_gauges_code.h"
#include "platform/code_redirect.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>

namespace {

/// nop x12 ; mov [rsp+10h], rbx ; mov [rsp+18h], r8 (offset 17) ; mov rax, [rsp+18h] ; xor rax, rdx ; ret
/// It returns r8 ^ rdx: the replaced instruction ran, and neither argument register changed.
class FunctionLike
{
public:
    static constexpr std::size_t kEntryOffset  = 12;
    static constexpr std::size_t kSiteOffset   = 17;
    static constexpr std::size_t kResumeOffset = 22;

    FunctionLike()
    {
        std::uint8_t code[32] = {};
        std::memset(code, 0x90, kEntryOffset);
        const std::uint8_t body[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10,  // 12: mov [rsp+10h], rbx
            0x4C, 0x89, 0x44, 0x24, 0x18,  // 17: mov [rsp+18h], r8
            0x48, 0x8B, 0x44, 0x24, 0x18,  // 22: mov rax, [rsp+18h]
            0x48, 0x31, 0xD0,              // 27: xor rax, rdx
            0xC3};                         // 30: ret
        std::memcpy(code + kEntryOffset, body, sizeof(body));
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    std::uint64_t  Call(void* manager, std::uint64_t widget, const float* location) const
    {
        using Fn = std::uint64_t (*)(void*, std::uint64_t, const float*);
        return reinterpret_cast<Fn>(At(kEntryOffset))(manager, widget, location);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestCapture()
{
    std::puts("enemy gauges: each call recorded with its widget and location, none while replaying, none past "
              "capacity, and restored");
    const FunctionLike function;
    static lss::hud::GaugeRecord record;
    const float location[3] = {1.5f, -2.25f, 300.0f};
    const auto  expected    = [&](std::uint64_t widget) { return reinterpret_cast<std::uint64_t>(location) ^ widget; };
    EXPECT(function.Call(nullptr, 0x1111, location) == expected(0x1111));

    const auto code = lss::hud::GaugeCaptureCode(record);
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kSiteOffset), lss::hud::kGaugeEntryBytes, code,
                            function.At(FunctionLike::kResumeOffset)));

    record.placements[0].targeted = 7;  // the code must write 0
    EXPECT(function.Call(nullptr, 0xABCDEF, location) == expected(0xABCDEF));
    EXPECT(record.count == 1);
    EXPECT(record.placements[0].widget == 0xABCDEF);
    EXPECT(std::memcmp(record.placements[0].location, location, sizeof(location)) == 0);
    EXPECT(record.placements[0].targeted == 0);

    record.replaying = 1;
    EXPECT(function.Call(nullptr, 0x2222, location) == expected(0x2222));
    EXPECT(record.count == 1);  // the mod's own call is not recorded
    record.replaying = 0;

    for (std::uint32_t i = 1; i < lss::hud::kGaugeRecordCapacity; ++i)
    {
        function.Call(nullptr, 0x3000 + i, location);
    }
    EXPECT(record.count == lss::hud::kGaugeRecordCapacity);
    EXPECT(record.placements[lss::hud::kGaugeRecordCapacity - 1].widget == 0x3000 + lss::hud::kGaugeRecordCapacity - 1);
    EXPECT(function.Call(nullptr, 0x4444, location) == expected(0x4444));  // full: still the game's call
    EXPECT(record.count == lss::hud::kGaugeRecordCapacity);

    EXPECT(redirect.Restore());
    record.count = 0;
    EXPECT(function.Call(nullptr, 0x5555, location) == expected(0x5555));
    EXPECT(record.count == 0);  // restored: nothing recorded
}

}  // namespace

int main()
{
    TestCapture();

    return lss::test::Report();
}
