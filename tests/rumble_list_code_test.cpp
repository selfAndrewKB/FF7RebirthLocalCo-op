// Host-side tests for the 1.004 jump box's list-load code (player_two/feedback/rumble_list_code.h), run without the
// game.
//
// The bytes are EXECUTED in a harness that recreates the site: rsi = the jump box's owner (its list at +0x3B0), the
// code, then rsi is returned. On the thread the route named, with player 2's list set, player 2's list is taken;
// on any other thread, or with no list, the game's own.

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
void*         g_listTwo = nullptr;  // player 2's list

void Fill(lss::test::ExecutablePage& page)
{
    const std::uint8_t before[] = {0x56,              // push rsi
                                   0x48, 0x89, 0xCE};  // mov rsi, rcx
    const std::uint8_t after[]  = {0x48, 0x89, 0xF0,  // mov rax, rsi
                                   0x5E,              // pop rsi
                                   0xC3};             // ret
    const auto         code     = JumpListLoadCode(&g_thread, &g_listTwo);
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
    g_listTwo = &listTwo;
    EXPECT(harness(owner) == &listTwo);

    g_listTwo = nullptr;  // the list dropped under the lock: the game's own
    EXPECT(harness(owner) == &gameList);

    g_thread  = thisThread + 1;  // another thread's add
    g_listTwo = &listTwo;
    EXPECT(harness(owner) == &gameList);

    g_thread = 0;  // no route
    EXPECT(harness(owner) == &gameList);
}

void TestTheBytes()
{
    std::puts("jump list load: the code starts with the game's own load and embeds the two addresses");
    const auto code = JumpListLoadCode(&g_thread, &g_listTwo);
    EXPECT(std::memcmp(code.data(), kJumpListLoadBytes, sizeof(kJumpListLoadBytes)) == 0);
    std::uint64_t embedded = 0;
    std::memcpy(&embedded, code.data() + 17, sizeof(embedded));
    EXPECT(embedded == reinterpret_cast<std::uint64_t>(&g_thread));
    std::memcpy(&embedded, code.data() + 31, sizeof(embedded));
    EXPECT(embedded == reinterpret_cast<std::uint64_t>(&g_listTwo));
    EXPECT(code[27] == 0x75 && code[28] == 0x14 && 29 + 0x14 == code.size());  // jne done lands at the end
}

}  // namespace

int main()
{
    TestTheBytes();
    TestWhichList();

    return lss::test::Report();
}
