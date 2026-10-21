// Host-side tests for player_two/generation's per-tick identities, run without the game.
//
// The weak pointers need the engine's resolver (player_two/weak_object), which the host does not have: they read 0
// here. What is tested is the publishing: the chocobo given to BeginTick is readable while the generation is active,
// and every identity is cleared the moment the generation is withdrawn, ended or faulted, or a tick finds it inactive.

#include "player_two/generation.h"

#include "expect.h"
#include "player_two/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdio>

namespace {

namespace generation = lss::player_two::generation;

bool NoWeaks()
{
    return generation::MemberWeak() == 0 && generation::PlayerOnePawnWeak() == 0 && generation::ChocoboWeak() == 0;
}

void TestInactive()
{
    std::puts("generation: nothing is published while inactive");
    std::byte chocobo{};
    generation::BeginTick(&chocobo);
    EXPECT(generation::Chocobo() == nullptr);
    EXPECT(NoWeaks());
}

void TestPublishedAndCleared()
{
    std::puts("generation: the tick's chocobo is published while active and cleared with the generation");
    std::array<std::byte, 0x2000> one{}, two{};  // stand-in controllers: every read of them finds zero
    std::byte                     member{}, chocobo{};
    EXPECT(!lss::player_two::weak_object::Known());  // the host has no resolver: every weak reads 0
    generation::Begin(one.data(), two.data());
    generation::SetMember(&member);
    generation::BeginTick(&chocobo);
    EXPECT(generation::Active());
    EXPECT(generation::Chocobo() == &chocobo);
    EXPECT(NoWeaks());
    generation::BeginTick(nullptr);
    EXPECT(generation::Chocobo() == nullptr);
    generation::BeginTick(&chocobo);
    generation::Withdraw();
    EXPECT(generation::Chocobo() == nullptr);

    generation::Begin(one.data(), two.data());
    generation::BeginTick(&chocobo);
    EXPECT(generation::Chocobo() == &chocobo);
    generation::Fault(L"a test fault");
    EXPECT(generation::Chocobo() == nullptr);
    EXPECT(!generation::Active());

    generation::Begin(one.data(), two.data());
    generation::BeginTick(&chocobo);
    generation::End();
    EXPECT(generation::Chocobo() == nullptr && generation::PlayerTwoController() == nullptr);

    generation::Begin(one.data(), two.data());
    generation::BeginTick(&chocobo);
    generation::Withdraw();
    generation::BeginTick(&chocobo);  // an inactive tick publishes nothing
    EXPECT(generation::Chocobo() == nullptr);
}

}  // namespace

int main()
{
    TestInactive();
    TestPublishedAndCleared();

    return lss::test::Report();
}