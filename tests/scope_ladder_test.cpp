// Host-side tests for scopes/scope_ladder, run without the game.
//
// Recording parts pin the ladder's rules: the chosen parts open in the pack's order and close in reverse; a
// required part that refuses stops the ladder there (the parts before it close at once, nothing after it opens,
// the ladder is not held, the part names itself or gives its own words); an optional part that refuses is left
// out, noted, and the ladder goes on, held.

#include "scopes/scope_ladder.h"

#include "expect.h"

#include <cstdio>
#include <string>

namespace {

using lss::scopes::Ladder;

struct Args
{
    char refuse = 0;    // the part this letter names refuses
    bool words  = false;  // the refusing part gives its own words
};

std::string g_log;  // "+A" when A opens (held or not), "-A" when it closes

template <char Letter, unsigned Bit>
struct Part
{
    static constexpr unsigned       kBit  = Bit;
    static constexpr const wchar_t* kName = Letter == 'A' ? L"A" : Letter == 'B' ? L"B" : L"C";

    explicit Part(const Args& args) noexcept : held(args.refuse != Letter), words(args.words)
    {
        g_log += '+';
        g_log += Letter;
    }
    ~Part()
    {
        g_log += '-';
        g_log += Letter;
    }
    Part(const Part&)            = delete;
    Part& operator=(const Part&) = delete;

    bool           Held() const noexcept { return held; }
    const wchar_t* Refusal() const noexcept { return held || !words ? nullptr : L"its own words"; }

    bool held;
    bool words;
};

using A = Part<'A', 1>;
using B = Part<'B', 2>;
using C = Part<'C', 4>;
using L = Ladder<Args, A, B, C>;

void TestOrder()
{
    std::puts("scope ladder: the chosen parts open in the pack's order and close in reverse");
    g_log.clear();
    {
        const L ladder(1 | 2 | 4, Args{});
        EXPECT(ladder.Held() && ladder.Refusal() == nullptr);
        EXPECT(ladder.Holds<A>() && ladder.Holds<B>() && ladder.Holds<C>());
        EXPECT(g_log == "+A+B+C");
    }
    EXPECT(g_log == "+A+B+C-C-B-A");

    g_log.clear();
    {
        const L ladder(1 | 4, Args{});  // B not chosen
        EXPECT(ladder.Held() && ladder.Holds<A>() && !ladder.Holds<B>() && ladder.Holds<C>());
        EXPECT(ladder.Get<B>() == nullptr && ladder.Get<C>() != nullptr);
    }
    EXPECT(g_log == "+A+C-C-A");

    g_log.clear();
    {
        const L ladder(0, Args{});  // nothing chosen: held, trivially
        EXPECT(ladder.Held());
    }
    EXPECT(g_log.empty());
}

void TestRequiredRefusal()
{
    std::puts("scope ladder: a required part that refuses stops the ladder; nothing of it stays open");
    g_log.clear();
    {
        const L ladder(1 | 2 | 4, Args{'B'});
        EXPECT(!ladder.Held());
        EXPECT(!ladder.Holds<A>() && !ladder.Holds<B>() && !ladder.Holds<C>());
        EXPECT(std::wstring(ladder.Refusal()) == L"B" && ladder.Noted() == nullptr);
        EXPECT(g_log == "+A+B-B-A");  // B closed at once, then A; C never opened
    }
    EXPECT(g_log == "+A+B-B-A");  // nothing left for the destructor

    g_log.clear();
    {
        const L ladder(1 | 2 | 4, Args{'B', true});
        EXPECT(!ladder.Held() && std::wstring(ladder.Refusal()) == L"its own words");
    }

    g_log.clear();
    {
        const L ladder(1 | 2 | 4, Args{'A'});  // the first part refuses: nothing else opens
        EXPECT(!ladder.Held() && std::wstring(ladder.Refusal()) == L"A");
    }
    EXPECT(g_log == "+A-A");
}

void TestOptionalRefusal()
{
    std::puts("scope ladder: an optional part that refuses is left out and noted; the ladder goes on, held");
    g_log.clear();
    {
        const L ladder(1 | 2 | 4, 2, Args{'B'});
        EXPECT(ladder.Held() && ladder.Refusal() == nullptr);
        EXPECT(std::wstring(ladder.Noted()) == L"B");
        EXPECT(ladder.Holds<A>() && !ladder.Holds<B>() && ladder.Holds<C>());
        EXPECT(g_log == "+A+B-B+C");
    }
    EXPECT(g_log == "+A+B-B+C-C-A");

    g_log.clear();
    {
        const L ladder(1 | 2 | 4, 2, Args{'B', true});
        EXPECT(ladder.Held() && std::wstring(ladder.Noted()) == L"its own words");
    }

    g_log.clear();
    {
        const L ladder(1 | 2 | 4, 2, Args{'C'});  // an optional B holds; the required C refuses: all closed
        EXPECT(!ladder.Held() && std::wstring(ladder.Refusal()) == L"C" && ladder.Noted() == nullptr);
        EXPECT(!ladder.Holds<A>() && !ladder.Holds<B>() && !ladder.Holds<C>());
        EXPECT(g_log == "+A+B+C-C-B-A");
    }
    EXPECT(g_log == "+A+B+C-C-B-A");
}

}  // namespace

int main()
{
    TestOrder();
    TestRequiredRefusal();
    TestOptionalRefusal();

    return lss::test::Report();
}
