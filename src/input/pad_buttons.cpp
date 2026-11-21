#include "input/pad_buttons.h"

#include "facts/controller.h"
#include "platform/dev_switch.h"

#include "input/button_view.h"
#include "input/pad_sticks.h"
#include "input/pad_table.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/call_site.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "views/whole_screen.h"

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
#include <utility>
#include <vector>

namespace lss::input::pad_buttons {

namespace {

// The input processor's Tick (research/RE-p2-per-pad-buttons.md): the IInputProcessor vtable's slot 1.
// (this, float DeltaTime, FSlateApplication&, TSharedRef<ICursor> by address.)
constexpr std::uint8_t kInputTickBytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
                                            0x57, 0x48, 0x8D, 0xAC, 0x24, 0x18, 0xFD, 0xFF, 0xFF, 0xB8, 0xE8, 0x03};
constexpr platform::SlotFact kInputTick{L"the Square input processor's Tick slot (1)", 0x145BA5CD0,
                                        {L"SquareInput__InputProcessorTick_AggregateDevices", 0x1409F60B4, kInputTickBytes}};

using TickFn = void (*)(void* self, float deltaTime, void* application, void* cursor);

// The action-down dispatcher: updates the tables and, unless its alreadyHandled byte is set,
// hands the action to the UI handler stack and the menu core's delegate (menus, field prompts,
// battle commands). Its callers that can carry a seat's presses: the XInput main loop (every bound action), two
// DirectInput loops, and the keyboard's and mouse's four (research/RE-kbm-square-input.md: the keyboard's binding loop,
// bits 17 and 18; its extra-key list, 17; the mouse buttons and the Tick's mouse-button repeat, 19), which are a seat's
// while the keyboard plays it.
constexpr std::uint8_t   kActionDownBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                               0x41, 0x56, 0x41, 0x57, 0xB8, 0x00, 0x01, 0x00, 0x00, 0xE8, 0xA6, 0x39};
constexpr platform::FunctionFact kActionDown{L"SquareInput's action-down dispatcher", 0x140DD81C0, kActionDownBytes};
constexpr std::array<std::uintptr_t, 7> kActionDownCalls{0x140E93EC3, 0x140E93A69, 0x140E93C3D, 0x140E943BF,
                                                         0x140E94565, 0x140BAF865, 0x1409F6746};
// The DirectInput key list's call sits at %16 = 13 and the keyboard loop's at 15 (research/RE-1005-field-2.md), so they
// cross a 16-byte block. Both are in KeyDown_MapKeyToActions, so a dispatch call that crosses a block is changed with the
// extra-key calls: from the game thread, in the input Tick once the game's Tick has returned, when no frame of that
// function is running.
constexpr std::size_t kFirstKeyboardCall = 3;  // kActionDownCalls from here on: the keyboard's and mouse's

// The XInput loop's two extra-key calls: the per-button extra keys and the extra-key list
// (SquareInput_CollectShortcutSecondKeys: the Shortcut1..5_Second keys a face button sends while the shortcut
// modifier is held; research/RE-p2-command-menu-input.md). Their alreadyHandled
// is the loop's result so far. Both calls cross a 16-byte block, so they are changed only from the
// game thread, in the input Tick after the game's (outside KeyDown_MapKeyToActions, the only function
// that runs them, which runs on the game thread: from Slate's key-down, the Tick and the analog
// handler [ASSUMPTION]). The DirectInput-only single calls stay
// the game's.
constexpr std::array<std::uintptr_t, 2> kExtraKeyCalls{0x140E93FAC, 0x140E940A7};
constexpr unsigned                      kExtraKeyLines = 40;
constexpr std::size_t                   kActionEventKey = 0x18;  // the action event's key (FName)

// ActionDownFn: pad_buttons.h.

// The menu-opening actions (research/RE-pause-menu-any-pad.md): the FKey globals PauseMenu, MapMenu and EnemyIntel
// (facts/controller.h), whose FNames the game's one pause poll reads pressed or held on the merged tables (and the
// menus' close handlers read on the UI route). Read once the game has filled them (guarded, from the Tick), since the
// globals are built by the game's own start-up, which may run after the mod installs.
constexpr std::array<std::uintptr_t, 3> kOpenerKeyNames{facts::kPauseMenuKey, facts::kMapMenuKey, facts::kEnemyIntelKey};
constexpr unsigned                      kOpenerReadFrames = 600;  // input ticks to wait for them before a warning

