// Host-side tests for the battle popups' capture code, run without the game.
//
// The bytes the mod writes near the game (hud/battle/popups_code.h) are EXECUTED here, installed through
// platform::CodeRedirect over a stand-in for a popup wrapper's entry: `mov [rsp+10h], rbx` 8 bytes
// into a 16-byte block (as at the game's site), then a body returning rdx ^ r9, so a changed
// argument register shows. The seven arguments are passed as the game passes them.

#include "hud/battle/popups_code.h"
#include "platform/code_redirect.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdio>
#include <cstring>

namespace {

/// nop x8 ; mov [rsp+10h], rbx (offset 8) ; mov rax, rdx ; xor rax, r9 ; ret
class FunctionLike
{
public:
    static constexpr std::size_t kSiteOffset   = 8;
    static constexpr std::size_t kResumeOffset = 13;

    FunctionLike()
    {
        const std::uint8_t code[] = {
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  //  0: nop x8
            0x48, 0x89, 0x5C, 0x24, 0x10,                    //  8: mov [rsp+10h], rbx
            0x48, 0x89, 0xD0,                                // 13: mov rax, rdx
            0x4C, 0x31, 0xC8,                                // 16: xor rax, r9
            0xC3};                                           // 19: ret
        page_.Write(0, code);
        page_.Seal();
    }

    std::uintptr_t At(std::size_t offset) const { return page_.At(offset); }
    std::uint64_t  Call(std::uint64_t controller, const void* key, const float* location, std::uint64_t offset,
                        std::uint8_t clamp) const
    {
        using Fn = std::uint64_t (*)(void*, std::uint64_t, void*, const void*, const float*, std::uint64_t, std::uint8_t);
        float out[2]{};
        void* canvas[2]{};
        return page_.Function<Fn>()(out, controller, canvas, key, location, offset, clamp);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestKind(lss::hud::PopupKey kind)
{
    const bool slate = kind == lss::hud::PopupKey::Slate;
    std::printf("popups (%s): each call recorded with its key, controller, location, offset and clamp; none while "
                "replaying or past capacity; restored\n", slate ? "Slate" : "UMG");
    const FunctionLike function;
    static lss::hud::PopupRecord record;
    record = {};
    const std::uint64_t widget      = 0xAAAA5555;
    const std::uint64_t sharedPtr[] = {widget, 0x1234};  // a TSharedPtr<SWidget>: the Slate key is its object
    const void*         key         = slate ? static_cast<const void*>(sharedPtr) : reinterpret_cast<const void*>(widget);
    const float         location[3] = {100.5f, -200.25f, 30.0f};
    const float         offsetPair[2] = {12.0f, -8.0f};
    std::uint64_t       offset = 0;
    std::memcpy(&offset, offsetPair, sizeof(offset));
    const auto expected = [&](std::uint64_t controller) { return controller ^ reinterpret_cast<std::uint64_t>(key); };

    const auto code = lss::hud::PopupCaptureCode(record, kind);
    lss::platform::CodeRedirect redirect;
    EXPECT(redirect.Install(function.At(FunctionLike::kSiteOffset), lss::hud::kPopupEntryBytes, code,
                            function.At(FunctionLike::kResumeOffset)));

    EXPECT(function.Call(0x77, key, location, offset, 2) == expected(0x77));
    EXPECT(record.count == 1);
    const lss::hud::PopupPlacement& placed = record.placements[0];
    EXPECT(placed.key == widget);
    EXPECT(placed.controller == 0x77);
    EXPECT(std::memcmp(placed.location, location, sizeof(location)) == 0);
    EXPECT(std::memcmp(placed.offset, offsetPair, sizeof(offsetPair)) == 0);
    EXPECT(placed.clamp == 2);

    record.replaying = 1;
    EXPECT(function.Call(0x88, key, location, offset, 1) == expected(0x88));
    EXPECT(record.count == 1);  // the mod's own call is not recorded
    record.replaying = 0;

    for (std::uint32_t i = 1; i < lss::hud::kPopupRecordCapacity; ++i)
    {
        function.Call(i, key, location, offset, 0);
    }
    EXPECT(record.count == lss::hud::kPopupRecordCapacity);
    EXPECT(function.Call(0x99, key, location, offset, 0) == expected(0x99));  // full: still the game's call
    EXPECT(record.count == lss::hud::kPopupRecordCapacity);

    EXPECT(redirect.Restore());
    record.count = 0;
    EXPECT(function.Call(0x55, key, location, offset, 0) == expected(0x55));
    EXPECT(record.count == 0);
}

}  // namespace

int main()
{
    TestKind(lss::hud::PopupKey::Slate);
    TestKind(lss::hud::PopupKey::Umg);

    return lss::test::Report();
}
