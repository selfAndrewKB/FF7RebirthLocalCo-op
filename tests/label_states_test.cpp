// Host-side tests for hud::label_states::LabelBook, run without the game.
//
// A fake widget's "state" is a number standing for its text and layout (0: the empty state the constructor writes).
// The cases: the bootstrap from the empty state; later frames swap player 2's own state in and player 1's back; a
// second SetText in one run swaps nothing; a font reload resets player 2's state; a widget dying inside player 2's pass
// or aside; releasing everything; refusals, a full table and faults; the weak references balance.

#include "hud/label_states.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using lss::hud::label_states::EmptyState;
using lss::hud::label_states::LabelAccess;
using lss::hud::label_states::LabelBook;
using lss::hud::label_states::Parked;

int g_failures = 0;

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

struct FakeWidget
{
    int           state       = 0;
    bool          alive       = true;
    bool          usable      = true;
    bool          swapFaults  = false;
    std::uint64_t stamp       = 7;
    int           weak        = 1;  // its own WeakThis
    int           invalidates = 0;
    // The controller is the widget's address here; the book only compares and passes it.
};

std::vector<int> g_freed;  // states freed from a set-aside copy (0 = an empty one)

int StateOf(const Parked& parked)
{
    int state = 0;
    std::memcpy(&state, parked.text, sizeof(state));
    return state;
}

void SetState(Parked& parked, int state)
{
    std::memcpy(parked.text, &state, sizeof(state));
}

FakeWidget* AsWidget(void* p)
{
    return static_cast<FakeWidget*>(p);
}

bool Usable(void* w) noexcept { return AsWidget(w)->usable && AsWidget(w)->alive; }
bool Alive(void* c) noexcept { return AsWidget(c)->alive; }
bool Swap(void* w, Parked& parked) noexcept
{
    FakeWidget* widget = AsWidget(w);
    if (widget->swapFaults)
    {
        return false;
    }
    const int aside = StateOf(parked);
    SetState(parked, widget->state);
    widget->state = aside;
    return true;
}
void          Invalidate(void* w) noexcept { ++AsWidget(w)->invalidates; }
std::uint64_t Stamp(void* w) noexcept { return AsWidget(w)->stamp; }
bool          HoldWeak(void* c) noexcept
{
    ++AsWidget(c)->weak;
    return true;
}
void ReleaseWeak(void* c) noexcept { --AsWidget(c)->weak; }
void FreeParked(Parked& parked) noexcept
{
    g_freed.push_back(StateOf(parked));
    parked = EmptyState();
}

LabelBook Fresh()
{
    g_freed.clear();
    return LabelBook(LabelAccess{&Usable, &Alive, &Swap, &Invalidate, &Stamp, &HoldWeak, &ReleaseWeak, &FreeParked});
}

// One frame of the game: player 2's run sets its text (the relayout writes `p2` if the text differs), player 2's paint,
// the swap-back. Player 1's state is in the widget before and after.
void PlayerTwoRun(LabelBook& book, FakeWidget& w, int p2)
{
    EXPECT(book.BeforeSetText(&w, &w));
    if (w.state != p2)
    {
        w.state = p2;  // the game's relayout, into whichever state is in the widget
    }
}

void TestEmptyStateLayout()
{
    std::puts("labels: the empty state is all zero but the shadow offset 1.0, 1.0");
    const Parked empty = EmptyState();
    float        shadow[2] = {};
    std::memcpy(shadow, empty.quads + lss::hud::label_states::kShadowInQuads, sizeof(shadow));
    EXPECT(shadow[0] == 1.0f && shadow[1] == 1.0f);
    EXPECT(StateOf(empty) == 0);
    for (std::size_t i = 0; i < lss::hud::label_states::kShadowInQuads; ++i)
    {
        EXPECT(empty.quads[i] == std::byte{0});
    }
}

void TestBootstrapThenOwnStates()
{
    std::puts("labels: the first run lays player 2's state out from empty; later frames only swap");
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;  // player 1's
    EXPECT(book.BeforeSetText(&w, &w));
    EXPECT(w.state == 0);  // the empty state: the game's compare fails and its relayout builds player 2's
    EXPECT(book.AnyInside());
    w.state = 2;  // the relayout
    book.SwapBack();
    EXPECT(w.state == 1 && !book.AnyInside());
    EXPECT(w.weak == 2 && book.Count() == 1);

    PlayerTwoRun(book, w, 2);
    EXPECT(w.state == 2);  // player 2's own state, already laid out: no relayout needed
    book.SwapBack();
    EXPECT(w.state == 1);
    EXPECT(book.Counts().created == 1 && book.Counts().swapsIn == 2 && book.Counts().swapsOut == 2);
    EXPECT(w.invalidates == 4);
    EXPECT(g_freed.empty());
}

void TestSecondSetTextInOneRun()
{
    std::puts("labels: a second SetText on the same block in one run swaps nothing");
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    EXPECT(book.BeforeSetText(&w, &w));
    w.state = 2;
    EXPECT(book.BeforeSetText(&w, &w));
    EXPECT(w.state == 2 && book.Counts().swapsIn == 1);
    book.SwapBack();
    EXPECT(w.state == 1);
}

void TestFontReloadResets()
{
    std::puts("labels: a changed font stamp frees player 2's old state and starts from empty");
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    PlayerTwoRun(book, w, 2);
    book.SwapBack();
    w.stamp = 8;  // a font reload relaid player 1's state out
    EXPECT(book.BeforeSetText(&w, &w));
    EXPECT(w.state == 0);
    EXPECT(g_freed.size() == 1 && g_freed[0] == 2);
    EXPECT(book.Counts().fontResets == 1);
    w.state = 3;
    book.SwapBack();
    EXPECT(w.state == 1);
    PlayerTwoRun(book, w, 3);
    EXPECT(w.state == 3 && book.Counts().fontResets == 1);  // the new stamp is kept
    book.SwapBack();
}

