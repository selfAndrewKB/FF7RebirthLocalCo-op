#include "player_two/input/pad_buttons.h"

#include "facts/controller.h"
#include "platform/dev_switch.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/call_site.h"
#include "player_two/generation.h"
#include "player_two/input/button_view.h"
#include "player_two/input/pad_sticks.h"
#include "players/seats.h"
#include "players/unreal_access.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
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
// block: the XInput main loop (every bound action) and two DirectInput loops.
constexpr std::uint8_t   kActionDownBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                               0x41, 0x56, 0x41, 0x57, 0xB8, 0x00, 0x01, 0x00, 0x00, 0xE8, 0x6A, 0xB1};
constexpr platform::FunctionFact kActionDown{L"SquareInput's action-down dispatcher", 0x1407C3DAC, kActionDownBytes};
constexpr std::array<std::uintptr_t, 3> kActionDownCalls{0x140E745A0, 0x140E74141, 0x140E74312};
// 1.005 (RE-1005-field-2.md §8): the DirectInput key list's call is at 0x140E93C3D, %16 = 13, so it crosses a 16-byte
// block. It is in KeyDown_MapKeyToActions too, so on 1.005 a dispatch call that crosses a block is changed with the
// extra-key calls, from the game thread in the input Tick after the game's (outside that function).

// The XInput loop's two extra-key calls (§9.2): per-button extra keys (0x140E7468D) and the extra-key
// list (0x140E7477E, SquareInput_CollectShortcutSecondKeys: the Shortcut1..5_Second keys a face button
// sends while the shortcut modifier is held; RE-p2-command-menu-input.md §3.1). Their alreadyHandled
// is the loop's result so far. Both calls cross a 16-byte block, so they are changed only from the
// game thread, in the input Tick after the game's (outside KeyDown_MapKeyToActions, the only function
// that runs them, which runs on the game thread: from Slate's key-down, the Tick and the analog
// handler [ASSUMPTION from §9.2]). The DirectInput-only single calls (0x140E741CE, 0x140E7424F) stay
// the game's.
constexpr std::array<std::uintptr_t, 2> kExtraKeyCalls{0x140E7468D, 0x140E7477E};
constexpr unsigned                      kExtraKeyLines = 40;
constexpr std::size_t                   kActionEventKey = 0x18;  // the action event's key (FName)

// ActionDownFn: pad_buttons.h.

// A TSet<element> in the input object, stock TSparseArray layout (§1.1): data, ArrayNum, the
// allocation bits (4 inline dwords, a heap pointer, NumBits, MaxBits).
struct TableFields
{
    std::uint32_t  offset;
    const wchar_t* name;
};
constexpr std::array<TableFields, 2> kTables{{{0xF8, L"raw"}, {0x148, L"UI"}}};
#if LSS_DEV
// The chocobo's Accel (R2) and Dash actions (RE-p2-chocobo-input.md §2.4): FName globals, traced frame by frame (dev
// tools).
constexpr std::uintptr_t kAccelKeyName = 0x1491E4550;
constexpr std::uintptr_t kDashKeyName  = 0x1491E4580;
#endif

constexpr std::uint32_t kTableNum        = 0x08;
constexpr std::uint32_t kTableInlineBits = 0x10;
constexpr std::uint32_t kTableHeapBits   = 0x20;
constexpr std::uint32_t kTableMaxBits    = 0x2C;
constexpr std::uint32_t kInlineBitCount  = 128;
constexpr std::int32_t  kMaxElements     = 1024;

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
    std::vector<HeldBytes>          twos;   // scratch: the open ButtonScope's seat's bytes
};

// Set once at install.
std::array<platform::CallRedirect, kActionDownCalls.size()> g_actionDownCalls;
std::array<platform::CallRedirect, kExtraKeyCalls.size()>   g_extraKeyCalls;   // installed by the first Tick
std::array<std::uintptr_t, kExtraKeyCalls.size()>           g_extraKeySites{};  // live addresses, 0 if not verified
std::array<std::uintptr_t, kActionDownCalls.size()>         g_lateActionDownSites{};  // 1.005: crossing; by the first Tick
bool                    g_extraKeysTried = false;  // game thread
unsigned                g_extraKeyLines  = 0;      // game thread
ActionDownFn            g_actionDown = nullptr;
PlayerTwoRouteFn        g_playerTwoRoute = nullptr;  // game thread
platform::VTableEntry   g_tickSlot;
TickFn                  g_engineTick = nullptr;
const std::byte* const* g_inputOwner = nullptr;

