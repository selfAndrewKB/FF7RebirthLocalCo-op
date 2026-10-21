#include "player_two/in_battle.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"

#include <atomic>
#include <cstdint>

namespace lss::player_two::facts {

namespace {

using TestFn = std::uint8_t (*)();

std::atomic<TestFn> g_isInBattle{nullptr};  // set once at startup, read from any thread

}  // namespace

bool InstallInBattle(const platform::GameImage& image) noexcept
{
    const auto found = platform::FindFunction(image, kIsInBattle);
    g_isInBattle.store(found ? reinterpret_cast<TestFn>(*found) : nullptr, std::memory_order_release);
    return found.has_value();
}

bool InBattleKnown() noexcept
{
    return g_isInBattle.load(std::memory_order_relaxed) != nullptr;
}

bool InBattle() noexcept
{
    const TestFn test   = g_isInBattle.load(std::memory_order_acquire);
    std::uint8_t answer = 0;
    return test != nullptr && platform::CallGuarded(test, answer) && answer != 0;
}

}  // namespace lss::player_two::facts
