#include "engine_tick.h"

#include "platform/entry_redirect.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace lss::engine_tick {

namespace {

// UGameEngine::Tick(float DeltaSeconds, bool bIdleMode), the function the engine object's vtable holds (the game's
// UEndGameEngine override is a thunk that jumps here; the live engine is a plain UGameEngine in both builds).
// Research: research/RE-engine-tick-entry.md, .ida-mcp/1004_UGameEngine_Tick.c, 1005_UGameEngine_Tick.c. Its entry:
// the two home-slot stores (bIdleMode, this), then push rbp, push rbx; the first instruction is five
// position-independent bytes inside one 16-byte block, which the entry redirect replaces. The function re-reads
// bIdleMode from its home slot later, so the trampoline replays the store.
constexpr std::uint8_t kTickBytes[] = {0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53};
constexpr platform::FunctionFact kTick{L"UGameEngine::Tick", 0x14109F840, kTickBytes};

/// The Tick entry of UGameEngine's vtable: UE4SS's UEngine slot 10 after the UObject virtuals (77 in 1.004, 78 in
/// 1.005, which inserted one). The research build's address; 1.005's is in replaced-engine-tick.json.
constexpr std::uintptr_t kTickSlotEntry = 0x146B9A728;

using TickFn = void (*)(void* engine, float deltaSeconds, bool idleMode);

/// What a tick calls, published as one pointer so a tick never sees a callback without its self.
struct Target
{
    Callback callback;
    void*    self;
};

platform::EntryRedirect g_entry;
Target                  g_record{};             // written by Start before it is published; one Start per run
std::atomic<Target*>    g_target{nullptr};
std::atomic<int>        g_inFlight{0};          // ticks inside the callback now (Stop waits for them)

void Tick(void* engine, float deltaSeconds, bool idleMode)
{
    g_inFlight.fetch_add(1, std::memory_order_acquire);
    if (const Target* target = g_target.load(std::memory_order_acquire))
    {
        target->callback(target->self);
    }
    g_inFlight.fetch_sub(1, std::memory_order_release);
    if (const auto original = g_entry.Original())
    {
        reinterpret_cast<TickFn>(original)(engine, deltaSeconds, idleMode);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (g_entry.IsInstalled())
    {
        return true;
    }
    const auto tick = platform::FindFunction(image, kTick);  // logs a mismatch
    const auto slot = image.ReadPointer(kTickSlotEntry);
    if (!tick || !slot || *slot != *tick)
    {
        platform::Log(L"check failed: UGameEngine's vtable does not hold UGameEngine::Tick where the research read it "
                      L"(a game update, or another mod replaced it)");
        return false;
    }
    if (!g_entry.Install(*tick, image.EntryInstruction(kTick.address, {kTickBytes, 5}),
                         reinterpret_cast<std::uintptr_t>(&Tick)))
    {
        platform::Log(L"check failed: UGameEngine::Tick's entry could not be redirected (its first instruction is not "
                      L"the one the research read)");
        return false;
    }
    platform::Log(L"engine tick: UGameEngine::Tick's entry runs through the mod (before the worlds tick)");
    return true;
}

void Start(Callback callback, void* self) noexcept
{
    g_record = {callback, self};
    g_target.store(&g_record, std::memory_order_release);
}

void Stop() noexcept
{
    g_target.store(nullptr, std::memory_order_release);
    // A tick already inside the callback finishes before the caller tears the mod down (the unloader may run off
    // the game thread; on the game thread no tick can be in flight).
    if (!players::generation::OnGameThread())
    {
        while (g_inFlight.load(std::memory_order_acquire) > 0)
        {
            std::this_thread::yield();
        }
    }
}

}  // namespace lss::engine_tick
