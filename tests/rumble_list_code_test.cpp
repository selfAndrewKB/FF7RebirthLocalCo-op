// Host-side tests for the 1.004 jump box's list-load code (player_two/feedback/rumble_list_code.h), run without the
// game.
//
// The bytes are EXECUTED in a harness that recreates the site: rsi = the jump box's owner (its list at +0x3B0), the
// code, then rsi is returned. On the thread the route named, with the route's list set, that list is taken; on any
// other thread, or with no list, the game's own. The route names one seat's list at a time (player 2's or player 3's):
// the same bytes take whichever it names.

#include "player_two/feedback/rumble_list_code.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace lss::player_two::rumble_list_code;

/// void* harness(void* owner /*rcx*/): rsi after the code.
using Harness = void* (*)(void* owner);

std::uint32_t g_thread = 0;        // the id the route named (0: none)
void*         g_routeList = nullptr;  // the list the route named (a seat's)

void Fill(lss::test::ExecutablePage& page)
{
    const std::uint8_t before[] = {0x56,              // push rsi
                                   0x48, 0x89, 0xCE};  // mov rsi, rcx
    const std::uint8_t after[]  = {0x48, 0x89, 0xF0,  // mov rax, rsi
                                   0x5E,              // pop rsi
                                   0xC3};             // ret
    const auto         code     = JumpListLoadCode(&g_thread, &g_routeList);
    page.Write(0, before);
    page.Write(sizeof(before), code);
    page.Write(sizeof(before) + code.size(), after);
    page.Seal();
}

void TestWhichList()
{
    std::puts("jump list load: player 2's list on the named thread when it exists, else the game's own");
    alignas(16) std::uint8_t owner[0x400] = {};
    std::uint8_t             gameList     = 0;
    std::uint8_t             listTwo      = 0;
    void* const              gameListAt   = &gameList;
    std::memcpy(owner + 0x3B0, &gameListAt, sizeof(gameListAt));

    lss::test::ExecutablePage page;
    Fill(page);
    const auto harness = page.Function<Harness>();
    const auto thisThread = static_cast<std::uint32_t>(GetCurrentThreadId());

    g_thread  = thisThread;
    g_routeList = &listTwo;
    EXPECT(harness(owner) == &listTwo);

    g_routeList = nullptr;  // the list dropped under the lock: the game's own
    EXPECT(harness(owner) == &gameList);

    g_thread  = thisThread + 1;  // another thread's add
    g_routeList = &listTwo;
    EXPECT(harness(owner) == &gameList);

    g_thread = 0;  // no route
    EXPECT(harness(owner) == &gameList);
}

void TestTheBytes()
{
    std::puts("jump list load: the code starts with the game's own load and embeds the two addresses");
    const auto code = JumpListLoadCode(&g_thread, &g_routeList);
    EXPECT(std::memcmp(code.data(), kJumpListLoadBytes, sizeof(kJumpListLoadBytes)) == 0);
    std::uint64_t embedded = 0;
    std::memcpy(&embedded, code.data() + 17, sizeof(embedded));
    EXPECT(embedded == reinterpret_cast<std::uint64_t>(&g_thread));
    std::memcpy(&embedded, code.data() + 31, sizeof(embedded));
    EXPECT(embedded == reinterpret_cast<std::uint64_t>(&g_routeList));
    EXPECT(code[27] == 0x75 && code[28] == 0x14 && 29 + 0x14 == code.size());  // jne done lands at the end
}

void TestEachSeatsList()
{
    std::puts("jump list load: the route's list is whichever seat's it names; the bytes stay the same size");
    static_assert(kJumpListLoadCodeSize == 49, "the code fits the 49 bytes placed at the site");
    alignas(16) std::uint8_t owner[0x400] = {};
    std::uint8_t             gameList     = 0;
    std::uint8_t             listTwo      = 0;  // player 2's
    std::uint8_t             listThree    = 0;  // player 3's
    void* const              gameListAt   = &gameList;
    std::memcpy(owner + 0x3B0, &gameListAt, sizeof(gameListAt));

    lss::test::ExecutablePage page;
    Fill(page);
    const auto harness    = page.Function<Harness>();
    const auto thisThread = static_cast<std::uint32_t>(GetCurrentThreadId());

    g_thread  = thisThread;
    g_routeList = &listThree;  // player 3's chocobo landed: the route names player 3's list
    EXPECT(harness(owner) == &listThree);
    g_routeList = &listTwo;    // then player 2's
    EXPECT(harness(owner) == &listTwo);
    g_routeList = nullptr;     // the route over (its list cleared first)
    EXPECT(harness(owner) == &gameList);
    g_thread = 0;
}

}  // namespace

int main()
{
    TestTheBytes();
    TestWhichList();
    TestEachSeatsList();

    return lss::test::Report();
}
