// Host-side tests for the 1.004 jump box's list-load code (feedback/traversal_rumble_code.h), run without the
// game.
//
// The bytes are EXECUTED in a harness that recreates the site: rsi = the jump box's owner (its list at +0x3B0), the
// code, then rsi is returned. On a thread a route slot names, with that slot's list set, that list is taken; on any
// other thread, or with no list, the game's own. Two slots: two landings on two threads each take their own seat's.

#include "feedback/traversal_rumble_code.h"

#include "exec_page.h"
#include "expect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace lss::feedback::traversal_rumble_code;

/// void* harness(void* owner /*rcx*/): rsi after the code.
using Harness = void* (*)(void* owner);

JumpSlots g_slots{};

void Fill(lss::test::ExecutablePage& page)
{
    const std::uint8_t before[] = {0x56,              // push rsi
                                   0x48, 0x89, 0xCE};  // mov rsi, rcx
    const std::uint8_t after[]  = {0x48, 0x89, 0xF0,  // mov rax, rsi
                                   0x5E,              // pop rsi
                                   0xC3};             // ret
    const auto         code     = JumpListLoadCode(&g_slots);
    page.Write(0, before);
    page.Write(sizeof(before), code);
    page.Write(sizeof(before) + code.size(), after);
    page.Seal();
}

void Name(std::size_t slot, std::uint32_t thread, void* list)
{
    g_slots[slot].thread.store(thread);
    g_slots[slot].list.store(list);
}

struct Site
{
    alignas(16) std::uint8_t owner[0x400] = {};
    std::uint8_t             gameList     = 0;
    Site()
    {
        void* const at = &gameList;
        std::memcpy(owner + 0x3B0, &at, sizeof(at));
    }
};

void TestWhichList()
{
    std::puts("jump list load: the named thread's list when it exists, else the game's own, in either slot");
    Site                      site;
    std::uint8_t              listTwo = 0;
    lss::test::ExecutablePage page;
    Fill(page);
    const auto harness    = page.Function<Harness>();
    const auto thisThread = static_cast<std::uint32_t>(GetCurrentThreadId());

    for (std::size_t slot = 0; slot < kJumpSlots; ++slot)
    {
        Name(slot, thisThread, &listTwo);
        EXPECT(harness(site.owner) == &listTwo);

        Name(slot, thisThread, nullptr);  // the list dropped under the lock: the game's own
        EXPECT(harness(site.owner) == &site.gameList);

        Name(slot, thisThread + 1, &listTwo);  // another thread's add
        EXPECT(harness(site.owner) == &site.gameList);

        Name(slot, 0, nullptr);  // no route
        EXPECT(harness(site.owner) == &site.gameList);
    }
}

void TestTwoLandings()
{
    std::puts("jump list load: two landings on two threads each take their own seat's list");
    Site                      site;
    std::uint8_t              listTwo = 0, listThree = 0;
    lss::test::ExecutablePage page;
    Fill(page);
    const auto harness    = page.Function<Harness>();
    const auto thisThread = static_cast<std::uint32_t>(GetCurrentThreadId());

    Name(0, thisThread + 1, &listTwo);  // player 2's landing on another worker
    Name(1, thisThread, &listThree);    // player 3's on this one
    EXPECT(harness(site.owner) == &listThree);
    Name(0, thisThread, &listTwo);  // swapped: this thread is slot 0's now
    Name(1, thisThread + 1, &listThree);
    EXPECT(harness(site.owner) == &listTwo);
    Name(0, 0, nullptr);
    EXPECT(harness(site.owner) == &site.gameList);  // slot 1 names another thread
    Name(1, 0, nullptr);
}

void TestTheBytes()
{
    std::puts("jump list load: the code starts with the game's own load, embeds the slots' address, every jump lands");
    const auto code = JumpListLoadCode(&g_slots);
    EXPECT(std::memcmp(code.data(), kJumpListLoadBytes, sizeof(kJumpListLoadBytes)) == 0);
    std::uint64_t embedded = 0;
    std::memcpy(&embedded, code.data() + 17, sizeof(embedded));
    EXPECT(embedded == reinterpret_cast<std::uint64_t>(&g_slots));
    EXPECT(code[27] == 0x75 && 29 + code[28] == 35);                // jne second
    EXPECT(code[33] == 0xEB && 35 + code[34] == 44);                // jmp take
    EXPECT(code[38] == 0x75 && std::size_t{40} + code[39] == code.size());  // jne done lands at the end
    static_assert(kJumpListLoadCodeSize == 51, "the code placed at the site");
}

}  // namespace

int main()
{
    TestTheBytes();
    TestWhichList();
    TestTwoLandings();

    return lss::test::Report();
}
