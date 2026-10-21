// Host-side tests for renderer::SubmitOneFamilyPerView, run without the game.
//
// A fake FSceneViewFamily is laid out with the offsets from renderer/scene_view_family.h, and a
// fake engine submission records what each call saw. The tests check what the renderer is shown
// on each submission and that the family comes back byte-for-byte as the engine built it.

#include "renderer/one_family_per_view.h"
#include "renderer/scene_view_family.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using lss::renderer::SubmitOneFamilyPerView;
namespace family = lss::renderer::family;

/// What the engine saw on one submission.
struct Seen
{
    const void*   firstView;
    std::int32_t  viewCount;
    std::int32_t  skipFrame;
    std::uint8_t  additional;
};

std::vector<Seen> g_seen;
std::string       g_order;  // 'S' per submission, 'B' per between-families call

void Between()
{
    g_order += 'B';
}

void RecordingSubmit(void*, void*, std::byte* viewFamily)
{
    g_order += 'S';
    Seen seen{};
    std::memcpy(&seen.viewCount, viewFamily + family::kViewsCount, sizeof(seen.viewCount));
    std::memcpy(&seen.skipFrame, viewFamily + family::kSkipFrameNumberIncrement, sizeof(seen.skipFrame));
    std::memcpy(&seen.additional, viewFamily + family::kAdditionalViewFamily, sizeof(seen.additional));
    const void** views = nullptr;
    std::memcpy(&views, viewFamily + family::kViewsData, sizeof(views));
    seen.firstView = views != nullptr ? views[0] : nullptr;
    g_seen.push_back(seen);
}

/// A fake family holding `views`, with the engine's own values for the two flags.
struct FakeFamily
{
    std::array<std::byte, 0x100> memory{};

    FakeFamily(std::vector<const void*>& views, std::int32_t engineSkip, std::uint8_t engineAdditional)
    {
        const void** data  = views.empty() ? nullptr : views.data();
        const auto   count = static_cast<std::int32_t>(views.size());
        std::memcpy(memory.data() + family::kViewsData, &data, sizeof(data));
        std::memcpy(memory.data() + family::kViewsCount, &count, sizeof(count));
        std::memcpy(memory.data() + family::kSkipFrameNumberIncrement, &engineSkip, sizeof(engineSkip));
        std::memcpy(memory.data() + family::kAdditionalViewFamily, &engineAdditional, sizeof(engineAdditional));
    }
};

void TestOneViewIsPassedThroughUnchanged()
{
    std::puts("one view: a single, unchanged submission");
    int                      a = 0;
    std::vector<const void*> views{&a};
    FakeFamily               fake(views, 0, 0);
    const auto               before = fake.memory;

    g_seen.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fake.memory.data(), &RecordingSubmit);

    EXPECT(g_seen.size() == 1);
    EXPECT(g_seen[0].viewCount == 1 && g_seen[0].skipFrame == 0 && g_seen[0].additional == 0);
    EXPECT(fake.memory == before);
}

void TestTwoViewsBecomeTwoFamilies()
{
    std::puts("two views: two single-view submissions, the second flagged");
    int                      a = 0, b = 0;
    std::vector<const void*> views{&a, &b};
    FakeFamily               fake(views, 0, 0);
    const auto               before = fake.memory;

    g_seen.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fake.memory.data(), &RecordingSubmit);

    EXPECT(g_seen.size() == 2);
    EXPECT(g_seen[0].firstView == &a && g_seen[0].viewCount == 1);
    EXPECT(g_seen[0].skipFrame == 0 && g_seen[0].additional == 0);  // the engine's own values
    EXPECT(g_seen[1].firstView == &b && g_seen[1].viewCount == 1);
    EXPECT(g_seen[1].skipFrame == 1 && g_seen[1].additional == 1);
    EXPECT(fake.memory == before);
    EXPECT(views[0] == &a && views[1] == &b);  // the engine's view order is restored
}

void TestThreeViewsKeepOrder()
{
    std::puts("three views: each submitted alone, in order, and restored");
    int                      a = 0, b = 0, c = 0;
    std::vector<const void*> views{&a, &b, &c};
    FakeFamily               fake(views, 0, 0);
    const auto               before = fake.memory;

    g_seen.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fake.memory.data(), &RecordingSubmit);

    EXPECT(g_seen.size() == 3);
    EXPECT(g_seen[0].firstView == &a && g_seen[1].firstView == &b && g_seen[2].firstView == &c);
    EXPECT(g_seen[2].skipFrame == 1 && g_seen[2].additional == 1);
    EXPECT(fake.memory == before);
    EXPECT(views[0] == &a && views[1] == &b && views[2] == &c);
}

void TestEngineFlagValuesAreRestored()
{
    std::puts("nonzero engine flags: the first submission keeps them, all are restored");
    int                      a = 0, b = 0;
    std::vector<const void*> views{&a, &b};
    FakeFamily               fake(views, 7, 0);
    const auto               before = fake.memory;

    g_seen.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fake.memory.data(), &RecordingSubmit);

    EXPECT(g_seen[0].skipFrame == 7);
    EXPECT(g_seen[1].skipFrame == 1);
    EXPECT(fake.memory == before);
}

void TestNoViewsIsPassedThrough()
{
    std::puts("no views: a single, unchanged submission");
    std::vector<const void*> views;
    FakeFamily               fake(views, 0, 0);
    const auto               before = fake.memory;

    g_seen.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fake.memory.data(), &RecordingSubmit);

    EXPECT(g_seen.size() == 1);
    EXPECT(fake.memory == before);
}

void TestBetweenFamiliesRunsBetweenSubmissions()
{
    std::puts("between families: called once between each two submissions, never around a single one");
    int                      a = 0, b = 0, c = 0;
    std::vector<const void*> one{&a};
    std::vector<const void*> two{&a, &b};
    std::vector<const void*> three{&a, &b, &c};
    FakeFamily               fakeOne(one, 0, 0);
    FakeFamily               fakeTwo(two, 0, 0);
    FakeFamily               fakeThree(three, 0, 0);

    g_order.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fakeOne.memory.data(), &RecordingSubmit, &Between);
    EXPECT(g_order == "S");
    g_order.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fakeTwo.memory.data(), &RecordingSubmit, &Between);
    EXPECT(g_order == "SBS");
    g_order.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fakeThree.memory.data(), &RecordingSubmit, &Between);
    EXPECT(g_order == "SBSBS");
    g_order.clear();
    SubmitOneFamilyPerView(nullptr, nullptr, fakeTwo.memory.data(), &RecordingSubmit);  // none given
    EXPECT(g_order == "SS");
}

}  // namespace

int main()
{
    TestOneViewIsPassedThroughUnchanged();
    TestTwoViewsBecomeTwoFamilies();
    TestThreeViewsKeepOrder();
    TestEngineFlagValuesAreRestored();
    TestNoViewsIsPassedThrough();
    TestBetweenFamiliesRunsBetweenSubmissions();

    return lss::test::Report();
}