// Game thread (the Tick and the seats' calls run on it).
using PerSeat = std::array<unsigned, players::kMaxPlayers>;  // by seat; index 0 (player 1) unused
std::array<Table, kTables.size()> g_tables;
bool     g_active      = false;  // the views are current (per-pad input on, a seat's player in)
int      g_scopeSeat   = 0;      // the seat whose ButtonScope is open, 0 for none
unsigned g_framesSplit = 0;
PerSeat  g_scopes{};
PerSeat  g_scopeSkips{};  // a table moved between the Tick and the seat's call
unsigned g_writeFaults = 0;
unsigned g_lines       = 0;
PerSeat  g_seatDowns{};    // a seated pad's action-downs kept from the UI and the menu core
unsigned g_padOneDowns = 0;  // pad 1's action-downs kept from the UI while a seat holds the menus
int      g_menuPlayer  = 0;  // whose view the tables show outside the seats' calls, and whose presses reach the UI:
                             // 0 player 1, or the seat whose shop is up (SetMenuPlayer)
std::atomic<unsigned> g_mask{kPartViews | kPartEvents};  // SetMask

/// What each built seat sees, for a change or trace line: ", player 2 sees 1" per seat.
std::wstring SeatViews(const button_view::Views& views)
{
    std::wstring text;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        text += std::format(L", {} sees {}", players::SeatName(seat), views.now[seat] ? 1 : 0);
    }
    return text;
}

#if LSS_DEV
const std::uint64_t*  g_traceKeys[2] = {};  // Accel, Dash (live addresses of the FName globals)
std::array<std::array<std::uint64_t, 2>, 2> g_traceLast{};  // per table, per key: the last traced bytes

/// The FName in a traced key's global, or 0.
std::uint64_t TraceKey(std::size_t i) noexcept
{
    std::uint64_t key = 0;
    return g_traceKeys[i] != nullptr && platform::CopyGuarded(&key, g_traceKeys[i], sizeof(key)) ? key : 0;
}

/// Accel and Dash: every change of the bytes the game left this frame and of the views, uncapped by the status lines.
void Trace(std::size_t which, const Element& element, const button_view::Views& views)
{
    if (!platform::Enabled(platform::Level::Trace))
    {
        return;
    }
    for (std::size_t k = 0; k < 2; ++k)
    {
        const std::uint64_t key = TraceKey(k);
        if (key == 0 || element.name != key)
        {
            continue;
        }
        std::uint64_t packed = std::uint64_t{element.previous} | std::uint64_t{element.held} << 8 |
                               std::uint64_t{element.repeat} << 16 | std::uint64_t{element.latch} << 24 |
                               std::uint64_t{element.sources & 0x00FFFFFFu} << 32 | std::uint64_t{views.gameHeld} << 56 |
                               std::uint64_t{views.now[0]} << 60 | 1ull << 63;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            packed |= std::uint64_t{views.now[seat]} << (60 + seat);  // the seats the line prints (SeatViews), bits 61-62
        }
        if (packed == g_traceLast[which][k])
        {
            return;
        }
        g_traceLast[which][k] = packed;
        platform::LogTrace(L"buttons trace ({}): {} as the game left it: previous {} held {} repeat {} latch {} sources 0x{:08X}; "
                      L"game held {}; player 1 sees {}{}",
                      kTables[which].name, k == 0 ? L"Accel" : L"Dash", element.previous, element.held, element.repeat,
                      element.latch, element.sources, views.gameHeld, views.now[0] ? 1 : 0, SeatViews(views));
        return;
    }
}