// A TSet<element> in the input object, stock TSparseArray layout: data, ArrayNum, the
// allocation bits (4 inline dwords, a heap pointer, NumBits, MaxBits).
struct TableFields
{
    std::uint32_t  offset;
    const wchar_t* name;
};
constexpr std::array<TableFields, 2> kTables{{{0xF8, L"raw"}, {0x148, L"UI"}}};
#if LSS_DEV
// The chocobo's Accel (R2) and Dash actions (research/RE-p2-chocobo-input.md): FName globals, traced frame by frame (dev
// tools).
constexpr std::uintptr_t kAccelKeyName = 0x14901C630;
constexpr std::uintptr_t kDashKeyName  = 0x14901C660;
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
SeatRouteFn             g_seatRoute = nullptr;  // game thread
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
PerSeat  g_openerDowns{};  // a seated pad's presses of a menu opener, passed to the game as they came
PerSeat  g_menuDowns{};    // a seated pad's presses while a menu owns input, passed to the game as they came
unsigned g_padOneDowns = 0;  // pad 1's action-downs kept from the UI while a seat holds the menus
int      g_menuPlayer  = 0;  // whose view the tables show outside the seats' calls, and whose presses reach the UI:
                             // 0 player 1, or the seat whose shop is up (SetMenuPlayer)
bool     g_menuTime     = false;  // the game's input is shared this frame: a pause-family menu owns it, or a minigame
bool     g_minigameTime = false;  // a minigame shares it (views::SharedPlay of the engine tick's reason)
std::array<const std::uint64_t*, kOpenerKeyNames.size()> g_openerKeys{};  // live addresses (set at install; null: none)
std::array<std::uint64_t, kOpenerKeyNames.size()>        g_openers{};     // the FNames, once read
bool     g_openersRead   = false;  // all three read non-zero
unsigned g_openerFrames  = 0;      // input ticks spent reading them
// L3 (TakeLeftThumbPress): the engine key's FKey global (live address; null: none), its FName once read, and each
// seat's press noted at the input tick until taken.
const std::uint64_t*                     g_leftThumbKey = nullptr;
std::uint64_t                            g_leftThumb    = 0;
unsigned                                 g_leftThumbFrames = 0;  // input ticks spent reading it
std::array<bool, players::kMaxPlayers>   g_leftThumbPressed{};
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
    return Acts(kPartViews) && pad_sticks::Enabled() && players::generation::AnyActive() && players::generation::OnGameThread();
}

/// The built seats whose player is in now (button_view::SeatBit each): their pads are their own, the rest player 1's.
unsigned SeatsIn() noexcept
{
    unsigned seats = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat))
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

/// Whether `key` names a menu-opening action (PauseMenu, MapMenu, EnemyIntel); false until they are read.
bool Opener(std::uint64_t key) noexcept
{
    return g_openersRead && key != 0 && (key == g_openers[0] || key == g_openers[1] || key == g_openers[2]);
}

/// Reads the three menu openers' FNames from the game's FKey globals until all three are non-zero. Logged once found,
/// a warning once if not after kOpenerReadFrames input ticks (a seat's pad's Start then opens nothing).
void ReadOpeners() noexcept
{
    // The L3 key the same way (the engine's own key, filled by its start-up): one guarded read a tick until set.
    if (std::uint64_t key = 0; g_leftThumb == 0 && g_leftThumbKey != nullptr &&
                               platform::CopyGuarded(&key, g_leftThumbKey, sizeof(key)) && key != 0)
    {
        g_leftThumb = key;
    }
    else if (g_leftThumb == 0 && g_leftThumbKey != nullptr && ++g_leftThumbFrames == kOpenerReadFrames)
    {
        platform::LogWarning(L"buttons: the L3 key's name was not read after {} input ticks; players 2 and 3 cannot ask "
                             L"to be brought back with L3 until it is",
                             kOpenerReadFrames);
    }
    if (g_openersRead || g_openerFrames > kOpenerReadFrames)
    {
        return;
    }
    ++g_openerFrames;
    unsigned found = 0;
    for (std::size_t i = 0; i < kOpenerKeyNames.size(); ++i)
    {
        std::uint64_t key = 0;
        if (g_openerKeys[i] != nullptr && platform::CopyGuarded(&key, g_openerKeys[i], sizeof(key)) && key != 0)
        {
            g_openers[i] = key;
            ++found;
        }
    }
    if (found == kOpenerKeyNames.size())
    {
        g_openersRead = true;
        platform::Log(L"buttons: every pad's Start, map and enemy-intel presses reach the game's pause poll and menus; "
                      L"while a menu owns the game's input every pad drives it (the game's own merged state in the "
                      L"tables, its presses passed as they came)");
    }
    else if (g_openerFrames > kOpenerReadFrames)
    {
        platform::LogWarning(L"buttons: the menu-opening actions' names were not read after {} input ticks ({} of {} set); "
                             L"pad 2's or 3's Start, map and enemy-intel presses stay its own player's (no pause from it)",
                             kOpenerReadFrames, found, kOpenerKeyNames.size());
    }
}