void TestDiesInsidePlayerTwosPass()
{
    std::puts("labels: a widget dying with player 2's state in: player 1's, aside, is freed");
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    PlayerTwoRun(book, w, 2);
    w.alive = false;  // its destructor freed player 2's state, in the widget
    book.SwapBack();
    EXPECT(w.state == 2);  // untouched
    EXPECT(g_freed.size() == 1 && g_freed[0] == 1);
    EXPECT(w.weak == 1 && book.Count() == 0 && book.Counts().diedInside == 1);
}

void TestDiesAside()
{
    std::puts("labels: a widget dying outside the pass: player 2's state, aside, is freed at the sweep");
    auto       book = Fresh();
    FakeWidget w;
    w.state = 1;
    PlayerTwoRun(book, w, 2);
    book.SwapBack();
    book.Sweep();
    EXPECT(book.Count() == 1 && g_freed.empty());
    w.alive = false;
    book.Sweep();
    EXPECT(g_freed.size() == 1 && g_freed[0] == 2);
    EXPECT(w.weak == 1 && book.Count() == 0 && book.Counts().diedAside == 1);
}

void TestMiddleRecordDropped()
{
    std::puts("labels: dropping a middle record keeps the others' states and references right");
    auto       book = Fresh();
    FakeWidget a, b, c;
    a.state = 1;
    b.state = 11;
    c.state = 21;
    PlayerTwoRun(book, a, 2);
    PlayerTwoRun(book, b, 12);
    PlayerTwoRun(book, c, 22);
    b.alive = false;  // dies inside player 2's pass
    book.SwapBack();
    EXPECT(a.state == 1 && c.state == 21 && b.state == 12);
    EXPECT(g_freed.size() == 1 && g_freed[0] == 11);
    EXPECT(book.Count() == 2 && b.weak == 1 && a.weak == 2 && c.weak == 2);

    PlayerTwoRun(book, a, 2);  // both survivors still find their own records
    PlayerTwoRun(book, c, 22);
    EXPECT(a.state == 2 && c.state == 22 && book.Counts().created == 3);
    book.SwapBack();
    a.alive = false;  // dies aside
    book.Sweep();
    EXPECT(c.state == 21 && book.Count() == 1 && a.weak == 1 && c.weak == 2);
    EXPECT(g_freed.size() == 2 && g_freed[1] == 2);
    PlayerTwoRun(book, c, 22);
    EXPECT(c.state == 22);
    book.ReleaseAll();
    EXPECT(c.state == 21 && c.weak == 1 && g_freed.size() == 3 && g_freed[2] == 22);
}

void TestReleaseAll()
{
    std::puts("labels: releasing everything leaves player 1's state in and frees player 2's");
    auto       book = Fresh();
    FakeWidget a, b;
    a.state = 1;
    b.state = 11;
    PlayerTwoRun(book, a, 2);
    PlayerTwoRun(book, b, 12);
    book.ReleaseAll();  // even from inside the pass
    EXPECT(a.state == 1 && b.state == 11);
    EXPECT(g_freed.size() == 2);
    EXPECT(a.weak == 1 && b.weak == 1 && book.Count() == 0 && book.Counts().released == 2);
}

void TestRefusalsAndFull()
{
    std::puts("labels: an unusable widget and a full table are left shared, untouched");
    auto       book = Fresh();
    FakeWidget odd;
    odd.state  = 5;
    odd.usable = false;
    EXPECT(book.BeforeSetText(&odd, &odd));
    EXPECT(odd.state == 5 && odd.weak == 1 && book.Count() == 0 && book.Counts().refused == 1);

    std::vector<FakeWidget> many(LabelBook::kMaxLabels + 1);
    for (FakeWidget& w : many)
    {
        w.state = 1;
        EXPECT(book.BeforeSetText(&w, &w));
    }
    EXPECT(book.Count() == LabelBook::kMaxLabels && book.Counts().full == 1);
    EXPECT(many.back().state == 1 && many.back().weak == 1);
    book.ReleaseAll();
    for (const FakeWidget& w : many)
    {
        EXPECT(w.state == 1 && w.weak == 1);
    }
}

void TestFaults()
{
    std::puts("labels: a faulting swap-in reports false; a faulting swap-back drops the record");
    auto       book = Fresh();
    FakeWidget w;
    w.state      = 1;
    w.swapFaults = true;
    EXPECT(!book.BeforeSetText(&w, &w));
    EXPECT(w.state == 1 && !book.AnyInside() && book.Counts().faults == 1);
    book.ReleaseAll();
    EXPECT(w.weak == 1);

    auto       again = Fresh();
    FakeWidget v;
    v.state = 1;
    PlayerTwoRun(again, v, 2);
    v.swapFaults = true;
    again.SwapBack();
    EXPECT(v.state == 2);  // the game's next SetText for player 1 lays player 1's out again
    EXPECT(g_freed.size() == 1 && g_freed[0] == 1);
    EXPECT(v.weak == 1 && again.Count() == 0 && again.Counts().faults == 1);
}

}  // namespace

int main()
{
    TestEmptyStateLayout();
    TestBootstrapThenOwnStates();
    TestSecondSetTextInOneRun();
    TestFontReloadResets();
    TestDiesInsidePlayerTwosPass();
    TestDiesAside();
    TestMiddleRecordDropped();
    TestReleaseAll();
    TestRefusalsAndFull();
    TestFaults();
    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all label state tests passed");
    return 0;
}
