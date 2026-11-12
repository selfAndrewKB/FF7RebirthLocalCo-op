// Host-side tests for views/column_source.h: where each column's view comes from, per moment and per surface (the
// table in plan/topics/multi-monitor.md). Surfaces: one monitor with two or three players,
// the held columns (the three-player monitor mode, or `view-columns` on one monitor). Moments: field play, the game
// taking the whole screen, a shared ride, a seat's player waiting for a member (one, or every one), the console's
// override.

#include "views/column_source.h"

#include "expect.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using namespace lss::views::column_source;
using lss::players::SeatState;
using lss::views::columns::kFirstMirrorId;
using lss::views::columns::MapOf;

// Named: a brace literal splits the EXPECT macro.
constexpr SeatSource kSelf{Source::Self, Why::None};
constexpr SeatSource kColumnWhole{Source::PlayerOne, Why::WholeScreen};  // the column stays, on player 1's
constexpr SeatSource kColumnHanded{Source::PlayerOne, Why::Handed};
constexpr SeatSource kScreenWhole{Source::Screen, Why::WholeScreen};  // no column: the engine's single view
constexpr SeatSource kScreenHanded{Source::Screen, Why::Handed};

/// A moment: the listed controller ids (column order), each seat's word, the rest by name.
template <std::size_t N>
constexpr Moment At(const std::array<std::int32_t, N>& ids, SeatState seat1, SeatState seat2, bool held = false,
                    bool wholeScreen = false, bool sharedRide = false, Override choice = Override::Auto)
{
    Moment m;
    m.map         = MapOf(ids);
    m.seats[1]    = seat1;
    m.seats[2]    = seat2;
    m.columnsHeld = held;
    m.wholeScreen = wholeScreen;
    m.sharedRide  = sharedRide;
    m.choice      = choice;
    return m;
}

constexpr std::array<std::int32_t, 1> kOnePlayer{0};
constexpr std::array<std::int32_t, 2> kTwoPlayers{0, 1};
constexpr std::array<std::int32_t, 3> kThreePlayers{0, 1, 2};
constexpr std::array<std::int32_t, 3> kTwoAndMirror{0, 1, kFirstMirrorId};
constexpr std::array<std::int32_t, 3> kOneAndMirrors{0, kFirstMirrorId, kFirstMirrorId + 1};
constexpr std::array<std::int32_t, 3> kThreeOutOfOrder{0, 2, kFirstMirrorId};  // player 3 in, player 2 out

constexpr bool kHeld = true;

void TestFieldPlay()
{
    std::puts("column source: field play: every column its player's own");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out));
        EXPECT(!s.Single() && s.SplitShown() && s.ViewsSplit() && s.columnCount == 2);
        EXPECT(s.seats[1] == kSelf && s.columns[1] == kSelf && !s.OnPlayerOne(1));
        EXPECT(s.OwnColumn(1) == 1 && s.OwnColumn(2) == -1);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing));
        EXPECT(!s.Single() && s.SplitShown() && s.columnCount == 3);
        EXPECT(s.columns[1] == kSelf && s.columns[2] == kSelf);
        EXPECT(s.OwnColumn(1) == 1 && s.OwnColumn(2) == 2);  // each player's listener on their own column
    }
    {
        // Columns held, two players and a mirror: the player's own, the mirror player 1's.
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld));
        EXPECT(!s.Single() && s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.seats[1] == kSelf && s.columns[1] == kSelf && s.columns[2].why == Why::NoPlayer);
        EXPECT(s.OwnColumn(1) == 1 && s.OwnColumn(2) == -1);  // the mirror's column is no one's own
    }
    {
        // Player 1 alone: nothing to decide, one column.
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out));
        EXPECT(!s.Single() && !s.SplitShown() && !s.ViewsSplit() && s.columnCount == 1);
        EXPECT(s.columns[0] == kSelf && s.OwnColumn(1) == -1 && s.OwnColumn(2) == -1);
        EXPECT(s.OwnColumn(0) == -1 && s.OwnColumn(3) == -1);  // player 1 and no seat: none
    }
    {
        // A seat in but between members (joining, changing): their own view, as from the join.
        const Shown s = Decide(At(kTwoPlayers, SeatState::In, SeatState::Out));
        EXPECT(s.seats[1] == kSelf && s.SplitShown());
    }
    static_assert(Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out)).SplitShown(), "constexpr");
}