/// Whether every element shows the game's merged state this frame: a menu owns the game's input and no seat's shop is
/// up (a shop is not a pause-family menu and keeps its seat's pad, research/RE-pause-menu-any-pad.md; its view wins while it
/// is up, as its presses do in ActionDown).
bool MergedEverywhere() noexcept
{
    return g_menuTime && g_menuPlayer == 0;
}

/// Fills `bytes` with what the Tick writes into each element: the game's own merged state (its previous and held bytes
/// under its one-pad merge) for the three menu openers, and for every element while a menu owns the game's input
/// (research/RE-pause-menu-any-pad.md); otherwise the menu player's view (player 1's, or a seat's while its player's shop is
/// up).
void WrittenBytes(const Table& table, std::vector<HeldBytes>& bytes) noexcept
{
    const bool merged = MergedEverywhere();
    for (std::size_t i = 0; i < bytes.size() && i < table.views.size(); ++i)
    {
        const button_view::Views& views = table.views[i];
        if (merged || (i < table.names.size() && Opener(table.names[i])))
        {
            bytes[i] = {views.gamePrevious, views.gameHeld};
        }
        else
        {
            bytes[i] = {static_cast<std::uint8_t>(views.previous[g_menuPlayer]),
                        static_cast<std::uint8_t>(views.now[g_menuPlayer])};
        }
    }
}

/// Once per frame after the game's Tick: advances each view (with the seats in `seats`, the keyboard's `keyboardSeat`) and writes the menu player's
/// (player 1's, or a seat's while its player's shop is up), or the game's merged state where every pad is the game's
/// pad (WrittenBytes).
void SplitTable(const std::byte* owner, std::size_t which, unsigned seats, int keyboardSeat)
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
        table.views[i].Advance(element.held, element.sources, seats, keyboardSeat);
        if (which == 0 && g_leftThumb != 0 && element.name == g_leftThumb)
        {
            for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
            {
                if (table.views[i].now[seat] && !table.views[i].previous[seat])
                {
                    g_leftThumbPressed[static_cast<std::size_t>(seat)] = true;  // until TakeLeftThumbPress
                }
            }
        }
        Trace(which, element, table.views[i]);
        if (table.views[i].gameHeld != table.loggedHeld[i] || element.sources != table.loggedSources[i])
        {
            LogChange(kTables[which].name, element, table.loggedHeld[i], table.loggedSources[i], table.views[i]);
            table.loggedHeld[i]    = table.views[i].gameHeld;
            table.loggedSources[i] = element.sources;
        }
    }
    WrittenBytes(table, table.ones);
    if (!WriteHeld(table.data, table.ones.data(), table.allocated.data(), table.ones.size()))
    {
        ++g_writeFaults;
        return;
    }
    for (std::size_t i = 0; i < table.ones.size() && i < table.views.size(); ++i)
    {
        table.views[i].Wrote(table.ones[i].held);  // what the next Tick compares the game's byte with
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
    g_tables           = {};
    g_leftThumbPressed = {};
    g_menuPlayer       = 0;  // the game's merge: the one pad again
    g_menuTime         = false;
    g_minigameTime     = false;
}

