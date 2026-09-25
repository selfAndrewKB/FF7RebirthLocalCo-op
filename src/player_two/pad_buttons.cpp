#include "player_two/pad_buttons.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/NameTypes.hpp>

#include "platform/call_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/button_view.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/pad_sticks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
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

// The action-down dispatcher (§9): updates the tables and, unless its alreadyHandled byte is set,
// hands the action to the UI handler stack and the menu core's delegate (menus, field prompts,
// battle commands). Its callers that can carry pad 2's presses and whose call fits one aligned
// block: the XInput main loop (every bound action) and two DirectInput loops. The two XInput
// extra-key calls and two DirectInput single calls straddle a block and are left as the game's.
constexpr std::uintptr_t kActionDown = 0x1407C3DAC;
constexpr std::uint8_t   kActionDownBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                               0x41, 0x56, 0x41, 0x57, 0xB8, 0x00, 0x01, 0x00, 0x00, 0xE8, 0x6A, 0xB1};
constexpr std::array<std::uintptr_t, 3> kActionDownCalls{0x140E745A0, 0x140E74141, 0x140E74312};

/// (owner, unused, source bit, the action event, alreadyHandled): 1 when the UI stack consumed it.
using ActionDownFn = char (*)(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled);

// A TSet<element> in the input object, stock TSparseArray layout (§1.1): data, ArrayNum, the
// allocation bits (4 inline dwords, a heap pointer, NumBits, MaxBits).
struct TableFields
{
    std::uint32_t  offset;
    const wchar_t* name;
};
constexpr std::array<TableFields, 2> kTables{{{0xF8, L"raw"}, {0x148, L"UI"}}};
constexpr std::uint32_t kTableNum        = 0x08;
constexpr std::uint32_t kTableInlineBits = 0x10;
constexpr std::uint32_t kTableHeapBits   = 0x20;
constexpr std::uint32_t kTableMaxBits    = 0x2C;
constexpr std::uint32_t kInlineBitCount  = 128;
constexpr std::int32_t  kMaxElements     = 1024;
constexpr unsigned      kLinesPerStatus  = 120;

struct Element  // 40 bytes
{
    std::uint64_t name;
    std::uint8_t  unread[0x10];
    std::uint8_t  previous;  // +0x18
    std::uint8_t  held;      // +0x19
    std::uint8_t  repeat;
    std::uint8_t  latch;
    std::uint32_t sources;  // +0x1C
    std::uint8_t  hash[8];
};
static_assert(sizeof(Element) == 40 && offsetof(Element, previous) == 0x18 && offsetof(Element, held) == 0x19 &&
              offsetof(Element, sources) == 0x1C);

struct HeldBytes
{
    std::uint8_t previous = 0;
    std::uint8_t held     = 0;
};

/// One table as read this frame, each element's per-player views, and reused scratch space.
struct Table
{
    std::byte*                      data  = nullptr;  // as read at this frame's Tick; checked again before writes
    std::int32_t                    count = 0;
    std::vector<Element>            elements;
    std::vector<char>               allocated;
    std::vector<button_view::Views> views;
    std::vector<std::uint64_t>      names;  // the key each view belongs to
    std::vector<std::uint8_t>       loggedHeld;
    std::vector<std::uint32_t>      loggedSources;
    std::vector<std::uint32_t>      words;  // scratch: the allocation bits
    std::vector<HeldBytes>          ones;   // scratch: player 1's bytes (restored after a ButtonScope)
    std::vector<HeldBytes>          twos;   // scratch: player 2's bytes
};

// Set once at install.
std::array<platform::CallRedirect, kActionDownCalls.size()> g_actionDownCalls;
ActionDownFn            g_actionDown = nullptr;
platform::VTableEntry   g_tickSlot;
TickFn                  g_engineTick = nullptr;
const std::byte* const* g_inputOwner = nullptr;