/// The two FName globals the trace compares keys with.
void FindTraceKeys(const platform::GameImage& image) noexcept
{
    const auto accel = image.Rebase(kAccelKeyName, sizeof(std::uint64_t));
    const auto dash  = image.Rebase(kDashKeyName, sizeof(std::uint64_t));
    g_traceKeys[0]   = accel ? reinterpret_cast<const std::uint64_t*>(*accel) : nullptr;
    g_traceKeys[1]   = dash ? reinterpret_cast<const std::uint64_t*>(*dash) : nullptr;
}
#else
void Trace(std::size_t, const Element&, const button_view::Views&) {}
void FindTraceKeys(const platform::GameImage&) noexcept {}
#endif

bool Acts(unsigned part) noexcept
{
    return (g_mask.load(std::memory_order_relaxed) & part) != 0;
}

bool ViewsWanted() noexcept
{
    return Acts(kPartViews) && pad_sticks::Enabled() && generation::AnyActive() && generation::OnGameThread();
}

/// The built seats whose player is in now (button_view::SeatBit each): their pads are their own, the rest player 1's.
unsigned SeatsIn() noexcept
{
    unsigned seats = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (generation::Active(seat))
        {
            seats |= button_view::SeatBit(seat);
        }
    }
    return seats;
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
    if (!platform::Enabled(platform::Level::Trace))
    {
        return;
    }
    ++g_lines;
    platform::LogTrace(L"buttons ({}): {} held {}->{} sources 0x{:08X}->0x{:08X}; player 1 sees {}{}", table,
                  players::unreal::NameText(element.name), wasHeld, views.gameHeld, wasSources,
                  element.sources, views.now[0] ? 1 : 0, SeatViews(views));
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

/// Once per frame after the game's Tick: advances each view (with the seats in `seats`) and writes the menu player's
/// (player 1's, or a seat's while its player's shop is up).
void SplitTable(const std::byte* owner, std::size_t which, unsigned seats)
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
        table.views[i].Advance(element.held, element.sources, seats);
        Trace(which, element, table.views[i]);
        if (table.views[i].gameHeld != table.loggedHeld[i] || element.sources != table.loggedSources[i])
        {
            LogChange(kTables[which].name, element, table.loggedHeld[i], table.loggedSources[i], table.views[i]);
            table.loggedHeld[i]    = table.views[i].gameHeld;
            table.loggedSources[i] = element.sources;
        }
    }
    ViewBytes(table, g_menuPlayer, table.ones);
    if (!WriteHeld(table.data, table.ones.data(), table.allocated.data(), table.ones.size()))
    {
        ++g_writeFaults;
    }
}

/// Per-pad input off or every seat's player out: gives each action back the game's own held state (no
/// edge), so a hold only a seat's pad kept is not left looking released, then forgets the views.
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
    g_tables     = {};
    g_menuPlayer = 0;  // the game's merge: the one pad again
}

void InstallExtraKeyCalls() noexcept;

void Tick(void* self, float deltaTime, void* application, void* cursor)
{
    g_engineTick(self, deltaTime, application, cursor);
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::InputTick);
    if (!g_extraKeysTried && g_actionDown != nullptr && generation::OnGameThread())
    {
        InstallExtraKeyCalls();
    }
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
        const unsigned seats = SeatsIn();
        for (std::size_t which = 0; which < kTables.size(); ++which)
        {
            SplitTable(owner, which, seats);
        }
        g_active = true;
        ++g_framesSplit;
    }
    catch (...)
    {
        // An allocation failed (the log or a vector): this frame keeps the game's merge.
    }
}

// The route to a seat's own UI is player 2's (pad_buttons.h, PlayerTwoRouteFn): another seat's presses skip it.
static_assert(players::kSeatsBuilt == 1, "the menu route is player 2's: before seat 2 is built, menu_input_route takes "
                                         "the seat (by the press's pad) and ActionDown offers it every seat's press");

