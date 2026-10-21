// Host-side tests for platform/build_translation: translating research-build addresses to another build.

#include "platform/build_translation.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

namespace {

using namespace lss::platform;

// Two ranges: 0x1000..0x1040 moved to 0x2000 (a call's rel32 at +0x10 and a RIP-relative disp32 at +0x20 moved),
// and 0x3000..0x3008 (a data slot) moved to 0x2F00. One hand-found address at 0x5000 -> 0x6000 with its own bytes.
constexpr std::array<MovedField, 2>      kFields{{{0x10, 4}, {0x20, 4}}};
constexpr std::array<TranslatedRange, 2> kRanges{{{0x1000, 0x2000, 0x40, 0, 2}, {0x3000, 0x2F00, 0x8, 2, 0}}};
constexpr std::uint8_t                   kHandFound[] = {0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::array<ReplacedAddress, 1> kReplaced{{{0x5000, 0x6000, kHandFound}}};

constexpr GameBuild kBuild{.name          = L"test",
                           .timeDateStamp = 1,
                           .sizeOfImage   = 2,
                           .checkSum      = 3,
                           .research      = false,
                           .ranges        = kRanges,
                           .fields        = kFields,
                           .replaced      = kReplaced};

constexpr GameBuild kResearchBuild{.name = L"research", .research = true};

void TestTranslate()
{
    std::puts("translation: addresses inside a range move with it, others translate to nothing");
    EXPECT(TranslateRva(kBuild, 0x1000, 1) == 0x2000u);
    EXPECT(TranslateRva(kBuild, 0x1010, 4) == 0x2010u);
    EXPECT(TranslateRva(kBuild, 0x103F, 1) == 0x203Fu);
    EXPECT(!TranslateRva(kBuild, 0x103F, 2));  // runs past the range's end
    EXPECT(!TranslateRva(kBuild, 0x0FFF, 1));  // before the first range
    EXPECT(!TranslateRva(kBuild, 0x1040, 1));  // in the gap
    EXPECT(TranslateRva(kBuild, 0x3000, 8) == 0x2F00u);
    EXPECT(!TranslateRva(kBuild, 0x3004, 8));
    EXPECT(TranslateRva(kBuild, 0x5000, 5) == 0x6000u);  // hand-found
    EXPECT(!TranslateRva(kBuild, 0x5001, 1));            // hand-found addresses translate exactly, not as ranges
    EXPECT(TranslateRva(kResearchBuild, 0x1234, 8) == 0x1234u);
}

void TestResearch()
{
    std::puts("translation: a live RVA back to the research build's");
    EXPECT(ResearchRva(kBuild, 0x2000) == 0x1000u);
    EXPECT(ResearchRva(kBuild, 0x203F) == 0x103Fu);
    EXPECT(!ResearchRva(kBuild, 0x2040));
    EXPECT(ResearchRva(kBuild, 0x2F04) == 0x3004u);
    EXPECT(ResearchRva(kBuild, 0x6000) == 0x5000u);
    EXPECT(ResearchRva(kResearchBuild, 0x77) == 0x77u);
}

void TestMatches()
{
    std::puts("translation: byte checks ignore only the moved fields");
    std::array<std::uint8_t, 0x18> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        expected[i] = static_cast<std::uint8_t>(i + 1);
    }
    std::array<std::uint8_t, 0x18> live = expected;
    // The bytes at research 0x1008..0x1020: the moved field at +0x10..+0x14 is at offset 8 of this window.
    EXPECT(MatchesTranslated(kBuild, 0x1008, expected, live));
    live[8]  = 0xAA;
    live[11] = 0xBB;
    EXPECT(MatchesTranslated(kBuild, 0x1008, expected, live));  // inside the moved field
    live[12] = 0xCC;
    EXPECT(!MatchesTranslated(kBuild, 0x1008, expected, live));  // just past it
    live = expected;
    live[7] = 0xDD;
    EXPECT(!MatchesTranslated(kBuild, 0x1008, expected, live));  // just before it
    EXPECT(!MatchesTranslated(kBuild, 0x1030, expected, live));  // runs past the range's end
    EXPECT(!MatchesTranslated(kBuild, 0x1008, std::span(expected).first(4), live));  // sizes differ

    std::puts("translation: a hand-found address checks its own bytes, as many as the research array is long");
    constexpr std::uint8_t kResearchBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC};
    EXPECT(MatchesTranslated(kBuild, 0x5000, kResearchBytes, kHandFound));
    EXPECT(!MatchesTranslated(kBuild, 0x5000, kResearchBytes, kResearchBytes));
    constexpr std::uint8_t kShort[] = {0x48, 0x89, 0x5C, 0x24};
    EXPECT(MatchesTranslated(kBuild, 0x5000, kShort, std::span(kHandFound).first(4)));  // a shorter research array
    constexpr std::uint8_t kLong[]     = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
    constexpr std::uint8_t kLongLive[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
    EXPECT(!MatchesTranslated(kBuild, 0x5000, kLong, kLongLive));  // more than the build's recorded bytes
    constexpr std::uint8_t kPatched[] = {0x48, 0x89, 0x5C, 0x24, 0x09};
    EXPECT(!MatchesTranslated(kBuild, 0x5000, kResearchBytes, kPatched));  // someone changed the build's bytes

    std::puts("translation: the research build compares every byte");
    EXPECT(MatchesTranslated(kResearchBuild, 0x1008, expected, expected));
    live    = expected;
    live[8] = 0xAA;
    EXPECT(!MatchesTranslated(kResearchBuild, 0x1008, expected, live));
}

void TestFindBuild()
{
    std::puts("builds: the two known builds by their header values, nothing else");
    const GameBuild* research = FindBuild(0x68FD6FDE, 0x09BAD000, 0x07877C99);
    EXPECT(research != nullptr && research->research);
    const GameBuild* next = FindBuild(0x6A16CED2, 0x099D9000, 0x0769EA6E);
    EXPECT(next != nullptr && !next->research && !next->ranges.empty());
    EXPECT(FindBuild(0x6A16CED2, 0x099D9000, 0) == nullptr);
    // The generated table must be sorted and not overlap, or the lookup misses addresses.
    bool sorted = true;
    for (std::size_t i = 1; next != nullptr && i < next->ranges.size(); ++i)
    {
        const TranslatedRange& a = next->ranges[i - 1];
        sorted                   = sorted && a.oldRva + a.length <= next->ranges[i].oldRva;
        sorted = sorted && static_cast<std::size_t>(a.firstField) + a.fieldCount <= next->fields.size();
    }
    EXPECT(sorted);
    // Every moved field lies inside its own range, or a check could skip bytes past the range's end.
    bool inside = true;
    for (std::size_t i = 0; next != nullptr && i < next->ranges.size(); ++i)
    {
        const TranslatedRange& r = next->ranges[i];
        for (std::uint32_t k = 0; k < r.fieldCount && r.firstField + k < next->fields.size(); ++k)
        {
            const MovedField& f = next->fields[r.firstField + k];
            inside              = inside && static_cast<std::uint64_t>(f.offset) + f.size <= r.length;
        }
    }
    EXPECT(inside);
    for (std::size_t i = 1; next != nullptr && i < next->replaced.size(); ++i)
    {
        EXPECT(next->replaced[i - 1].oldRva < next->replaced[i].oldRva);
    }
}

void TestPerBuild()
{
    std::puts("per-build values: the running build's value, the research build's by default");
    constexpr PerBuild<std::uint32_t> kOffset{0x2E8, 0x2F0};
    EXPECT(RunningBuild() == BuildIndex::Research);
    EXPECT(static_cast<std::uint32_t>(kOffset) == 0x2E8u);
    SetRunningBuild(BuildIndex::V1005);
    EXPECT(static_cast<std::uint32_t>(kOffset) == 0x2F0u);
    const std::uint32_t sum = kOffset + 8;  // reads like the plain constant it replaces
    EXPECT(sum == 0x2F8u);
    SetRunningBuild(BuildIndex::Research);
    EXPECT(static_cast<std::uint32_t>(kOffset) == 0x2E8u);
    static_assert(kOffset.For(BuildIndex::V1005) == 0x2F0u);
}

}  // namespace

int main()
{
    TestTranslate();
    TestResearch();
    TestMatches();
    TestFindBuild();
    TestPerBuild();
    return lss::test::Report();
}