void TestWholeScreenOneMonitor()
{
    std::puts("column source: the game takes the whole screen, one monitor: the engine's single view (Screen)");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, true));
        EXPECT(s.Single() && !s.SplitShown() && !s.ViewsSplit());
        EXPECT(s.seats[1] == kScreenWhole && s.columns[1] == kScreenWhole);
        EXPECT(!s.OnPlayerOne(1));  // no column of theirs: the redirect and the HUD skip do not apply
        EXPECT(s.OwnColumn(1) == -1);  // one view, one listener: player 1's
    }
    {
        // A seat outside the game next to one in: the one in alone decides (every seat in waits).
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Awaiting));
        EXPECT(s.Single() && s.seats[1] == kScreenHanded && !s.seatIn[2]);
        EXPECT(s.seats[2] == kSelf);  // out: no source but the default, no reason
        const Shown r = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Rejoining));
        EXPECT(r.Single());
    }
    {
        // Reason priority: the console over everything, the whole screen over a seat's own wait.
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out, false, true, true,
                                  Override::Single));
        EXPECT(s.seats[1].source == Source::Screen && s.seats[1].why == Why::Console);
        const Shown w = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, false, true));
        EXPECT(w.Single() && w.seats[2] == kScreenWhole && w.seats[1] == kScreenWhole);
        const Shown r = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, false, false, true));
        EXPECT(r.Single() && r.seats[2].why == Why::SharedRide);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, false, true));
        EXPECT(s.Single());
        EXPECT(s.seats[2] == kScreenWhole);
    }
    {
        // With one player the game's own screen: never single by the mod.
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out, false, true));
        EXPECT(!s.Single());
    }
    {
        // An id the mod cannot read beside the player under the single view: its column is no one's, and the
        // player's own is not on screen.
        const std::array<std::int32_t, 3> ids{0, -1, 1};
        const Shown s = Decide(At(ids, SeatState::Playing, SeatState::Out, false, true));
        EXPECT(s.Single() && s.columns[1].why == Why::NoPlayer && s.OwnColumn(1) == -1);
    }
    static_assert(Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, true)).Single(), "the screen");
}

void TestWholeScreenColumnsHeld()
{
    std::puts("column source: the game takes the whole screen, columns held: player 1's in every column (PlayerOne)");
    {
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, true));
        EXPECT(!s.Single() && !s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.seats[1] == kColumnWhole);
        EXPECT(s.OnPlayerOne(1));
        EXPECT(s.columns[1] == s.seats[1]);
        EXPECT(s.columns[2].source == Source::PlayerOne && s.columns[2].why == Why::NoPlayer);  // the mirror
        EXPECT(s.OwnColumn(1) == -1);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, kHeld, true));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.OnPlayerOne(2));
    }
    {
        // The failed-switch fallback (mod.cpp): the same moment decided again with the columns held turns every
        // Screen into PlayerOne and nothing else.
        Moment m = At(kThreePlayers, SeatState::Playing, SeatState::Handed, false, true);
        const Shown one = Decide(m);
        m.columnsHeld = true;
        const Shown held = Decide(m);
        EXPECT(one.Single() && !held.Single() && held.OnPlayerOne(1) && held.OnPlayerOne(2));
        EXPECT(held.seats[1] == kColumnWhole && held.seats[2] == kColumnWhole);
        EXPECT(held.columnCount == one.columnCount);
    }
    static_assert(Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, true)).OnPlayerOne(1),
                  "constexpr");
    static_assert(!Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, true)).Single(),
                  "columns held");
}

void TestSharedRide()
{
    std::puts("column source: a shared ride: player 1's everywhere (the screen on one monitor)");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out, false, false, true));
        EXPECT(s.Single() && s.seats[1].source == Source::Screen && s.seats[1].why == Why::SharedRide);
    }
    {
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed, kHeld, false, true));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.OnPlayerOne(2));
        EXPECT(s.seats[2].why == Why::SharedRide);
    }
}