/// A seated pad's presses update the tables (its player's polling reads them) but are passed as already
/// handled: the game's own flag for "an earlier binding consumed this", so they never open a menu or
/// run a field prompt, which belong to player 1's context. The seat comes from the press's source bit.
char ActionDown(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled)
{
    const bool split = alreadyHandled == 0 && Acts(kPartEvents) && pad_sticks::Enabled() && generation::AnyActive() &&
                       generation::OnGameThread();
    if (!split)
    {
        return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
    }
    const int seat = button_view::SeatOfSource(sourceBit, SeatsIn());
    if (g_menuPlayer != 0)
    {
        // A seat's shop is up: its pad is the menu pad, as the one pad is in single player; pad 1's presses and any
        // other seat's update the tables but are passed as handled.
        if (((button_view::PadBits(0) >> (sourceBit & 31)) & 1) != 0)
        {
            ++g_padOneDowns;
            alreadyHandled = 1;
        }
        else if (seat != 0 && seat != g_menuPlayer)
        {
            alreadyHandled = 1;
        }
        return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
    }
    if (seat != 0)
    {
        ++g_seatDowns[seat];
        char result = 0;
        if (seat == players::kPlayerTwoSeat && g_playerTwoRoute != nullptr &&
            g_playerTwoRoute(g_actionDown, owner, unused, sourceBit, event, alreadyHandled, result))
        {
            return result;  // player 2's own UI took it
        }
        alreadyHandled = 1;
    }
    return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
}

/// The extra-key calls: the same as the main loop's (ActionDown), with each key logged by name (the
/// first few, both pads), since which keys come this way is not otherwise known.
char ExtraKeyDown(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled)
{
    // The call sites were changed on the understanding that only the game thread runs them; a call from
    // another thread would mean a swap could have torn them (call_redirect.h).
    static std::atomic<unsigned> offThread{0};
    if (!generation::OnGameThread() && offThread.fetch_add(1, std::memory_order_relaxed) < 5)
    {
        platform::Log(L"WARNING: an extra-key call ran off the game thread; its call site was changed on the understanding "
                      L"that only the game thread runs it");
    }
    const char result = ActionDown(owner, unused, sourceBit, event, alreadyHandled);
    if (platform::Enabled(platform::Level::Trace) && g_extraKeyLines < kExtraKeyLines && event != nullptr)
    {
        ++g_extraKeyLines;
        std::uint64_t key = 0;
        platform::CopyGuarded(&key, static_cast<const std::byte*>(event) + kActionEventKey, sizeof(key));
        unsigned built = 0;  // every built seat's pad, in or not (named as the line always named pad 2)
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            built |= button_view::SeatBit(seat);
        }
        const int seat = button_view::SeatOfSource(sourceBit, built);
        platform::LogTrace(L"buttons: extra key {} from {} (source bit {}, already handled {}) -> {}",
                      players::unreal::NameText(key),
                      seat != 0 ? std::format(L"pad {}", seat + 1) : std::wstring(L"pad 1 or another device"),
                      sourceBit, alreadyHandled, static_cast<int>(result));
    }
    return result;
}

/// 1.005: a dispatch call that crosses a block, changed from the game thread: ActionDown, with the same warning as the
/// extra-key calls should another thread ever run it.
char ActionDownOwnThread(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled)
{
    static std::atomic<unsigned> offThread{0};
    if (!generation::OnGameThread() && offThread.fetch_add(1, std::memory_order_relaxed) < 5)
    {
        platform::Log(L"WARNING: a dispatch call ran off the game thread; its call site was changed on the understanding "
                      L"that only the game thread runs it");
    }
    return ActionDown(owner, unused, sourceBit, event, alreadyHandled);
}

