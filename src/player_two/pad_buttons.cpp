#include "player_two/pad_buttons.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/NameTypes.hpp>

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::player_two::pad_buttons {

namespace {

// The input processor's Tick (RE-p2-per-pad-buttons.md §1.3 step 5): IInputProcessor vtable
// 0x145D40580, slot 1. (this, float DeltaTime, FSlateApplication&, TSharedRef<ICursor> by address.)
constexpr std::uint8_t kInputTickBytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
                                            0x57, 0x48, 0x8D, 0xAC, 0x24, 0x18, 0xFD, 0xFF, 0xFF, 0xB8, 0xE8, 0x03};
constexpr platform::SlotFact kInputTick{L"the Square input processor's Tick slot (1)", 0x145D40588,
                                        {L"SquareInput__InputProcessorTick_AggregateDevices", 0x1407C057C, kInputTickBytes}};

using TickFn = void (*)(void* self, float deltaTime, void* application, void* cursor);

// A TSet<element> in the input object, stock TSparseArray layout (§1.1): data, ArrayNum, the
// allocation bits (4 inline dwords, a heap pointer, NumBits, MaxBits), FirstFreeIndex, NumFree.
struct TableFields
{
    std::uint32_t offset;
    const wchar_t* name;
};
constexpr std::array<TableFields, 2> kTables{{{0xF8, L"raw"}, {0x148, L"UI"}}};
constexpr std::uint32_t kTableNum        = 0x08;
constexpr std::uint32_t kTableInlineBits = 0x10;
constexpr std::uint32_t kTableHeapBits   = 0x20;
constexpr std::uint32_t kTableMaxBits    = 0x2C;
constexpr std::uint32_t kTableNumFree    = 0x34;
constexpr std::uint32_t kInlineBitCount  = 128;
constexpr std::uint32_t kElementSize     = 40;
constexpr std::uint32_t kElementState    = 0x18;  // S0 prev-held, S1 held, S2 repeat, S3 latch, then u32 S4 sources
constexpr std::int32_t  kMaxElements     = 1024;
constexpr unsigned      kLinesPerStatus  = 120;

struct State
{
    std::uint8_t  previous = 0;
    std::uint8_t  held     = 0;
    std::uint8_t  repeat   = 0;
    std::uint8_t  latch    = 0;
    std::uint32_t sources  = 0;
};

// Set once at install.
platform::VTableEntry     g_tickSlot;
TickFn                    g_engineTick = nullptr;
const std::byte* const*   g_inputOwner = nullptr;

// Game thread (the Tick runs on it).
std::array<std::vector<State>, kTables.size()> g_last;
unsigned g_lines   = 0;
unsigned g_dropped = 0;
bool     g_ownerLogged = false;

template <class T>
bool Read(const std::byte* at, T& out) noexcept
{
    return platform::CopyGuarded(&out, at, sizeof(T));
}

bool Allocated(const std::byte* table, std::int32_t index) noexcept
{
    std::uint32_t maxBits = 0;
    if (!Read(table + kTableMaxBits, maxBits))
    {
        return false;
    }
    const std::byte* bits = table + kTableInlineBits;
    if (maxBits > kInlineBitCount && !Read(table + kTableHeapBits, bits))
    {
        return false;
    }
    std::uint32_t word = 0;
    return bits != nullptr && Read(bits + (index / 32) * sizeof(std::uint32_t), word) && ((word >> (index % 32)) & 1) != 0;
}

std::wstring NameOf(std::uint64_t name)
{
    return RC::Unreal::FName(static_cast<std::int64_t>(name)).ToString();
}

void LogChange(const wchar_t* table, std::uint64_t name, const State& was, const State& now)
{
    if (g_lines >= kLinesPerStatus)
    {
        ++g_dropped;
        return;
    }
    ++g_lines;
    platform::Log(L"buttons ({}): {} held {}->{} prev {} repeat {} latch {} sources 0x{:08X}->0x{:08X}", table, NameOf(name),
                  was.held, now.held, now.previous, now.repeat, now.latch, was.sources, now.sources);
}

void Watch(const std::byte* owner, std::size_t which)
{
    const std::byte* table = owner + kTables[which].offset;
    const std::byte* data  = nullptr;
    std::int32_t     count = 0;
    if (!Read(table, data) || !Read(table + kTableNum, count) || data == nullptr || count < 0 || count > kMaxElements)
    {
        return;
    }
    auto& last = g_last[which];
    last.resize(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i)
    {
        const std::byte* element = data + static_cast<std::size_t>(i) * kElementSize;
        State            now{};
        std::uint64_t    name = 0;
        if (!Allocated(table, i) || !Read(element, name) || !Read(element + kElementState, now))
        {
            continue;
        }
        State& was = last[static_cast<std::size_t>(i)];
        if (now.held != was.held || now.sources != was.sources)
        {
            LogChange(kTables[which].name, name, was, now);
        }
        was = now;
    }
}

void Tick(void* self, float deltaTime, void* application, void* cursor)
{
    g_engineTick(self, deltaTime, application, cursor);
    if (!generation::OnGameThread() || !generation::Active() || g_inputOwner == nullptr)
    {
        return;
    }
    try
    {
        const std::byte* owner = *g_inputOwner;
        if (owner == nullptr)
        {
            return;
        }
        if (!g_ownerLogged)
        {
            g_ownerLogged = true;
            platform::Log(L"buttons: input object {} is {} the input processor the Tick runs on", static_cast<const void*>(owner),
                          static_cast<const void*>(owner) == self ? L"also" : L"NOT");
        }
        for (std::size_t which = 0; which < kTables.size(); ++which)
        {
            Watch(owner, which);
        }
    }
    catch (...)
    {
        // Only the log allocates; a failed line is skipped.
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto owner  = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    const auto engine = platform::FindFunction(image, kInputTick.target);
    if (!owner || !engine)
    {
        return false;
    }
    g_inputOwner = reinterpret_cast<const std::byte* const*>(*owner);
    g_engineTick = reinterpret_cast<TickFn>(*engine);
    return platform::ReplaceSlot(image, kInputTick, reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot);
}

void Remove() noexcept
{
    if (!g_tickSlot.Restore())
    {
        platform::Log(L"buttons: the input Tick slot was changed after us; left in place (it passes the call through)");
    }
}

void LogStatus() noexcept
{
    platform::Log(L"  buttons: {} change line(s) logged, {} dropped; tables raw {} / UI {} elements", g_lines, g_dropped,
                  g_last[0].size(), g_last[1].size());
    g_lines   = 0;
    g_dropped = 0;
}

}  // namespace lss::player_two::pad_buttons
