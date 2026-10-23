// Host-side tests for player_two::leader_answer::Decide (player_two/scopes/leader_answer.h), run without the game: the
// five
// answer sources' priority, the game-thread rule, the counting, and when the component's type is read.

#include "player_two/scopes/leader_answer.h"

#include "expect.h"

#include <cstdio>

namespace {

using lss::player_two::leader_answer::Answer;
using lss::player_two::leader_answer::Decide;
using lss::player_two::leader_answer::kNoType;
using lss::player_two::leader_answer::Verdict;

/// A component of player type `type`, counting how often Decide asked for it.
struct Component
{
    int         type;
    mutable int reads = 0;
    int         operator()() const noexcept
    {
        ++reads;
        return type;
    }
};

bool Is(const Answer& answer, Verdict verdict, bool counted)
{
    return answer.verdict == verdict && answer.counted == counted;
}

void OnlyType()
{
    std::puts("leader answer: one type counts as the player and every other as not, over every other source");
    const Component five{5};
    EXPECT(Is(Decide(5, 7, 7, 7, true, true, true, five), Verdict::Yes, false));
    EXPECT(Is(Decide(3, 5, 5, 5, true, true, true, five), Verdict::No, false));
    EXPECT(Is(Decide(3, kNoType, kNoType, kNoType, false, false, false, five), Verdict::No, false));
}

void ThisThread()
{
    std::puts("leader answer: the climb's type over the thread's, then the thread's; any thread; counted");
    const Component five{5};
    EXPECT(Is(Decide(kNoType, 5, kNoType, kNoType, false, false, false, five), Verdict::Yes, true));
    EXPECT(Is(Decide(kNoType, kNoType, 5, kNoType, false, false, false, five), Verdict::Yes, true));
    EXPECT(Is(Decide(kNoType, 3, 5, kNoType, false, false, false, five), Verdict::Engine, false));  // the climb masks
    EXPECT(Is(Decide(kNoType, 3, 5, kNoType, false, false, true, five), Verdict::Engine, false));
}

void GameThread()
{
    std::puts("leader answer: the member tick's type and its component answer on the game thread only");
    const Component five{5};
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, 5, false, false, true, five), Verdict::Yes, true));
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, 5, false, false, false, five), Verdict::Engine, false));
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, kNoType, true, true, true, five), Verdict::Yes, true));
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, kNoType, true, true, false, five), Verdict::Engine, false));
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, kNoType, true, false, true, five), Verdict::Engine, false));
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, 3, true, false, true, five), Verdict::Engine, false));
}

void FallThrough()
{
    std::puts("leader answer: a missed compare falls through to the next source; each source reads the type once");
    const Component five{5};
    EXPECT(Is(Decide(kNoType, 3, kNoType, 5, false, false, true, five), Verdict::Yes, true));  // the climb misses
    EXPECT(five.reads == 2);
    const Component same{5};
    EXPECT(Is(Decide(kNoType, 5, 5, kNoType, false, false, false, same), Verdict::Yes, true));
    EXPECT(same.reads == 1);
    const Component first{5};
    EXPECT(Is(Decide(kNoType, kNoType, 5, 7, false, false, true, first), Verdict::Yes, true));  // source 3 before 4
    EXPECT(first.reads == 1);
    const Component byAddress{5};
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, 7, true, true, true, byAddress), Verdict::Yes, true));
    EXPECT(byAddress.reads == 0);  // the type mismatches, the address matches: no read
    const Component only{5};
    EXPECT(Is(Decide(5, kNoType, kNoType, kNoType, false, false, false, only), Verdict::Yes, false));
    EXPECT(only.reads == 1);
    EXPECT(Is(Decide(3, kNoType, kNoType, kNoType, false, false, false, only), Verdict::No, false));
    EXPECT(only.reads == 2);
    const Component unreadable{kNoType};
    EXPECT(Is(Decide(3, kNoType, kNoType, kNoType, false, false, false, unreadable), Verdict::No, false));
}

void Reads()
{
    std::puts("leader answer: the component's type is read only when a source compares it");
    const Component five{5};
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, kNoType, false, false, true, five), Verdict::Engine, false));
    EXPECT(five.reads == 0);
    EXPECT(Is(Decide(kNoType, kNoType, kNoType, kNoType, true, true, true, five), Verdict::Yes, true));
    EXPECT(five.reads == 0);  // the component matched by address
    EXPECT(Is(Decide(kNoType, kNoType, 5, kNoType, false, false, false, five), Verdict::Yes, true));
    EXPECT(five.reads == 1);
}

}  // namespace

int main()
{
    OnlyType();
    ThisThread();
    GameThread();
    FallThrough();
    Reads();
    return lss::test::Report();
}
