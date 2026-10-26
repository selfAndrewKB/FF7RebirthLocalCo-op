// Host-side tests for views/column_picture.h: what each column shows, per moment and per surface (the table in
// plan/topics/multi-monitor.md, "What each column shows"). Surfaces: one monitor with two or three players, the
// held columns (the three-player monitor mode, or `view-columns` on one monitor). Moments: field play, the game
// taking the whole screen, a shared ride, a further player waiting for a member (one, or every one), the console's
// override.

#include "views/column_picture.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using namespace lss::views::column_picture;
using lss::players::SeatState;
using lss::views::columns::kFirstMirrorId;
using lss::views::columns::MapOf;

constexpr SeatPicture kOwn{Picture::Own, Why::None};
constexpr SeatPicture kWhole{Picture::PlayerOne, Why::WholeScreen};  // (named: a brace literal splits the EXPECT macro)
constexpr SeatPicture kHanded{Picture::PlayerOne, Why::Handed};

/// A moment: the listed controller ids (column order), each seat's word, the rest by name.
template <std::size_t N>
constexpr Moment At(const std::array<std::int32_t, N>& ids, SeatState seat1, SeatState seat2, bool held = false,
                    bool wholeScreen = false, bool sharedRide = false, Override override = Override::Auto)
{
    Moment m;
    m.map         = MapOf(ids);
    m.seats[1]    = seat1;
    m.seats[2]    = seat2;
    m.columnsHeld = held;
    m.wholeScreen = wholeScreen;
    m.sharedRide  = sharedRide;
    m.choice      = override;
    return m;
}

constexpr std::array<std::int32_t, 1> kOnePlayer{0};
constexpr std::array<std::int32_t, 2> kTwoPlayers{0, 1};
constexpr std::array<std::int32_t, 3> kThreePlayers{0, 1, 2};
constexpr std::array<std::int32_t, 3> kTwoAndMirror{0, 1, kFirstMirrorId};
constexpr std::array<std::int32_t, 3> kOneAndMirrors{0, kFirstMirrorId, kFirstMirrorId + 1};
constexpr std::array<std::int32_t, 3> kThreeOutOfOrder{0, 2, kFirstMirrorId};  // player 3 in, player 2 out

void TestFieldPlay()
{
    std::puts("column picture: field play: every column its player's own");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out));
        EXPECT(!s.single && s.SplitShown() && s.ViewsSplit() && s.columnCount == 2);
        EXPECT(s.seats[1] == kOwn && s.columns[1] == kOwn && !s.OnPlayerOne(1));
        EXPECT(s.ListenerColumn() == 1);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing));
        EXPECT(!s.single && s.SplitShown() && s.columnCount == 3);
        EXPECT(s.columns[1] == kOwn && s.columns[2] == kOwn);
    }
    {
        // Player 1 alone: nothing to decide, one column.
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out));
        EXPECT(!s.single && !s.SplitShown() && !s.ViewsSplit() && s.columnCount == 1);
        EXPECT(s.ListenerColumn() == -1);
    }
    {
        // A seat in but between members (joining, changing): their own view, as from the join.
        const Shown s = Decide(At(kTwoPlayers, SeatState::In, SeatState::Out));
        EXPECT(s.seats[1] == kOwn && s.SplitShown());
    }
    static_assert(Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out)).SplitShown(), "constexpr");
}

void TestWholeScreenOneMonitor()
{
    std::puts("column picture: the game takes the whole screen, one monitor: the engine's single view");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, true));
        EXPECT(s.single && !s.SplitShown() && !s.ViewsSplit());
        EXPECT(s.seats[1] == kWhole);
        EXPECT(!s.OnPlayerOne(1));  // under the single view the redirect does not apply (there is one view)
        EXPECT(s.ListenerColumn() == 1);  // the listener's slot stays on player 2's column (as before step 6a)
    }
    {
        // A seat outside the game next to one in: the one in alone decides (every further player in waits).
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Awaiting));
        EXPECT(s.single && s.seats[1].why == Why::Handed && !s.seatIn[2]);
        const Shown r = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Rejoining));
        EXPECT(r.single);
    }
    {
        // Reason priority: the console over everything, the whole screen over a seat's own wait.
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out, false, true, true, Override::Single));
        EXPECT(s.seats[1].why == Why::Console);
        const Shown w = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, false, true));
        EXPECT(w.single && w.seats[2].why == Why::WholeScreen && w.seats[1].why == Why::WholeScreen);
        const Shown r = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, false, false, true));
        EXPECT(r.single && r.seats[2].why == Why::SharedRide);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, false, true));
        EXPECT(s.single);
        EXPECT(s.seats[2].why == Why::WholeScreen);
    }
    {
        // With one player the game's own screen: never single by the mod.
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out, false, true));
        EXPECT(!s.single);
    }
}