/// Once, from the input Tick after the game's (game thread, outside the only function that runs
/// them): the two extra-key calls, and on 1.005 the dispatch calls that cross a block.
void InstallExtraKeyCalls() noexcept
{
    g_extraKeysTried = true;
    unsigned late = 0, lateCalls = 0;
    for (std::size_t i = 0; i < g_lateActionDownSites.size(); ++i)
    {
        if (g_lateActionDownSites[i] == 0)
        {
            continue;
        }
        ++late;
        lateCalls += g_actionDownCalls[i].InstallFromOwnThread(g_lateActionDownSites[i],
                                                               reinterpret_cast<std::uintptr_t>(g_actionDown),
                                                               reinterpret_cast<std::uintptr_t>(&ActionDownOwnThread))
                         ? 1
                         : 0;
    }
    if (late != 0)
    {
        platform::Log(L"buttons: pad 2's presses kept from menus and prompts at {} of {} more dispatch call(s) (crossing a "
                      L"block; changed from the game thread)",
                      lateCalls, late);
    }
    unsigned calls   = 0;
    for (std::size_t i = 0; i < g_extraKeyCalls.size(); ++i)
    {
        calls += g_extraKeySites[i] != 0 &&
                         g_extraKeyCalls[i].InstallFromOwnThread(g_extraKeySites[i], reinterpret_cast<std::uintptr_t>(g_actionDown),
                                                                 reinterpret_cast<std::uintptr_t>(&ExtraKeyDown))
                     ? 1
                     : 0;
    }
    platform::Log(L"buttons: pad 2's extra keys (the shortcut keys) take the same route at {} of {} extra-key calls",
                  calls, g_extraKeyCalls.size());
}

}  // namespace

void SetMenuPlayer(int seat) noexcept
{
    const int menu = players::SeatBuilt(seat) ? seat : 0;
    if (menu == g_menuPlayer)
    {
        return;
    }
    g_menuPlayer = menu;
    if (menu != 0)
    {
        platform::Log(L"buttons: pad {} drives the menus now ({}'s view in the action tables, pad 1's presses kept from "
                      L"the UI)",
                      menu + 1, players::SeatName(menu));
    }
    else
    {
        platform::Log(L"buttons: pad 1 drives the menus again ({} of pad 1's presses were kept from the UI meanwhile)",
                      g_padOneDowns);
    }
    g_padOneDowns = 0;
}

void SetPlayerTwoRoute(PlayerTwoRouteFn route) noexcept
{
    g_playerTwoRoute = route;
}

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
    FindTraceKeys(image);
    if (!platform::ReplaceSlot(image, kInputTick, reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot))
    {
        return false;
    }
    const auto target = platform::FindFunction(image, kActionDown);
    unsigned   calls  = 0;
    if (target)
    {
        g_actionDown = reinterpret_cast<ActionDownFn>(*target);
        for (std::size_t i = 0; i < kActionDownCalls.size(); ++i)
        {
            const auto site = image.Rebase(kActionDownCalls[i], 5);
            if (site && platform::RunningBuild() == platform::BuildIndex::V1005 && !call_site::FitsOneBlock(*site))
            {
                g_lateActionDownSites[i] = *site;  // changed by the first Tick (InstallExtraKeyCalls)
                continue;
            }
            calls += site && g_actionDownCalls[i].Install(*site, *target, reinterpret_cast<std::uintptr_t>(&ActionDown)) ? 1 : 0;
        }
        for (std::size_t i = 0; i < kExtraKeyCalls.size(); ++i)
        {
            const auto site     = image.Rebase(kExtraKeyCalls[i], 5);
            g_extraKeySites[i] = site ? *site : 0;
        }
    }
    const auto late = std::count_if(g_lateActionDownSites.begin(), g_lateActionDownSites.end(),
                                    [](std::uintptr_t site) { return site != 0; });
    platform::Log(L"buttons: pad 2's presses kept from menus and prompts at {} of {} dispatch calls{}", calls,
                  kActionDownCalls.size(),
                  late != 0 ? std::format(L" ({} more from the first input tick: crossing a block)", late) : std::wstring());
    return true;
}

