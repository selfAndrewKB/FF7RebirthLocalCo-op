#include "player_two/battle/focus_scope.h"

#include "facts/hud.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace lss::player_two::focus_scope {

namespace {

using facts::kUICommandController;  // the UI system's slots (facts/hud.h)
using facts::kUIInput;
using facts::kUIKeyGuide;

// Facts read from the database (RE-p2-command-menu-input.md §2, §3.3, §10; this build).
// The input object's focus context, as {offset, size} copied in this order: the focus path
// (FWeakWidgetPath), the focus layers (TArray), the last focus cause, and the capture map (a TSet
// keyed by action: the widget that took a held action's press gets its release; the command menu
// adds one for each held Attack, Unique or Guard, so it is per-player state like the path), then the
// menu owner byte (+0x64: written by the member selecter's tick in the field top states, read by
// SquareInput_Owner64Gate 0x143A1D738 in the key-down mapper; per-pad input state tied to that player's
// menu, RE-p2-command-menu-levers.md §5.2).
struct Region
{
    std::uint32_t offset, size;
};
constexpr std::array<Region, 5> kContext{{{0x08, 32}, {0x40, 16}, {0x78, 1}, {0xA8, 0x50}, {0x64, 1}}};
constexpr std::uint32_t kDelegate = 0x1A0;  // the menu-core delegate, run when no widget takes an action
static_assert(kContext[0].size + kContext[1].size + kContext[2].size + kContext[3].size + kContext[4].size ==
              kContextSize);

using Kind  = platform::ByteContext::Kind;
using Range = platform::ByteContext::Range;

constexpr std::size_t kCaptureAt   = 32 + 16 + 1;  // the capture map's place in the copy
// An empty TSet as the game constructs one: all zero but its sparse array's FirstFreeIndex (+0x30), -1.
constexpr std::size_t kCaptureFirstFree = 0x30;

/// Player 1's live capture map read against the stock TSet layout the empty copy assumes: sparse array
/// Data {ptr, Num +0x08, Max +0x0C}, allocation bits {inline, secondary, NumBits +0x28, MaxBits +0x2C},
/// FirstFreeIndex +0x30, NumFreeIndices +0x34, HashSize +0x48. False if any count disagrees.
bool CaptureLayoutHolds(std::uintptr_t input) noexcept
{
    struct Counts
    {
        std::int32_t num = 0, max = 0, bits = 0, firstFree = 0, numFree = 0, hashSize = 0;
    } c;
    const auto map  = input + kContext[3].offset;
    const auto read = [map](std::uint32_t at, std::int32_t& out) {
        return platform::CopyGuarded(&out, reinterpret_cast<const void*>(map + at), sizeof(out));
    };
    if (!read(0x08, c.num) || !read(0x0C, c.max) || !read(0x28, c.bits) || !read(0x30, c.firstFree) ||
        !read(0x34, c.numFree) || !read(0x48, c.hashSize))
    {
        return false;
    }
    const bool freeList = c.firstFree == -1 ? c.numFree == 0 : c.firstFree >= 0 && c.firstFree < c.num && c.numFree > 0;
    return c.num >= 0 && c.num <= c.max && c.bits == c.num && c.numFree >= 0 && c.numFree <= c.num && freeList &&
           c.hashSize >= 0 && (c.hashSize & (c.hashSize - 1)) == 0;
}

std::array<std::uint8_t, kContextSize> EmptyContext() noexcept
{
    std::array<std::uint8_t, kContextSize> context{};
    const std::int32_t none = -1;
    std::memcpy(&context[kCaptureAt + kCaptureFirstFree], &none, sizeof(none));
    return context;
}
// The focus layers (TArray of 40 bytes, sorted by priority; int32 priority at +0x20). The command
// menu's own layers sit at -1000 (RE-p2-command-menu-input.md §11).
constexpr std::uint32_t kLayerSize         = 40;
constexpr std::uint32_t kLayerPriority     = 0x20;
constexpr std::int32_t  kCommandMenuLayer  = -1000;

using Context = std::array<std::uint8_t, kContextSize>;

// Game thread. The per-seat records are indexed by the seat; index 0 (player 1, whose context is the game's own) is
// unused.
std::uintptr_t                                g_endUISystem = 0;
std::array<Context, players::kMaxPlayers>     g_contexts{EmptyContext(), EmptyContext(), EmptyContext()};  // between scopes
std::array<std::uintptr_t, players::kMaxPlayers> g_keyGuides{};  // each seat's key guide, or 0
bool g_inside = false;  // one scope at a time, whichever seat's: the seats swap in turn
int  g_layout = 0;      // player 1's capture map checked: 0 not yet, 1 holds, -1 does not
static_assert(players::kMaxPlayers == 3, "g_contexts' initialiser names every player");

/// `seat`'s copy into the context's five Exchange values (before a scope), or out of them (after it).
void CopyBetween(int seat, platform::ByteContext& context, bool intoContext) noexcept
{
    Context&    saved = g_contexts[static_cast<std::size_t>(seat)];
    std::size_t at    = 0;
    for (std::size_t i = 0; i < kContext.size(); ++i)
    {
        if (intoContext)
        {
            std::memcpy(context.Value(i).data(), saved.data() + at, kContext[i].size);
        }
        else
        {
            std::memcpy(saved.data() + at, context.Value(i).data(), kContext[i].size);
        }
        at += kContext[i].size;
    }
}

}  // namespace