/// Once per input tick while the views are written: whether the game's input is shared, from the game's own state
/// (views/whole_screen.h): a pause-family menu owns it, or a minigame holds the whole screen (the game plays it on
/// player 1's picture through its pad-blind pollers, as it treats a second pad unmodded; the engine tick's reason, a
/// frame before). Logged on change: a menu at Debug, a minigame's start and end at Info.
void ReadMenuTime() noexcept
{
    const bool menu     = views::MenuOwnsInput();
    const bool minigame = !menu && views::SharedPlay(views::LastReason());
    const bool menuTime = menu || minigame;
    if (menuTime == g_menuTime && minigame == g_minigameTime)
    {
        return;
    }
    const bool minigameWas = g_minigameTime;
    g_menuTime             = menuTime;
    g_minigameTime         = minigame;
    if (minigame && !minigameWas)
    {
        platform::Log(L"buttons: a minigame holds the screen: every pad drives it (the game's merged state in the tables, "
                      L"every pad's presses passed as they came, every pad's stick moving player 1's character and camera)");
    }
    else if (!minigame && minigameWas)
    {
        platform::Log(L"buttons: the minigame no longer holds the screen");
    }
    if (!platform::Enabled(platform::Level::Debug) || minigame)
    {
        return;
    }
    if (menuTime && g_menuPlayer != 0)
    {
        platform::LogDebug(L"buttons: a menu owns the game's input while pad {}'s shop is up: the shop keeps its pad (its "
                           L"view in the tables, its presses only)",
                           g_menuPlayer + 1);
    }
    else if (menuTime)
    {
        platform::LogDebug(L"buttons: a menu owns the game's input: every pad drives it (the game's merged state in the "
                           L"tables, every pad's presses passed as they came)");
    }
    else
    {
        platform::LogDebug(L"buttons: no menu owns the game's input: each pad its own player's again");
    }
}

void InstallExtraKeyCalls() noexcept;

void Tick(void* self, float deltaTime, void* application, void* cursor)
{
    if (players::generation::OnGameThread())
    {
        ReadOpeners();  // from the game's start, before any seat's first press
        if (ViewsWanted() && g_inputOwner != nullptr)
        {
            // Before the game's Tick: its device polling sends presses through ActionDown, which must see this
            // frame's menu time (the game state changes only in the world tick, so it holds for the write below).
            ReadMenuTime();
        }
    }
    g_engineTick(self, deltaTime, application, cursor);
    const platform::frame_watch::Charge charge(platform::frame_watch::Part::InputTick);
    if (!g_extraKeysTried && g_actionDown != nullptr && players::generation::OnGameThread())
    {
        InstallExtraKeyCalls();
    }
    if (!ViewsWanted() || g_inputOwner == nullptr)
    {
        if (g_active && players::generation::OnGameThread())
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
        const unsigned seats    = SeatsIn();
        const int      keyboard = pad_table::KeyboardSeat();
        for (std::size_t which = 0; which < kTables.size(); ++which)
        {
            SplitTable(owner, which, seats, keyboard);
        }
        g_active = true;
        ++g_framesSplit;
    }
    catch (...)
    {
        // An allocation failed (the log or a vector): this frame keeps the game's merge.
    }
}

/// A seated pad's presses update the tables (its player's polling reads them) but are passed as already
/// handled: the game's own flag for "an earlier binding consumed this", so they never run a field prompt,
/// which belongs to player 1's context. The seat comes from the press's source bit (the keyboard's and mouse's are the
/// seat's while the keyboard plays it). Two presses are the
/// game's own, passed as they came: a menu opener's (so the pause poll's UI-table variant and the menus'
/// close handlers see the pad's Start as pad 1's), and every press while a menu owns the game's input
/// (the menus are driven through this route; the seat's own route stays out, as its menu stops natively).
char ActionDown(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled)
{
    const bool split = alreadyHandled == 0 && Acts(kPartEvents) && pad_sticks::Enabled() && players::generation::AnyActive() &&
                       players::generation::OnGameThread();
    if (!split)
    {
        return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
    }
    const int      keyboard = pad_table::KeyboardSeat();
    const unsigned seats    = SeatsIn();
    const int      seat     = button_view::SeatOfSource(sourceBit, seats, keyboard);
    if (g_menuPlayer != 0)
    {
        // A seat's shop is up: its pad is the menu pad, as the one pad is in single player; player 1's presses (pad 1,
        // and the keyboard and mouse while they are player 1's) and any other seat's update the tables but are passed
        // as handled.
        if (((button_view::PlayerOneDevices(seats, keyboard) >> (sourceBit & 31)) & 1) != 0)
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
        std::uint64_t key = 0;
        const bool    opener = g_openersRead && event != nullptr &&
                            platform::CopyGuarded(&key, static_cast<const std::byte*>(event) + kActionEventKey, sizeof(key)) &&
                            Opener(key);
        const bool merged = MergedEverywhere();
        if (merged || opener)
        {
            ++(merged ? g_menuDowns : g_openerDowns)[seat];
            if (opener && !merged && platform::Enabled(platform::Level::Debug))
            {
                platform::LogDebug(L"buttons: pad {}'s {} press goes to the game's pause poll and menus as it came (already "
                                   L"handled {})",
                                   seat + 1, players::unreal::NameText(key), alreadyHandled);
            }
            return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
        }
        ++g_seatDowns[seat];
        char result = 0;
        // The route finds the seat from the source bit itself (menu_input_route).
        if (g_seatRoute != nullptr &&
            g_seatRoute(g_actionDown, owner, unused, sourceBit, event, alreadyHandled, result))
        {
            return result;  // the seat's own UI took it
        }
        alreadyHandled = 1;
    }
    return g_actionDown(owner, unused, sourceBit, event, alreadyHandled);
}

