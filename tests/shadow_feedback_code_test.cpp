// Host-side tests for the ME shadow feedback skip code (renderer/me_shadow_feedback_code.h), run without the game.
//
// The bytes the mod places over the `jbe` entering the shadow pass's feedback region are EXECUTED here, for both
// builds, in a harness that recreates the site: the view in r13 (research) or r12 (1.005), its family copy's
// bAdditionalViewFamily at +0x74, the chunk count in [r14+18h], and for 1.005 the transition array at [rbp-68h].
// Where the code jumps (the array's free, the jbe's target) and where it resumes, stubs return a distinct value:
//   0xF   fell through: the count is not zero, the region runs (the game's path)
//   0xE   jumped to the jbe's target: the count is zero
//   skip  jumped to the array's free, with the registers that code needs: r15d = 30h (research, returned), or
//         rbx = the array and edi = 0 (1.005, returned as rbx << 8 | edi)
// and the counters in the fields say which path counted itself.

#include "renderer/me_shadow_feedback_code.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

using lss::renderer::kShadowFeedbackCodeSize;
using lss::renderer::ShadowFeedbackCode;
using lss::renderer::ShadowFeedbackCode1005;
using lss::renderer::ShadowFeedbackFields;

constexpr std::uint64_t kFellThrough = 0xF;
constexpr std::uint64_t kEmpty       = 0xE;
constexpr std::uint64_t kArray       = 0x11;  // what [rbp-68h] holds for 1.005
constexpr std::uint64_t kSkipResearch = 0x30;
constexpr std::uint64_t kSkip1005     = kArray << 8;

/// std::uint64_t harness(void* view /*rcx -> r13*/, void* chunk /*rdx -> r14*/, void* view /*r8 -> r12*/,
///                       void* frame /*r9 -> rbp*/)
using Harness = std::uint64_t (*)(void* view, void* chunk, void* viewAgain, void* frame);

ShadowFeedbackFields g_fields;  // the code embeds its address