void TestWaiting()
{
    std::puts("column source: player 2 or 3 waiting for a member: their column shows player 1's; everyone "
              "waiting: the screen on one monitor");
    {
        // Two players, player 2 waits: nobody but player 1 plays: the engine's single view.
        const Shown s = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out));
        EXPECT(s.Single() && s.seats[1] == kScreenHanded);
    }
    {
        // Three players, player 3 waits while player 2 plays, one monitor: the split stays, player 3's column shows
        // player 1's.
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed));
        EXPECT(!s.Single() && s.SplitShown() && s.ViewsSplit());
        EXPECT(s.seats[1] == kSelf && !s.OnPlayerOne(1));
        EXPECT(s.seats[2] == kColumnHanded && s.OnPlayerOne(2));
        EXPECT(s.columns[2] == s.seats[2]);
        EXPECT(s.OwnColumn(1) == 1 && s.OwnColumn(2) == -1);
    }
    {
        // The other way round: player 2 waits, player 3 plays; player 3's listener on their column.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Playing));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && !s.OnPlayerOne(2));
        EXPECT(s.OwnColumn(1) == -1 && s.OwnColumn(2) == 2);
    }
    {
        // Columns held, one waits while the other plays: the waiting one's column on player 1's too.
        const Shown s = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Handed, kHeld));
        EXPECT(!s.Single() && !s.OnPlayerOne(1) && s.OnPlayerOne(2) && s.SplitShown() && s.OwnColumn(1) == 1);
        EXPECT(s.OwnColumn(2) == -1);
    }
    {
        // Player 3 listed before player 2 (player 2 rejoined) and waiting: player 2's listener on their column 2.
        const std::array<std::int32_t, 3> ids{0, 2, 1};
        const Shown s = Decide(At(ids, SeatState::Playing, SeatState::Handed));
        EXPECT(s.columns[1] == kColumnHanded && s.columns[2] == kSelf && s.OwnColumn(1) == 2 && s.OwnColumn(2) == -1);
    }
    {
        // Both wait, one monitor: the screen.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed));
        EXPECT(s.Single() && s.seats[1] == kScreenHanded && s.seats[2] == kScreenHanded);
    }
    {
        // Both wait, columns held: the columns stay on player 1's.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed, kHeld));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.OnPlayerOne(2) && !s.SplitShown() && s.ViewsSplit());
    }
    {
        // Player 2 waits, columns held with a mirror: player 2's column on player 1's, the mirror's too.
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Handed, SeatState::Out, kHeld));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.columns[2].why == Why::NoPlayer && !s.SplitShown());
    }
    {
        // A seat in and between members is not waiting on the game: their own view.
        const Shown s = Decide(At(kThreePlayers, SeatState::In, SeatState::Handed));
        EXPECT(!s.Single() && s.seats[1] == kSelf && s.OnPlayerOne(2));
    }
}

void TestMirrors()
{
    std::puts("column source: a column no seat fills shows player 1's; a seat's column is read by its seat");
    {
        // The monitor mode with player 1 alone: two mirrors, nothing single, no split of a seat's view.
        const Shown s = Decide(At(kOneAndMirrors, SeatState::Out, SeatState::Out, kHeld));
        EXPECT(!s.Single() && !s.SplitShown() && s.ViewsSplit() && s.columnCount == 3);
        EXPECT(s.columns[0] == kSelf);
        EXPECT(s.columns[1].why == Why::NoPlayer && s.columns[2].why == Why::NoPlayer);
        EXPECT(s.OwnColumn(1) == -1 && s.OwnColumn(2) == -1);
    }
    {
        // Player 3 in with player 2 out (column 1 is seat 2's), a mirror in column 2.
        const Shown s = Decide(At(kThreeOutOfOrder, SeatState::Out, SeatState::Playing, kHeld));
        EXPECT(s.columns[1] == kSelf && s.columns[2].why == Why::NoPlayer && s.OwnColumn(2) == 1);
        EXPECT(s.OwnColumn(1) == -1);
        EXPECT(s.seats[1] == kSelf && !s.seatIn[1]);  // out: no reason, not in
    }
    {
        // A list the mod cannot read (an unread id): no one's column.
        const std::array<std::int32_t, 2> ids{0, -1};
        const Shown s = Decide(At(ids, SeatState::Out, SeatState::Out));
        EXPECT(s.columns[1].why == Why::NoPlayer && s.columnCount == 2);
    }
    {
        // An empty list (nothing readable yet): one column, player 1's own.
        const Shown s = Decide(At(std::array<std::int32_t, 0>{}, SeatState::Out, SeatState::Out));
        EXPECT(s.columnCount == 1 && !s.ViewsSplit() && s.columns[0] == kSelf);
    }
}