void TestWholeScreenColumnsHeld()
{
    std::puts("column picture: the game takes the whole screen, columns held: player 1's picture in every column");
    {
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, true, true));
        EXPECT(!s.single && !s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.seats[1] == kWhole);
        EXPECT(s.OnPlayerOne(1));
        EXPECT(s.columns[1] == s.seats[1]);
        EXPECT(s.columns[2].picture == Picture::PlayerOne && s.columns[2].why == Why::NoPlayer);  // the mirror
        EXPECT(s.ListenerColumn() == -1);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, true, true));
        EXPECT(!s.single && s.OnPlayerOne(1) && s.OnPlayerOne(2));
    }
    static_assert(Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, true, true)).OnPlayerOne(1), "constexpr");
    static_assert(!Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, true, true)).single, "columns held");
}

void TestSharedRide()
{
    std::puts("column picture: a shared ride: player 1's picture everywhere (single on one monitor)");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out, false, false, true));
        EXPECT(s.single && s.seats[1].why == Why::SharedRide);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed, true, false, true));
        EXPECT(!s.single && s.OnPlayerOne(1) && s.OnPlayerOne(2));
        EXPECT(s.seats[2].why == Why::SharedRide);
    }
}

void TestWaiting()
{
    std::puts("column picture: a further player waiting for a member: their column shows player 1's; everyone "
              "waiting: single on one monitor");
    {
        // Two players, player 2 waits: nobody but player 1 plays: the engine's single view (as before step 6a).
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out));
        EXPECT(s.single && s.seats[1] == kHanded);
    }
    {
        // Three players, player 3 waits while player 2 plays, one monitor: the split stays, player 3's column shows
        // player 1's picture (the gap closed by step 6a: before it, player 3 kept their own camera).
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed));
        EXPECT(!s.single && s.SplitShown() && s.ViewsSplit());
        EXPECT(s.seats[1] == kOwn && !s.OnPlayerOne(1));
        EXPECT(s.seats[2] == kHanded && s.OnPlayerOne(2));
        EXPECT(s.columns[2] == s.seats[2]);
        EXPECT(s.ListenerColumn() == 1);
    }
    {
        // The other way round: player 2 waits, player 3 plays; the listener follows player 3's column.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Playing));
        EXPECT(!s.single && s.OnPlayerOne(1) && !s.OnPlayerOne(2));
        EXPECT(s.ListenerColumn() == 2);
    }
    {
        // Columns held, one waits while the other plays: the waiting one's column on player 1's picture too (before
        // step 6a every seat needed a reason for any to show it).
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, true));
        EXPECT(!s.single && !s.OnPlayerOne(1) && s.OnPlayerOne(2) && s.SplitShown() && s.ListenerColumn() == 1);
    }
    {
        // Player 3 listed before player 2 (player 2 rejoined) and waiting: the listener skips to player 2's column.
        const std::array<std::int32_t, 3> ids{0, 2, 1};
        const Shown s = Decide(At(ids, SeatState::Playing, SeatState::Handed));
        EXPECT(s.columns[1] == kHanded && s.columns[2] == kOwn && s.ListenerColumn() == 2);
    }
    {
        // Both wait, one monitor: single.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed));
        EXPECT(s.single && s.seats[1].why == Why::Handed && s.seats[2].why == Why::Handed);
    }
    {
        // Both wait, columns held: the columns stay on player 1's picture.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed, true));
        EXPECT(!s.single && s.OnPlayerOne(1) && s.OnPlayerOne(2) && !s.SplitShown() && s.ViewsSplit());
    }
    {
        // Player 2 waits, columns held with a mirror: player 2's column on player 1's, the mirror's too.
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Handed, SeatState::Out, true));
        EXPECT(!s.single && s.OnPlayerOne(1) && s.columns[2].why == Why::NoPlayer && !s.SplitShown());
    }
    {
        // A seat in and between members is not waiting on the game: their own view.
        const Shown s = Decide(At(kThreePlayers, SeatState::In, SeatState::Handed));
        EXPECT(!s.single && s.seats[1] == kOwn && s.OnPlayerOne(2));
    }
}