// Game thread (the Tick and player 2's calls run on it).
std::array<Table, kTables.size()> g_tables;
bool     g_active      = false;  // the views are current (per-pad input on, player 2 in)
bool     g_scopeOpen   = false;
unsigned g_framesSplit = 0;
unsigned g_scopes      = 0;
unsigned g_scopeSkips  = 0;  // a table moved between the Tick and player 2's call
unsigned g_writeFaults = 0;
unsigned g_lines       = 0;
unsigned g_dropped     = 0;
unsigned g_padTwoDowns = 0;  // pad 2's action-downs kept from the UI and the menu core
std::atomic<unsigned> g_mask{kPartViews | kPartEvents};  // SetMask

bool Acts(unsigned part) noexcept
{
    return (g_mask.load(std::memory_order_relaxed) & part) != 0;
}

bool ViewsWanted() noexcept
{
    return Acts(kPartViews) && pad_sticks::Enabled() && generation::Active() && generation::OnGameThread();
}

/// Writes previous/held into each element whose `write` flag is set.
bool WriteHeld(std::byte* data, const HeldBytes* bytes, const char* write, std::size_t count) noexcept
{
    __try
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (write[i] != 0)
            {
                data[i * sizeof(Element) + offsetof(Element, previous)] = static_cast<std::byte>(bytes[i].previous);
                data[i * sizeof(Element) + offsetof(Element, held)]     = static_cast<std::byte>(bytes[i].held);
            }
        }
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Whether the table's storage is still where this frame's Tick read it (a key added since would
/// have moved it: writing through the old pointer would corrupt freed memory, not fault).
bool Fresh(const Table& table, std::uint32_t offset) noexcept
{
    const std::byte* owner = g_inputOwner != nullptr ? *g_inputOwner : nullptr;
    std::byte*       data  = nullptr;
    std::int32_t     count = 0;
    return owner != nullptr && table.data != nullptr && platform::CopyGuarded(&data, owner + offset, sizeof(data)) &&
           platform::CopyGuarded(&count, owner + offset + kTableNum, sizeof(count)) && data == table.data &&
           count == table.count;
}

/// Reads one table's elements and allocation bits. False if the table cannot be read.
bool Load(const std::byte* owner, Table& table, std::uint32_t offset)
{
    table.data               = nullptr;
    const std::byte* header  = owner + offset;
    std::byte*       data    = nullptr;
    std::int32_t     count   = 0;
    std::uint32_t    maxBits = 0;
    if (!platform::CopyGuarded(&data, header, sizeof(data)) ||
        !platform::CopyGuarded(&count, header + kTableNum, sizeof(count)) ||
        !platform::CopyGuarded(&maxBits, header + kTableMaxBits, sizeof(maxBits)) || data == nullptr || count <= 0 ||
        count > kMaxElements)
    {
        return false;
    }
    const std::byte* bits = header + kTableInlineBits;
    if (maxBits > kInlineBitCount && !platform::CopyGuarded(&bits, header + kTableHeapBits, sizeof(bits)))
    {
        return false;
    }
    const auto size = static_cast<std::size_t>(count);
    table.words.resize((size + 31) / 32);
    table.elements.resize(size);
    if (bits == nullptr || !platform::CopyGuarded(table.words.data(), bits, table.words.size() * sizeof(std::uint32_t)) ||
        !platform::CopyGuarded(table.elements.data(), data, size * sizeof(Element)))
    {
        return false;
    }
    table.data  = data;
    table.count = count;
    table.allocated.resize(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        table.allocated[i] = static_cast<char>((table.words[i / 32] >> (i % 32)) & 1);
    }
    table.views.resize(size);
    table.names.resize(size, 0);
    table.loggedHeld.resize(size, 0);
    table.loggedSources.resize(size, 0);
    table.ones.resize(size);
    table.twos.resize(size);
    return true;
}

void LogChange(const wchar_t* table, const Element& element, std::uint8_t wasHeld, std::uint32_t wasSources,
               const button_view::Views& views)
{
    if (g_lines >= kLinesPerStatus)
    {
        ++g_dropped;
        return;
    }
    ++g_lines;
    platform::Log(L"buttons ({}): {} held {}->{} sources 0x{:08X}->0x{:08X}; player 1 sees {}, player 2 sees {}", table,
                  RC::Unreal::FName(static_cast<std::int64_t>(element.name)).ToString(), wasHeld, views.gameHeld, wasSources,
                  element.sources, views.now[0] ? 1 : 0, views.now[1] ? 1 : 0);
}

/// Fills `bytes` with one player's view of each element.
void ViewBytes(const Table& table, int player, std::vector<HeldBytes>& bytes) noexcept
{
    for (std::size_t i = 0; i < bytes.size() && i < table.views.size(); ++i)
    {
        bytes[i] = {static_cast<std::uint8_t>(table.views[i].previous[player]),
                    static_cast<std::uint8_t>(table.views[i].now[player])};
    }
}

/// Once per frame after the game's Tick: advances each view and writes player 1's.
void SplitTable(const std::byte* owner, std::size_t which)
{
    Table& table = g_tables[which];
    if (!Load(owner, table, kTables[which].offset))
    {
        return;
    }
    for (std::size_t i = 0; i < table.elements.size(); ++i)
    {
        const Element& element = table.elements[i];
        if (table.allocated[i] == 0)
        {
            continue;
        }
        if (table.names[i] != element.name)
        {
            table.names[i] = element.name;  // a new key in this slot: its views start afresh
            table.views[i] = {};
        }
        table.views[i].Advance(element.held, element.sources);
        if (table.views[i].gameHeld != table.loggedHeld[i] || element.sources != table.loggedSources[i])
        {
            LogChange(kTables[which].name, element, table.loggedHeld[i], table.loggedSources[i], table.views[i]);
            table.loggedHeld[i]    = table.views[i].gameHeld;
            table.loggedSources[i] = element.sources;
        }
    }
    ViewBytes(table, 0, table.ones);
    if (!WriteHeld(table.data, table.ones.data(), table.allocated.data(), table.ones.size()))
    {
        ++g_writeFaults;
    }
}

/// Per-pad input off or player 2 out: gives each action back the game's own held state (no edge),
/// so a hold only pad 2 kept is not left looking released, then forgets the views.
void GiveBack() noexcept
{
    for (std::size_t which = 0; which < kTables.size(); ++which)
    {
        Table& table = g_tables[which];
        if (!Fresh(table, kTables[which].offset))
        {
            continue;
        }
        for (std::size_t i = 0; i < table.ones.size() && i < table.views.size(); ++i)
        {
            table.ones[i] = {table.views[i].gameHeld, table.views[i].gameHeld};
        }
        if (!WriteHeld(table.data, table.ones.data(), table.allocated.data(), table.ones.size()))
        {
            ++g_writeFaults;
        }
    }
    g_tables = {};
}

void Tick(void* self, float deltaTime, void* application, void* cursor)
{
    g_engineTick(self, deltaTime, application, cursor);
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::InputTick);
    if (!ViewsWanted() || g_inputOwner == nullptr)
    {
        if (g_active && generation::OnGameThread())
        {
            g_active = false;  // the game's merge from here on
            GiveBack();
        }
        return;
    }
    try
    {
        const std::byte* owner = *g_inputOwner;
        if (owner == nullptr)
        {
            return;
        }
        for (std::size_t which = 0; which < kTables.size(); ++which)
        {
            SplitTable(owner, which);
        }
        g_active = true;
        ++g_framesSplit;
    }
    catch (...)
    {
        // An allocation failed (the log or a vector): this frame keeps the game's merge.
    }
}