void TestOverrides()
{
    std::puts("column source: single-view on/off override the automatic choice while player 2 or 3 is in");
    {
        const Shown s = Decide(At(kTwoPlayers, SeatState::Playing, SeatState::Out, false, false, false,
                                  Override::Single));
        EXPECT(s.Single() && s.seats[1].source == Source::Screen && s.seats[1].why == Why::Console);
        const Shown t = Decide(At(kThreePlayers, SeatState::Playing, SeatState::Playing, false, false, false,
                                  Override::Single));
        EXPECT(t.Single() && t.seats[2].source == Source::Screen && t.seats[2].why == Why::Console);
    }
    {
        const Shown s = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, false, false,
                                  Override::Single));
        EXPECT(!s.Single() && s.OnPlayerOne(1) && s.seats[1].why == Why::Console);
    }
    {
        // Off: never player 1's for a seat, whatever the moment; a mirror still shows it.
        const Shown s = Decide(At(kThreePlayers, SeatState::Handed, SeatState::Handed, false, true, true,
                                  Override::Split));
        EXPECT(!s.Single() && s.seats[1] == kSelf && s.seats[2] == kSelf && s.SplitShown());
        const Shown t = Decide(At(kTwoPlayers, SeatState::Handed, SeatState::Out, false, true, false,
                                  Override::Split));
        EXPECT(!t.Single() && t.seats[1] == kSelf && t.SplitShown());
        const Shown m = Decide(At(kTwoAndMirror, SeatState::Playing, SeatState::Out, kHeld, true, false,
                                  Override::Split));
        EXPECT(m.columns[2].why == Why::NoPlayer && m.columns[1] == kSelf);
    }
    {
        // On with every seat out: nothing (the game's own screen).
        const Shown s = Decide(At(kOnePlayer, SeatState::Out, SeatState::Out, false, false, false,
                                  Override::Single));
        EXPECT(!s.Single());
    }
}

bool Says(const std::wstring& text, const wchar_t* part)
{
    return text.find(part) != std::wstring::npos;
}

void TestDescribe()
{
    std::puts("column source: each change has text, the same state none");
    EXPECT(Describe(1, kSelf, kSelf).empty());
    EXPECT(Describe(1, kScreenWhole, kScreenWhole).empty());
    EXPECT(Says(Describe(1, kSelf, kColumnWhole), L"player 2's view shows player 1's camera and HUD"));
    EXPECT(Says(Describe(1, kSelf, kScreenWhole), L"player 2's view gives way to player 1's"));
    EXPECT(Says(Describe(2, kSelf, kColumnHanded),
                L"player 3's view shows player 1's picture while the game has their party member"));
    EXPECT(Says(Describe(1, kSelf, kScreenHanded), L"player 2 waits for a party member"));
    EXPECT(Says(Describe(2, kColumnHanded, kSelf), L"player 3's view is their own again"));
    EXPECT(Says(Describe(2, kScreenHanded, kSelf), L"player 3's view is their own again"));
    // A reason change.
    EXPECT(Says(Describe(1, kColumnHanded, kColumnWhole), L"the game takes the whole screen"));
    // A mechanism change: columns held or released while the game takes the whole screen (Ctrl+F3 on one monitor),
    // or a third player's lone wait becoming everyone's (and back): a line each way.
    EXPECT(Says(Describe(1, kScreenWhole, kColumnWhole), L"player 2's view shows player 1's camera and HUD"));
    EXPECT(Says(Describe(1, kColumnWhole, kScreenWhole), L"player 2's view gives way to player 1's"));
    EXPECT(Says(Describe(2, kColumnHanded, kScreenHanded), L"player 3 waits for a party member"));
    EXPECT(Says(Describe(2, kScreenHanded, kColumnHanded), L"their column stays"));
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