void TestMirrors()
{
    std::puts("column picture: a column no seat fills shows player 1's picture; a seat's column is read by its seat");
    {
        // The monitor mode with player 1 alone: two mirrors, nothing single, no split of a further player.
        const Shown s = Decide(At(kOneAndMirrors, SeatState::Out, SeatState::Out, true));
        EXPECT(!s.single && !s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.columns[0] == kOwn);
        EXPECT(s.columns[1].why == Why::NoPlayer && s.columns[2].why == Why::NoPlayer);
        EXPECT(s.ListenerColumn() == -1);
    }
    {
        // Player 3 in with player 2 out (column 1 is seat 2's), a mirror in column 2.
        const Shown s = Decide(At(kThreeOutOfOrder, SeatState::Out, SeatState::Playing, true));
        EXPECT(s.columns[1] == kOwn && s.columns[2].why == Why::NoPlayer && s.ListenerColumn() == 1);
        EXPECT(s.seats[1] == kOwn && !s.seatIn[1]);  // out: no reason, not in
    }
    {
        // A list the mod cannot read (an unread id): no one's column.
        const std::array<std::int32_t, 2> ids{0, -1};
        const Shown s = Decide(At(ids, SeatState::Out, SeatState::Out));
        EXPECT(s.columns[1].why == Why::NoPlayer && s.columnCount == 2);
    }
    {
        // An empty list (nothing readable yet): one column.
        const Shown s = Decide(At(std::array<std::int32_t, 0>{}, SeatState::Out, SeatState::Out));
        EXPECT(s.columnCount == 1 && !s.ViewsSplit());
    }
}

void TestOverrides()
{
    std::puts("column picture: single-view on/off override the automatic choice while a further player is in");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, false, false, Override::Single));
        EXPECT(s.single && s.seats[1].why == Why::Console);
    }
    {
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, true, false, false, Override::Single));
        EXPECT(!s.single && s.OnPlayerOne(1) && s.seats[1].why == Why::Console);
    }
    {
        // Off: never player 1's picture for a seat, whatever the moment; a mirror still shows it.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed, false, true, true, Override::Split));
        EXPECT(!s.single && s.seats[1] == kOwn && s.seats[2] == kOwn && s.SplitShown());
        const Shown m = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, true, true, false, Override::Split));
        EXPECT(m.columns[2].why == Why::NoPlayer && m.columns[1] == kOwn);
    }
    {
        // On with no further player: nothing (the game's own screen).
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out, false, false, false, Override::Single));
        EXPECT(!s.single);
    }
}

void TestDescribe()
{
    std::puts("column picture: each change has text, the same state none");
    EXPECT(Describe(1, kOwn, kOwn, false).empty());
    EXPECT(Describe(1, kWhole, kWhole, true).empty());
    EXPECT(Describe(1, kOwn, kWhole, false).find(L"player 2's view shows player 1's camera and HUD") != std::wstring::npos);
    EXPECT(Describe(1, kOwn, kWhole, true).find(L"player 2's view gives way to player 1's") != std::wstring::npos);
    EXPECT(Describe(2, kOwn, kHanded, false).find(L"player 3's view shows player 1's picture while the game has their party member") !=
           std::wstring::npos);
    EXPECT(Describe(1, kOwn, kHanded, true).find(L"player 2 waits for a party member") != std::wstring::npos);
    EXPECT(Describe(2, kHanded, kOwn, false).find(L"player 3's view is their own again") != std::wstring::npos);
    EXPECT(Describe(1, kHanded, kWhole, false).find(L"the game takes the whole screen") != std::wstring::npos);  // a reason change
    for (const auto why : {Why::None, Why::WholeScreen, Why::SharedRide, Why::Handed, Why::Console, Why::NoPlayer})
    {
        EXPECT(NameOf(why)[0] != L'?');
    }
}

}  // namespace

int main()
{
    TestFieldPlay();
    TestWholeScreenOneMonitor();
    TestWholeScreenColumnsHeld();
    TestSharedRide();
    TestWaiting();
    TestMirrors();
    TestOverrides();
    TestDescribe();
    return lss::test::Report();
}