/// Pad 2's presses update the tables (player 2's polling reads them) but are passed as already
/// handled: the game's own flag for "an earlier binding consumed this", so they never open a menu or
/// run a field prompt, which belong to player 1's context.
char ActionDown(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled)
{
    const bool padTwo = ((button_view::PadBits(1) >> (sourceBit & 31)) & 1) != 0;
    if (padTwo && alreadyHandled == 0 && Acts(kPartEvents) && pad_sticks::Enabled() && generation::Active() &&
        generation::OnGameThread())
    {
        ++g_padTwoDowns;
        alreadyHandled = 1;
    }
    return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
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
    if (!platform::ReplaceSlot(image, kInputTick, reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot))
    {
        return false;
    }
    const auto target = image.Rebase(kActionDown);
    unsigned   calls  = 0;
    if (target && image.BytesMatch(kActionDown, kActionDownBytes))
    {
        g_actionDown = reinterpret_cast<ActionDownFn>(*target);
        for (std::size_t i = 0; i < kActionDownCalls.size(); ++i)
        {
            const auto site = image.Rebase(kActionDownCalls[i], 5);
            calls += site && g_actionDownCalls[i].Install(*site, *target, reinterpret_cast<std::uintptr_t>(&ActionDown)) ? 1 : 0;
        }
    }
    platform::Log(L"buttons: pad 2's presses kept from menus and prompts at {} of {} dispatch calls", calls,
                  kActionDownCalls.size());
    return true;
}

