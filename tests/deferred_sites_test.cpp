// Host-side tests for platform/deferred_sites, run without the game.
//
// The protocol of a module whose call sites are placed from the game thread on 1.005: at install the placement
// runs at once (its result is the install's) or is queued; the queue's run places it once, with the image kept
// for it; a Remove before the run cancels it; a full queue places nothing and says so; the default is the
// running build's.

#include "platform/deferred_sites.h"

#include "platform/build_value.h"
#include "platform/game_thread_sites.h"
#include "platform/log.h"

#include "expect.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace lss::platform {
void WriteLine(std::wstring_view line) noexcept  // the mod's log, to the console here
{
    std::printf("  log: %ls\n", std::wstring(line).c_str());
}
namespace detail {
std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::Trace)};  // every level, to the console
}
const wchar_t* LevelName(Level) noexcept
{
    return L"level";
}
}  // namespace lss::platform

namespace {

using lss::platform::BuildIndex;
using lss::platform::DeferredSites;
using lss::platform::GameImage;
using lss::platform::SetRunningBuild;
using lss::platform::game_thread_sites::Queue;
using lss::platform::game_thread_sites::Run;

int            g_placed      = 0;
std::uintptr_t g_placedBase  = 0;
bool           g_placeResult = true;

bool Place(const GameImage& image) noexcept
{
    ++g_placed;
    g_placedBase = image.Base();
    return g_placeResult;
}

void Nothing() noexcept
{
}

void TestAtInstall()
{
    std::puts("deferred sites: not deferred, the placement runs at install with the image, and its result is the install's");
    DeferredSites sites;
    g_placed      = 0;
    g_placeResult = true;
    EXPECT(sites.Install(GameImage::At(0x1000, 0x100), &Place, false));
    EXPECT(g_placed == 1 && g_placedBase == 0x1000);
    EXPECT(!sites.FromGameThread());
    EXPECT(!sites.Pending());
    g_placeResult = false;
    EXPECT(!sites.Install(GameImage::At(0x1000, 0x100), &Place, false));
    EXPECT(g_placed == 2);
    Run();
    EXPECT(g_placed == 2);  // nothing was queued
}

void TestDeferred()
{
    std::puts("deferred sites: deferred, the placement waits for the game thread's tick and runs there once, with the image kept");
    DeferredSites sites;
    g_placed      = 0;
    g_placeResult = true;
    EXPECT(sites.Install(GameImage::At(0x2000, 0x100), &Place, true));
    EXPECT(g_placed == 0);
    EXPECT(sites.FromGameThread());
    EXPECT(sites.Pending());
    Run();
    EXPECT(g_placed == 1 && g_placedBase == 0x2000);
    EXPECT(!sites.Pending());
    Run();
    EXPECT(g_placed == 1);
}

void TestCancel()
{
    std::puts("deferred sites: a Remove before the tick cancels the placement; the tick then does nothing");
    DeferredSites sites;
    g_placed = 0;
    EXPECT(sites.Install(GameImage::At(0x3000, 0x100), &Place, true));
    sites.Cancel();
    EXPECT(!sites.Pending());
    Run();
    EXPECT(g_placed == 0);
}

void TestNoRoom()
{
    std::puts("deferred sites: a full queue places nothing: the install says false and nothing is pending");
    for (int i = 0; i < 16; ++i)
    {
        EXPECT(Queue(&Nothing));
    }
    DeferredSites sites;
    g_placed = 0;
    EXPECT(!sites.Install(GameImage::At(0x4000, 0x100), &Place, true));
    EXPECT(!sites.Pending());
    EXPECT(sites.FromGameThread());
    Run();
    EXPECT(g_placed == 0);
}

void TestDefaultIsTheBuild()
{
    std::puts("deferred sites: by default the placement is deferred on 1.005 and not on the research build");
    SetRunningBuild(BuildIndex::V1005);
    EXPECT(DeferredSites::Deferred());
    DeferredSites sites;
    g_placed = 0;
    EXPECT(sites.Install(GameImage::At(0x5000, 0x100), &Place));
    EXPECT(g_placed == 0 && sites.Pending());
    Run();
    EXPECT(g_placed == 1);

    SetRunningBuild(BuildIndex::Research);
    EXPECT(!DeferredSites::Deferred());
    EXPECT(sites.Install(GameImage::At(0x5000, 0x100), &Place));
    EXPECT(g_placed == 2 && !sites.Pending());
}

}  // namespace

int main()
{
    TestAtInstall();
    TestDeferred();
    TestCancel();
    TestNoRoom();
    TestDefaultIsTheBuild();

    return lss::test::Report();
}
