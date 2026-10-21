// Host-side tests for platform::SeqFloats, run without the game.
//
// Nothing to read before the first publish; a publish is read back whole; a reader that sees the writer mid-publish
// (an odd version) or a version that moved under it gives up after four attempts.

#include "platform/seqlock.h"

#include "expect.h"

#include <array>
#include <cstdio>

namespace {

using lss::platform::SeqFloats;

void TestPublishAndRead()
{
    std::puts("seqlock: nothing before the first publish; then the latest values, whole");
    SeqFloats<3>         floats;
    std::array<float, 3> out{9, 9, 9};
    EXPECT(!floats.Read(out));  // the verdict is what counts: a failed read may have copied the zeros
    floats.Publish({1, 2, 3});
    EXPECT(floats.Read(out));
    EXPECT(out[0] == 1 && out[1] == 2 && out[2] == 3);
    floats.Publish({4, 5, 6});
    EXPECT(floats.Read(out));
    EXPECT(out[0] == 4 && out[1] == 5 && out[2] == 6);
    EXPECT(floats.version.load() == 4);
}

void TestWriterInProgress()
{
    std::puts("seqlock: an odd version (the writer mid-publish) is never read");
    SeqFloats<2> floats;
    floats.Publish({1, 2});
    floats.version.store(3);  // as the writer leaves it between its two bumps
    std::array<float, 2> out{};
    EXPECT(!floats.Read(out));
    floats.version.store(4);
    EXPECT(floats.Read(out));
    EXPECT(out[0] == 1 && out[1] == 2);
}

}  // namespace

int main()
{
    TestPublishAndRead();
    TestWriterInProgress();

    return lss::test::Report();
}