void Remove() noexcept
{
    if (!g_tickSlot.Restore())
    {
        platform::Log(L"buttons: the input Tick slot was changed after us; left in place (it passes the call through)");
    }
    for (auto& call : g_actionDownCalls)
    {
        if (call.IsInstalled() && !call.Restore())
        {
            platform::Log(L"buttons: a dispatch call was changed after us; left in place (it passes pad 1 through)");
        }
    }
}

ButtonScope::ButtonScope() noexcept
{
    if (g_scopeOpen || !g_active || !ViewsWanted())
    {
        return;
    }
    for (std::size_t which = 0; which < kTables.size(); ++which)
    {
        if (!Fresh(g_tables[which], kTables[which].offset))
        {
            ++g_scopeSkips;  // a table moved since the Tick: player 2's call keeps player 1's view
            return;
        }
    }
    for (Table& table : g_tables)
    {
        ViewBytes(table, 0, table.ones);  // what the Tick wrote: player 1's view
        ViewBytes(table, 1, table.twos);
        if (!WriteHeld(table.data, table.twos.data(), table.allocated.data(), table.twos.size()))
        {
            ++g_writeFaults;
        }
    }
    g_scopeOpen = true;
    held_       = true;
    ++g_scopes;
}

ButtonScope::~ButtonScope()
{
    if (!held_)
    {
        return;
    }
    for (std::size_t which = 0; which < kTables.size(); ++which)
    {
        Table& table = g_tables[which];
        if (!Fresh(table, kTables[which].offset))
        {
            ++g_scopeSkips;  // moved during the call: the old storage is not written
            continue;
        }
        if (!WriteHeld(table.data, table.ones.data(), table.allocated.data(), table.ones.size()))
        {
            ++g_writeFaults;
        }
    }
    g_scopeOpen = false;
}

void SetMask(unsigned mask) noexcept
{
    g_mask.store(mask, std::memory_order_relaxed);
    platform::Log(L"buttons: mask {} (split views {}, pad 2 kept from menus and prompts {})", mask, Acts(kPartViews),
                  Acts(kPartEvents));
}

void LogStatus() noexcept
{
    platform::Log(L"  buttons: split {} frame(s), player 2's view held {} time(s) ({} skipped: a table moved), pad 2's "
                  L"presses kept from the UI {}, {} write fault(s); {} change line(s) logged, {} dropped; tables raw {} / "
                  L"UI {} elements",
                  g_framesSplit, g_scopes, g_scopeSkips, g_padTwoDowns, g_writeFaults, g_lines, g_dropped,
                  g_tables[0].elements.size(), g_tables[1].elements.size());
    g_framesSplit = g_scopes = g_scopeSkips = g_writeFaults = g_lines = g_dropped = g_padTwoDowns = 0;
}

}  // namespace lss::player_two::pad_buttons