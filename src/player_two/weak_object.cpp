#include "player_two/weak_object.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/verified_code.h"

#include <atomic>

namespace lss::player_two::weak_object {

namespace {

using GetFn    = void* (*)(const std::uint64_t* weak);
using AssignFn = void (*)(std::uint64_t* weak, void* object);

std::atomic<GetFn>    g_get{nullptr};     // set once at startup, read from any thread
std::atomic<AssignFn> g_assign{nullptr};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto get    = platform::FindFunction(image, facts::kWeakObjectGet);
    const auto assign = platform::FindFunction(image, facts::kWeakObjectAssign);
    const bool found  = get && assign;
    g_get.store(found ? reinterpret_cast<GetFn>(*get) : nullptr, std::memory_order_release);
    g_assign.store(found ? reinterpret_cast<AssignFn>(*assign) : nullptr, std::memory_order_release);
    return found;
}

bool Known() noexcept
{
    return g_get.load(std::memory_order_relaxed) != nullptr;
}

void* ObjectOf(std::uint64_t weak) noexcept
{
    const GetFn get  = g_get.load(std::memory_order_acquire);
    void*       live = nullptr;
    return (weak >> 32) != 0 && get != nullptr && platform::CallGuarded(get, live, &weak) ? live : nullptr;
}

void* ObjectAt(const void* field) noexcept
{
    std::uint64_t weak = 0;
    return platform::Read(field, weak) ? ObjectOf(weak) : nullptr;
}

bool Assign(std::uint64_t* field, void* object) noexcept
{
    const AssignFn assign = g_assign.load(std::memory_order_acquire);
    return field != nullptr && assign != nullptr && platform::CallGuarded(assign, field, object);
}

std::uint64_t WeakOf(void* object) noexcept
{
    std::uint64_t weak = 0;
    return object != nullptr && Assign(&weak, object) && (weak >> 32) != 0 && ObjectOf(weak) == object ? weak : 0;
}

bool StillNames(std::uint64_t weak, const void* object) noexcept
{
    return weak != 0 && object != nullptr && ObjectOf(weak) == object;
}

}  // namespace lss::player_two::weak_object
