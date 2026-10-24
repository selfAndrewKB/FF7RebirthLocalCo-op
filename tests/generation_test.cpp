// Host-side tests for player_two/generation's per-tick identities and its seats, run without the game.
//
// The weak pointers need the engine's resolver (player_two/weak_object), which the host does not have: they read 0
// here. What is tested is the publishing: the chocobo given to BeginTick is readable while the seat's generation is
// active, and every identity is cleared the moment it is withdrawn, ended or faulted, or a tick finds it inactive;
// and the seats are kept apart: one seat's withdraw or fault leaves the other's, a fault without a seat withdraws all.

#include "player_two/generation.h"

#include "expect.h"
#include "player_two/weak_object.h"

#include <array>
#include <cstddef>
#include <cstdio>

namespace {

namespace generation = lss::player_two::generation;
using lss::players::kMaxPlayers;

constexpr int kTwo   = 1;  // player 2's seat
constexpr int kThree = 2;  // player 3's seat

bool NoWeaks()
{
    return generation::MemberWeak() == 0 && generation::PlayerOnePawnWeak() == 0 && generation::ChocoboWeak() == 0;
}

void Tick(const void* two, const void* three = nullptr)
{
    const void* const chocobos[kMaxPlayers] = {nullptr, two, three};
    generation::BeginTick(chocobos);
}

void TestInactive()
{
    std::puts("generation: nothing is published while inactive");
    std::byte chocobo{};
    Tick(&chocobo, &chocobo);
    EXPECT(generation::Chocobo() == nullptr);
    EXPECT(generation::ChocoboOf(kThree) == nullptr);
    EXPECT(!generation::AnyActive());
    EXPECT(NoWeaks());
}

void TestPublishedAndCleared()
{
    std::puts("generation: the tick's chocobo is published while active and cleared with the generation");
    std::array<std::byte, 0x2000> one{}, two{};  // stand-in controllers: every read of them finds zero
    std::byte                     member{}, chocobo{};
    EXPECT(!lss::player_two::weak_object::Known());  // the host has no resolver: every weak reads 0
    generation::Begin(kTwo, one.data(), two.data());
    generation::SetMember(kTwo, &member);
    Tick(&chocobo);
    EXPECT(generation::Active());
    EXPECT(generation::Chocobo() == &chocobo);
    EXPECT(generation::PlayerOneController() == one.data() && generation::PlayerTwoController() == two.data());
    EXPECT(NoWeaks());
    Tick(nullptr);
    EXPECT(generation::Chocobo() == nullptr);
    Tick(&chocobo);
    generation::Withdraw(kTwo);
    EXPECT(generation::Chocobo() == nullptr);

    generation::Begin(kTwo, one.data(), two.data());
    Tick(&chocobo);
    EXPECT(generation::Chocobo() == &chocobo);
    generation::Fault(kTwo, L"a test fault");
    EXPECT(generation::Chocobo() == nullptr);
    EXPECT(!generation::Active());
    EXPECT(generation::FaultReason() != nullptr);

    generation::Begin(kTwo, one.data(), two.data());
    EXPECT(generation::FaultReason() == nullptr);  // a new generation forgets the last one's fault
    Tick(&chocobo);
    generation::End(kTwo);
    EXPECT(generation::Chocobo() == nullptr && generation::PlayerTwoController() == nullptr);
    EXPECT(generation::PlayerOneController() == one.data());  // player 1's lives on

    generation::Begin(kTwo, one.data(), two.data());
    Tick(&chocobo);
    generation::Withdraw(kTwo);
    Tick(&chocobo);  // an inactive tick publishes nothing
    EXPECT(generation::Chocobo() == nullptr);
    generation::End(kTwo);
}

void TestSeatsApart()
{
    std::puts("generation: two seats are kept apart; a fault without a seat withdraws both");
    std::array<std::byte, 0x2000> one{}, two{}, three{};
    std::byte                     memberTwo{}, memberThree{}, chocoboTwo{}, chocoboThree{};
    generation::Begin(kTwo, one.data(), two.data());
    generation::Begin(kThree, one.data(), three.data());
    generation::SetMember(kTwo, &memberTwo);
    generation::SetMember(kThree, &memberThree);
    Tick(&chocoboTwo, &chocoboThree);
    EXPECT(generation::Active(kTwo) && generation::Active(kThree));
    EXPECT(generation::ChocoboOf(kTwo) == &chocoboTwo && generation::ChocoboOf(kThree) == &chocoboThree);
    EXPECT(generation::SeatOfController(two.data()) == kTwo);
    EXPECT(generation::SeatOfController(three.data()) == kThree);
    EXPECT(generation::SeatOfController(one.data()) == 0);
    EXPECT(generation::SeatOfController(&memberTwo) == -1);
    EXPECT(generation::SeatOfController(nullptr) == -1);
    EXPECT(generation::SeatOfMember(&memberThree) == kThree);
    EXPECT(generation::SeatOfMember(&memberTwo) == kTwo);
    EXPECT(generation::SeatOfMember(nullptr) == -1);
    EXPECT(generation::SeatOfMemberWeak(0) == -1);  // no weak (the host's) names no seat

    generation::Withdraw(kTwo);  // player 2 leaves; player 3 plays on
    EXPECT(!generation::Active(kTwo) && generation::Active(kThree) && generation::AnyActive());
    EXPECT(generation::ChocoboOf(kTwo) == nullptr && generation::ChocoboOf(kThree) == &chocoboThree);
    generation::Fault(kThree, L"player 3's fault");
    EXPECT(generation::FaultReason(kTwo) == nullptr && generation::FaultReason(kThree) != nullptr);
    EXPECT(!generation::AnyActive());

    generation::Begin(kTwo, one.data(), two.data());
    generation::Begin(kThree, one.data(), three.data());
    generation::Fault(L"a fault no seat can be named for");
    EXPECT(!generation::Active(kTwo) && !generation::Active(kThree));
    EXPECT(generation::FaultReason(kTwo) != nullptr && generation::FaultReason(kThree) != nullptr);
    generation::End(kTwo);
    generation::End(kThree);
    EXPECT(generation::SeatOfController(two.data()) == -1 && generation::MemberOf(kThree) == nullptr);
    EXPECT(generation::SeatOfController(one.data()) == -1);  // player 1's kept, but named only while a seat plays

    EXPECT(!generation::Active(0) && !generation::Active(kMaxPlayers));  // no seat outside the records
    EXPECT(generation::MemberOf(-1) == nullptr && generation::ControllerOf(kMaxPlayers) == nullptr);
}

}  // namespace

int main()
{
    TestInactive();
    TestPublishedAndCleared();
    TestSeatsApart();

    return lss::test::Report();
}