bool Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(facts::kEndUISystem, kUIKeyGuide + sizeof(void*));
    g_endUISystem     = system ? *system : 0;
    return system.has_value();
}

void SetKeyGuide(int seat, void* keyGuide) noexcept
{
    if (players::SeatIndex(seat))
    {
        g_keyGuides[static_cast<std::size_t>(seat)] = reinterpret_cast<std::uintptr_t>(keyGuide);
    }
}

Summary ContextOf(int seat) noexcept
{
    Summary summary;
    if (!players::SeatIndex(seat))
    {
        return summary;
    }
    const Context& saved = g_contexts[static_cast<std::size_t>(seat)];
    std::memcpy(&summary.pathCount, &saved[8], sizeof(std::int32_t));        // path: Widgets.Num
    std::memcpy(&summary.layerCount, &saved[32 + 8], sizeof(std::int32_t));  // layers: Num
    return summary;
}

std::int32_t LivePathCount() noexcept
{
    std::uintptr_t input = 0;
    std::int32_t   count = 0;
    return g_endUISystem != 0 && Read(g_endUISystem + kUIInput, input) && input != 0 &&
                   Read(input + kContext[0].offset + 8, count)
               ? count
               : 0;
}

bool PlayerOneTopIsCommandMenu() noexcept
{
    std::uintptr_t input = 0, layers = 0;
    std::int32_t   count = 0, priority = 0;
    return !g_inside && Read(g_endUISystem + kUIInput, input) && input != 0 &&
           Read(input + kContext[1].offset, layers) &&
           Read(input + kContext[1].offset + 8, count) && layers != 0 && count > 0 &&
           Read(layers + static_cast<std::uintptr_t>(count - 1) * kLayerSize + kLayerPriority, priority) &&
           priority == kCommandMenuLayer;
}

FocusScope::FocusScope(int seat, void* controller) noexcept : seat_(seat)
{
    std::uintptr_t input = 0;
    if (!players::SeatIndex(seat))
    {
        refusal_ = L"not a seat";
    }
    else if (g_inside || !generation::OnGameThread())
    {
        refusal_ = L"inside another focus scope, or off the game thread";
    }
    else if (g_endUISystem == 0 || !Read(g_endUISystem + kUIInput, input) || input == 0)
    {
        refusal_ = L"no input object";
    }
    else if (g_layout == 0 && (g_layout = CaptureLayoutHolds(input) ? 1 : -1) < 0)
    {
        platform::Log(L"focus scope: player 1's capture map does not have the TSet layout the further players' copies "
                      L"assume; their UI stays off");
    }
    if (refusal_ == nullptr && g_layout < 0)
    {
        refusal_ = L"the capture map's layout is not the one assumed";
    }
    if (refusal_ != nullptr)
    {
        return;
    }
    // The five regions exchanged with the seat's copy; the delegate unbound (zeros), the UI system's controller slot
    // and, when the seat has one, its key guide slot set.
    const std::uintptr_t keyGuide = g_keyGuides[static_cast<std::size_t>(seat)];
    std::array<Range, 8> ranges{};
    std::size_t          count = 0;
    for (const Region& region : kContext)
    {
        ranges[count++] = {input + region.offset, region.size, Kind::Exchange};
    }
    ranges[count++] = {input + kDelegate, kDelegateSize, Kind::Set};
    ranges[count++] = {g_endUISystem + kUICommandController, sizeof(controller), Kind::Set};
    if (keyGuide != 0)
    {
        ranges[count++] = {g_endUISystem + kUIKeyGuide, sizeof(keyGuide), Kind::Set};
    }
    if (!context_.Define(std::span<const Range>(ranges.data(), count)))
    {
        refusal_ = L"the seat's context could not be laid out";
        return;
    }
    CopyBetween(seat, context_, true);
    std::memcpy(context_.Value(kContext.size() + 1).data(), &controller, sizeof(controller));
    if (keyGuide != 0)
    {
        std::memcpy(context_.Value(kContext.size() + 2).data(), &keyGuide, sizeof(keyGuide));
    }
    if (!context_.Open())
    {
        refusal_ = L"player 1's context could not be read, or the seat's not written (player 1's is in place)";
        return;
    }
    entered_ = true;
    g_inside = true;
}

FocusScope::~FocusScope()
{
    if (!entered_)
    {
        return;
    }
    g_inside = false;
    // Player 1's context back; what the game left in the five regions is the seat's now (the Exchange values).
    const unsigned kept = context_.Close();
    if (abandon_)
    {
        g_contexts[static_cast<std::size_t>(seat_)] = EmptyContext();
        platform::Log(L"focus scope: {}'s context abandoned after a fault (reset to empty)", players::SeatName(seat_));
    }
    else
    {
        CopyBetween(seat_, context_, false);
    }
    if (kept != 0)
    {
        platform::LogDebug(L"focus scope: the game wrote the delegate or a UI system slot inside {}'s scope; the "
                           L"game's value kept",
                           players::SeatName(seat_));
    }
    if (context_.Faulted())
    {
        platform::Log(L"focus scope: FAILED to put player 1's context back or keep {}'s (player 1's UI focus may be "
                      L"wrong)",
                      players::SeatName(seat_));
    }
}

}  // namespace lss::player_two::focus_scope