void Remove() noexcept
{
    g_playerTwoRoute = nullptr;  // no route outlives the buttons' redirects
    if (!g_tickSlot.Restore())
    {
        platform::Log(L"buttons: the input Tick slot was changed after us; left in place (it passes the call through)");
    }
    // The extra-key calls (and on 1.005 a dispatch call that crosses a block) are restored only from the game thread
    // (call_redirect.h). Off it, they stay ours; the DLL is pinned, and with player 2 out they pass every press through.
    const bool gameThread = generation::OnGameThread();
    for (std::size_t i = 0; i < g_actionDownCalls.size(); ++i)
    {
        auto&      call = g_actionDownCalls[i];
        const bool late = g_lateActionDownSites[i] != 0;
        if (call.IsInstalled() && late && !gameThread)
        {
            platform::Log(L"buttons: a dispatch call left in place (removed off the game thread); it passes pad 1 through");
        }
        else if (call.IsInstalled() && !call.Restore())
        {
            platform::Log(L"buttons: a dispatch call was changed after us; left in place (it passes pad 1 through)");
        }
    }
    for (auto& call : g_extraKeyCalls)
    {
        if (call.IsInstalled() && (!gameThread || !call.Restore()))
        {
            platform::Log(L"buttons: an extra-key call left in place ({}); it passes presses through with player 2 out",
                          gameThread ? L"changed after us" : L"removed off the game thread");
        }
    }
}

ButtonScope::ButtonScope(int seat, std::uint64_t withoutKey) noexcept
{
    if (g_scopeSeat != 0 || !g_active || !players::SeatBuilt(seat) || !ViewsWanted() || !generation::Active(seat))
    {
        return;  // another seat's scope is open (seats take turns), or the seat's player is not in
    }
    for (std::size_t which = 0; which < kTables.size(); ++which)
    {
        if (!Fresh(g_tables[which], kTables[which].offset))
        {
            ++g_scopeSkips[seat];  // a table moved since the Tick: the seat's call keeps player 1's view
            return;
        }
    }
    for (Table& table : g_tables)
    {
        ViewBytes(table, g_menuPlayer, table.ones);  // what the Tick wrote: the menu player's view
        ViewBytes(table, seat, table.twos);
        for (std::size_t i = 0; withoutKey != 0 && i < table.twos.size() && i < table.names.size(); ++i)
        {
            if (table.names[i] == withoutKey)
            {
                table.twos[i] = {0, 0};  // not held, no edge
            }
        }
        if (!WriteHeld(table.data, table.twos.data(), table.allocated.data(), table.twos.size()))
        {
            ++g_writeFaults;
        }
    }
    g_scopeSeat = seat;
    held_       = true;
    ++g_scopes[seat];
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
            ++g_scopeSkips[g_scopeSeat];  // moved during the call: the old storage is not written
            continue;
        }
        if (!WriteHeld(table.data, table.ones.data(), table.allocated.data(), table.ones.size()))
        {
            ++g_writeFaults;
        }
    }
    g_scopeSeat = 0;
}

void SetMask(unsigned mask) noexcept
{
    g_mask.store(mask, std::memory_order_relaxed);
    platform::Log(L"buttons: mask {} (split views {}, pad 2 kept from menus and prompts {})", mask, Acts(kPartViews),
                  Acts(kPartEvents));
}

unsigned Mask() noexcept
{
    return g_mask.load(std::memory_order_relaxed);
}

void LogStatus() noexcept
{
    try
    {
        std::wstring seats;  // per built seat, as the line named player 2 alone
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            seats += std::format(L"{}'s view held {} time(s) ({} skipped: a table moved), pad {}'s presses kept from the "
                                 L"UI {}, ",
                                 players::SeatName(seat), g_scopes[seat], g_scopeSkips[seat], seat + 1, g_seatDowns[seat]);
        }
        platform::Log(L"  buttons: split {} frame(s), {}{} write fault(s); {} change line(s) logged; tables raw {} / "
                      L"UI {} elements",
                      g_framesSplit, seats, g_writeFaults, g_lines, g_tables[0].elements.size(),
                      g_tables[1].elements.size());
    }
    catch (...)
    {
        // Out of memory for the line: the counters still start afresh.
    }
    g_framesSplit = g_writeFaults = g_lines = 0;
    g_scopes = g_scopeSkips = g_seatDowns = PerSeat{};
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"buttons-mask",
                       .shape = control::Shape::Integer,
                       .max   = 3,
                       .help  = L"diagnosis: which parts of per-pad buttons act (1 the split views, 2 pad 2 kept from "
                                L"menus and prompts); both at start",
                       .set   = [](const Value& v) {
                           SetMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"buttons mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{Mask()}}; }});
}

#endif

}  // namespace lss::player_two::pad_buttons