class SiteLike
{
public:
    explicit SiteLike(bool v1005)
    {
        std::vector<std::uint8_t> code;
        const auto put = [&code](std::initializer_list<std::uint8_t> bytes) { code.insert(code.end(), bytes); };
        put({0x53, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push rbx rbp rdi r12 r13 r14 r15
        put({0x49, 0x89, 0xCD});                                                  // mov r13, rcx   the view
        put({0x49, 0x89, 0xD6});                                                  // mov r14, rdx   the chunk
        put({0x4D, 0x89, 0xC4});                                                  // mov r12, r8    the view (1.005)
        put({0x4C, 0x89, 0xCD});                                                  // mov rbp, r9    the frame
        put({0x45, 0x31, 0xFF});                                                  // xor r15d, r15d
        put({0xBF, 0x07, 0x00, 0x00, 0x00});                                      // mov edi, 7
        put({0x31, 0xDB});                                                        // xor ebx, ebx
        const std::size_t codeAt = code.size();
        code.resize(codeAt + kShadowFeedbackCodeSize);  // the mod's bytes, once the stubs' addresses are known
        // resume: the byte after the code, where the redirect appends its jump back
        put({0xB8, 0x0F, 0x00, 0x00, 0x00});  // mov eax, 0Fh
        const std::size_t jumpFromResume = code.size();
        put({0xEB, 0x00});
        const std::size_t skipAt = code.size();
        if (v1005)
        {
            put({0x48, 0x89, 0xD8, 0x48, 0xC1, 0xE0, 0x08, 0x09, 0xF8});  // mov rax, rbx ; shl rax, 8 ; or eax, edi
        }
        else
        {
            put({0x44, 0x89, 0xF8});  // mov eax, r15d
        }
        const std::size_t jumpFromSkip = code.size();
        put({0xEB, 0x00});
        const std::size_t emptyAt = code.size();
        put({0xB8, 0x0E, 0x00, 0x00, 0x00});  // mov eax, 0Eh (then the epilogue)
        const std::size_t epilogue = code.size();
        put({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5D, 0x5B, 0xC3});  // pops, ret
        code[jumpFromResume + 1] = static_cast<std::uint8_t>(epilogue - (jumpFromResume + 2));
        code[jumpFromSkip + 1]   = static_cast<std::uint8_t>(epilogue - (jumpFromSkip + 2));

        const auto bytes = v1005 ? ShadowFeedbackCode1005(g_fields, page_.At(skipAt), page_.At(emptyAt))
                                 : ShadowFeedbackCode(g_fields, page_.At(skipAt), page_.At(emptyAt));
        std::memcpy(code.data() + codeAt, bytes.data(), bytes.size());
        page_.Write(0, code);
        page_.Seal();
    }

    std::uint64_t Run(bool additionalFamily, std::uint64_t skip, std::uint32_t chunkCount) const
    {
        alignas(16) std::uint8_t family[0x80] = {};
        family[0x74]                          = additionalFamily ? 1 : 0;
        void* view[2]                         = {family, nullptr};  // FViewInfo+0: the renderer's family copy
        alignas(8) std::uint8_t chunk[0x20]   = {};
        std::memcpy(chunk + 0x18, &chunkCount, sizeof(chunkCount));
        alignas(16) std::uint8_t frame[0x100] = {};
        std::memcpy(frame + 0x80 - 0x68, &kArray, sizeof(kArray));  // [rbp-68h]
        g_fields.skip = skip;
        return page_.Function<Harness>()(view, chunk, view, frame + 0x80);
    }

private:
    lss::test::ExecutablePage page_;
};

void TestPaths(const char* build, bool v1005)
{
    std::printf("shadow feedback code (%s): player 1's family always runs the region; player 2's runs it with the "
                "switch off and skips to the free with it on; an empty count takes the jbe; each path counts\n",
                build);
    const SiteLike site(v1005);
    const auto     skipValue = v1005 ? kSkip1005 : kSkipResearch;
    g_fields = {};

    EXPECT(site.Run(false, 0, 5) == kFellThrough);
    EXPECT(g_fields.passedOne == 1 && g_fields.passedTwo == 0 && g_fields.skipped == 0);
    EXPECT(site.Run(false, 0, 0) == kEmpty);
    EXPECT(g_fields.passedOne == 2);
    EXPECT(site.Run(false, 1, 5) == kFellThrough);  // the switch does not touch player 1's family
    EXPECT(g_fields.passedOne == 3 && g_fields.skipped == 0);

    EXPECT(site.Run(true, 0, 5) == kFellThrough);
    EXPECT(g_fields.passedTwo == 1 && g_fields.skipped == 0);
    EXPECT(site.Run(true, 0, 0) == kEmpty);
    EXPECT(g_fields.passedTwo == 2);

    EXPECT(site.Run(true, 1, 5) == skipValue);
    EXPECT(g_fields.skipped == 1 && g_fields.passedTwo == 2 && g_fields.passedOne == 3);
    EXPECT(site.Run(true, 1, 0) == skipValue);  // skipped whatever the count
    EXPECT(g_fields.skipped == 2);
    EXPECT(site.Run(true, 0x100, 5) == kFellThrough);  // only the switch's low byte is read
    EXPECT(g_fields.passedTwo == 3 && g_fields.skipped == 2);
}

void TestTheBytes()
{
    std::puts("shadow feedback code: the two builds differ only in the view's register and the free's fix-up");
    const auto research = ShadowFeedbackCode(g_fields, 0x1111, 0x2222);
    const auto v1005    = ShadowFeedbackCode1005(g_fields, 0x1111, 0x2222);
    EXPECT(std::memcmp(research.data() + 4, v1005.data() + 4, 26) == 0);
    EXPECT(std::memcmp(research.data() + 36, v1005.data() + 36, research.size() - 36) == 0);
    std::uint64_t embedded = 0;
    std::memcpy(&embedded, research.data() + 10, sizeof(embedded));
    EXPECT(embedded == reinterpret_cast<std::uint64_t>(&g_fields));
    std::memcpy(&embedded, research.data() + 42, sizeof(embedded));
    EXPECT(embedded == 0x1111);
    std::memcpy(&embedded, research.data() + 75, sizeof(embedded));
    EXPECT(embedded == 0x2222);
}

}  // namespace

int main()
{
    TestTheBytes();
    TestPaths("research", false);
    TestPaths("1.005", true);

    return lss::test::Report();
}