/// The extra-key calls: the same as the main loop's (ActionDown), with each key logged by name (the
/// first few, every pad), since which keys come this way is not otherwise known.
char ExtraKeyDown(void* owner, void* unused, std::uint32_t sourceBit, void* event, std::uint8_t alreadyHandled)
{
    // The call sites were changed on the understanding that only the game thread runs them; a call from
    // another thread would mean a swap could have torn them (call_redirect.h).
    static std::atomic<unsigned> offThread{0};
    if (!players::generation::OnGameThread() && offThread.fetch_add(1, std::memory_order_relaxed) < 5)
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
        const int seat = button_view::SeatOfSource(sourceBit, built, pad_table::KeyboardSeat());
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
    if (!players::generation::OnGameThread() && offThread.fetch_add(1, std::memory_order_relaxed) < 5)
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
        platform::Log(L"buttons: players 2 and 3's presses kept from player 1's menus, prompts and battle commands at {} of {} "
                      L"more dispatch call(s) (crossing a block; changed from the game thread)",
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
    platform::Log(L"buttons: pads 2 and 3's extra keys (the shortcut keys) take the same route at {} of {} extra-key calls",
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
        platform::Log(L"buttons: player 1's devices drive the menus again ({} of their presses were kept from the UI "
                      L"meanwhile)",
                      g_padOneDowns);
    }
    g_padOneDowns = 0;
}

bool SharedPlay() noexcept
{
    return g_minigameTime && g_menuPlayer == 0;
}

void SetSeatRoute(SeatRouteFn route) noexcept
{
    g_seatRoute = route;
}

bool TakeLeftThumbPress(int seat) noexcept
{
    if (!players::SeatBuilt(seat) || !players::generation::OnGameThread())
    {
        return false;
    }
    return std::exchange(g_leftThumbPressed[static_cast<std::size_t>(seat)], false) && g_active;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto owner  = players::game_globals::Address(players::game_globals::Global::SquareInputOwner);
    const auto engine = platform::FindFunction(image, kInputTick.target);
    if (!owner || !engine)
    {
        return false;
    }
    g_inputOwner = reinterpret_cast<const std::byte* const*>(owner);
    g_engineTick = reinterpret_cast<TickFn>(*engine);
    FindTraceKeys(image);
    if (!platform::ReplaceSlot(image, kInputTick, reinterpret_cast<std::uintptr_t>(&Tick), g_tickSlot))
    {
        return false;
    }
    const auto target = platform::FindFunction(image, kActionDown);
    unsigned   calls  = 0;
    unsigned   keyboardSites = 0;  // the keyboard's and mouse's calls found (installed now or at the first Tick)
    if (target)
    {
        g_actionDown = reinterpret_cast<ActionDownFn>(*target);
        for (std::size_t i = 0; i < kActionDownCalls.size(); ++i)
        {
            const auto site = image.Rebase(kActionDownCalls[i], 5);
            keyboardSites += site && i >= kFirstKeyboardCall ? 1 : 0;
            if (site && platform::RunningBuild() != platform::BuildIndex::V1004 && !players::call_site::FitsOneBlock(*site))
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
    for (std::size_t i = 0; i < kOpenerKeyNames.size(); ++i)
    {
        const auto key  = image.Rebase(kOpenerKeyNames[i], sizeof(std::uint64_t));
        g_openerKeys[i] = key ? reinterpret_cast<const std::uint64_t*>(*key) : nullptr;
    }
    const auto leftThumb = image.Rebase(facts::kLeftThumbstickKey, sizeof(std::uint64_t));
    g_leftThumbKey       = leftThumb ? reinterpret_cast<const std::uint64_t*>(*leftThumb) : nullptr;
    if (g_leftThumbKey == nullptr)
    {
        platform::LogWarning(L"buttons: the L3 key's FKey global is not what the research read; players 2 and 3 cannot "
                             L"ask to be brought back with L3");
    }
    if (std::any_of(g_openerKeys.begin(), g_openerKeys.end(), [](const std::uint64_t* key) { return key == nullptr; }))
    {
        g_openerFrames = kOpenerReadFrames + 1;  // never read: nothing to wait for
        platform::LogWarning(L"buttons: a menu-opening action's FKey global is not what the research read; pad 2's or 3's "
                             L"Start, map and enemy-intel presses stay its own player's (no pause from it)");
    }
    const auto late = std::count_if(g_lateActionDownSites.begin(), g_lateActionDownSites.end(),
                                    [](std::uintptr_t site) { return site != 0; });
    platform::Log(L"buttons: players 2 and 3's presses (their pads, and the keyboard and mouse while they play one) kept "
                  L"from player 1's menus, prompts and battle commands at {} of {} dispatch calls{}",
                  calls,
                  kActionDownCalls.size(),
                  late != 0 ? std::format(L" ({} more from the first input tick: crossing a block)", late) : std::wstring());
    if (keyboardSites != kActionDownCalls.size() - kFirstKeyboardCall)
    {
        platform::LogWarning(L"buttons: {} of the keyboard's and mouse's {} dispatch calls found: while the keyboard plays "
                             L"player 2 or 3, its presses there still reach player 1's menus, prompts and battle commands",
                             keyboardSites, kActionDownCalls.size() - kFirstKeyboardCall);
    }
    return true;
}

void Remove() noexcept
{
    g_seatRoute = nullptr;  // no route outlives the buttons' redirects
    if (!g_tickSlot.Restore())
    {
        platform::Log(L"buttons: the input Tick slot was changed after us; left in place (it passes the call through)");
    }
    // The extra-key calls (and on 1.005 a dispatch call that crosses a block) are restored only from the game thread
    // (call_redirect.h). Off it, they stay ours; the DLL is pinned, and with no seat in they pass every press through.
    const bool gameThread = players::generation::OnGameThread();
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
            platform::Log(L"buttons: an extra-key call left in place ({}); it passes presses through with players 2 and 3 out",
                          gameThread ? L"changed after us" : L"removed off the game thread");
        }
    }
}

ButtonScope::ButtonScope(int seat, std::uint64_t withoutKey) noexcept
{
    if (g_scopeSeat != 0 || !g_active || !players::SeatBuilt(seat) || !ViewsWanted() || !players::generation::Active(seat))
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
        // table.ones still holds what the Tick wrote (the menu player's view, or the merged state): nothing else
        // writes it between the Tick and a scope, and scopes do not nest. Recomputing it could differ (a shop opened
        // since) from what Wrote recorded, and the next Tick would take the restore for a game write.
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
    platform::Log(L"buttons: mask {} (split views {}, pads 2 and 3 kept from menus and prompts {})", mask, Acts(kPartViews),
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
                                 L"UI {} (menu openers passed {}, presses in a menu passed {}), ",
                                 players::SeatName(seat), g_scopes[seat], g_scopeSkips[seat], seat + 1, g_seatDowns[seat],
                                 g_openerDowns[seat], g_menuDowns[seat]);
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
    g_scopes = g_scopeSkips = g_seatDowns = g_openerDowns = g_menuDowns = PerSeat{};
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"buttons-mask",
                       .shape = control::Shape::Integer,
                       .max   = 3,
                       .help  = L"diagnosis: which parts of per-pad buttons act (1 the split views, 2 pads 2 and 3 kept from "
                                L"menus and prompts; the pause menu for every pad needs 1); both at start",
                       .set   = [](const Value& v) {
                           SetMask(static_cast<unsigned>(std::get<std::int64_t>(v)));
                           return Outcome{true, L"buttons mask set (see the log)"};
                       },
                       .get = [] { return Value{std::int64_t{Mask()}}; }});
}

#endif

}  // namespace lss::input::pad_buttons