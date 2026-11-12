#include "hud/minimap/label_relayouts.h"

#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::label_relayouts {

namespace {

namespace generation = players::generation;

// SEndTextBlock (research/RE-p2-minimap-cost.md). Tick, slot 1 of its vtable: writes the allotted size to +0x558;
// with wrap (+0x408), +0x498 or justification on and that width ≠ +0x560, stores it in +0x560 and sets +0x565; then, if
// +0x565 is set, clears it and relayouts. The entry bytes run up to its `vmovsd [rcx+558h], xmm0`.
constexpr std::uint8_t kTickBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xA4, 0xEA, 0x22, 0x01, 0x48, 0x2B,
                                       0xE0, 0xC5, 0xFB, 0x10, 0x02, 0xC5, 0xFB, 0x11, 0x81, 0x58, 0x05, 0x00, 0x00};
constexpr platform::SlotFact kTick{L"the text block's Tick slot (1)", 0x1457936F8,
                                   {L"SEndTextBlock__Tick_RelayoutIfWidthChanged", 0x140E80480, kTickBytes}};

constexpr std::uint32_t kText      = 0x2C8;  // FString {TCHAR* data, int32 Num (with the null), int32 Max}
constexpr std::uint32_t kTextNum   = 0x2D0;
constexpr std::uint32_t kWidth     = 0x558;  // the allotted size Tick stores (width, height)
constexpr std::uint32_t kWrapWidth = 0x560;  // the width the layout was made for
constexpr std::uint32_t kLock      = 0x564;  // set: a relayout only marks itself pending
constexpr std::uint32_t kPending   = 0x565;

constexpr std::size_t  kTextChars     = 40;   // kept per text
constexpr std::size_t  kCompareChars  = 256;  // compared per text (longer texts: their first 256)
constexpr std::size_t  kMaxBlocks     = 256;  // text blocks tracked per window
constexpr std::size_t  kListedBlocks  = 8;    // in the 5-second Debug line
constexpr ULONGLONG    kWindowMs      = 5'000;

using TickFn = void (*)(void* self, const void* geometry, double currentTime, float deltaTime);

// The places counted apart: elsewhere, then each player's minimap run (0 player 1's), then each seat's paint.
constexpr std::size_t kRunPlace   = 1;                                  // + the player
constexpr std::size_t kPaintPlace = kRunPlace + players::kMaxPlayers;   // + the seat
constexpr std::size_t kPlaces     = kPaintPlace + players::kMaxPlayers;
using ByPlace = std::array<std::uint32_t, kPlaces>;

struct Block
{
    ByPlace       setText{};  // relayouts through SetText, per place
    ByPlace       paint{};    // relayouts in Tick, per place
    std::uint32_t total          = 0;
    std::uint32_t sameText       = 0;  // SetText relayouts with the same text (too long for the game's compare)
    std::uint32_t changedText    = 0;
    std::uint32_t seatState      = 0;  // SetText relayouts on a seat's own label state (minimap_labels)
    std::uint32_t pending        = 0;  // Tick relayouts that found one pending
    std::uint32_t widthChanged   = 0;  // Tick relayouts after a width change
    std::uint32_t locked         = 0;  // relayouts that only marked themselves pending
    float         widthFrom      = 0.0f;
    float         widthTo        = 0.0f;
    std::wstring  text;               // at its last relayout
    bool          everyFrame     = false;  // reported at Info
};

platform::VTableEntry g_tickSlot;
std::atomic<bool>     g_watching{false};  // written on the game thread (and by Remove), read in the Tick slot
std::size_t           g_place     = 0;  // where the frame is: an index of ByPlace
ULONGLONG             g_windowEnd = 0;
std::uint64_t         g_windowFrame = 0;
std::uint32_t         g_untracked = 0;  // relayouts of blocks past kMaxBlocks this window
std::unordered_map<std::uintptr_t, Block> g_blocks;

std::wstring PlaceName(std::size_t place)
{
    if (place >= kPaintPlace && place < kPlaces)
    {
        return std::format(L"{}'s paint", players::SeatName(static_cast<int>(place - kPaintPlace)));
    }
    if (place >= kRunPlace && place < kPaintPlace)
    {
        return std::format(L"{}'s minimap run", players::SeatName(static_cast<int>(place - kRunPlace)));
    }
    return L"elsewhere";
}

using platform::Read;  // the guarded reads: platform/guarded.h

/// Up to kTextChars characters of an FString {data, Num}; line breaks as spaces.
std::wstring Characters(const void* data, int num) noexcept
{
    std::wstring out;
    if (data == nullptr || num <= 1)
    {
        return out;
    }
    std::array<wchar_t, kTextChars> buffer{};
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(num - 1), kTextChars);
    if (!platform::CopyGuarded(buffer.data(), data, count * sizeof(wchar_t)))
    {
        return L"(unreadable)";
    }
    out.assign(buffer.data(), count);
    std::replace_if(out.begin(), out.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }, L' ');
    return out;
}

/// Two FString buffers of `num` characters (with the null) hold the same text, compared up to kCompareChars.
bool SameCharacters(const void* a, const void* b, int num) noexcept
{
    if (num <= 1 || a == b)
    {
        return true;
    }
    std::array<wchar_t, kCompareChars> left{}, right{};
    const std::size_t bytes = std::min<std::size_t>(static_cast<std::size_t>(num - 1), kCompareChars) * sizeof(wchar_t);
    return a != nullptr && b != nullptr && platform::CopyGuarded(left.data(), a, bytes) &&
           platform::CopyGuarded(right.data(), b, bytes) && std::memcmp(left.data(), right.data(), bytes) == 0;
}

std::wstring TextOf(const void* textBlock) noexcept
{
    const void* data = nullptr;
    int         num  = 0;
    Read(textBlock, kText, data);
    Read(textBlock, kTextNum, num);
    return Characters(data, num);
}

Block* Track(const void* textBlock) noexcept
{
    const auto key = reinterpret_cast<std::uintptr_t>(textBlock);
    if (const auto found = g_blocks.find(key); found != g_blocks.end())
    {
        return &found->second;
    }
    if (g_blocks.size() >= kMaxBlocks)
    {
        ++g_untracked;
        return nullptr;
    }
    return &g_blocks[key];
}

void TickSlot(void* self, const void* geometry, double currentTime, float deltaTime)
{
    const auto original = reinterpret_cast<TickFn>(g_tickSlot.Original());
    if (!g_watching.load(std::memory_order_relaxed) || !generation::OnGameThread())
    {
        original(self, geometry, currentTime, deltaTime);
        return;
    }
    // The game is ticking this text block, so its memory is live for the call.
    const auto*  w             = static_cast<const std::byte*>(self);
    std::uint8_t pendingBefore = 0;
    float        wrapBefore = 0.0f, wrapAfter = 0.0f, width = 0.0f;
    std::memcpy(&pendingBefore, w + kPending, sizeof(pendingBefore));
    std::memcpy(&wrapBefore, w + kWrapWidth, sizeof(wrapBefore));
    original(self, geometry, currentTime, deltaTime);
    std::memcpy(&wrapAfter, w + kWrapWidth, sizeof(wrapAfter));
    const bool widthChanged = std::memcmp(&wrapBefore, &wrapAfter, sizeof(float)) != 0;
    if (pendingBefore == 0 && !widthChanged)
    {
        return;  // Tick relayouts only with one pending or after a new width (see kTick)
    }
    std::uint8_t locked = 0;
    std::memcpy(&locked, w + kLock, sizeof(locked));
    std::memcpy(&width, w + kWidth, sizeof(width));
    Block* const block = Track(self);
    if (block == nullptr)
    {
        return;
    }
    ++block->paint[g_place];
    ++block->total;
    block->pending += pendingBefore != 0 ? 1u : 0u;
    block->locked += locked != 0 ? 1u : 0u;
    if (widthChanged)
    {
        ++block->widthChanged;
        block->widthFrom = wrapBefore;
        block->widthTo   = wrapAfter;
    }
    block->text = TextOf(self);
    if (platform::Enabled(platform::Level::Trace))
    {
        platform::LogTrace(L"label relayouts: {} '{}' relaid out in its Tick in {} ({}; allotted width {:.1f}{})", self,
                           block->text, PlaceName(g_place),
                           pendingBefore != 0 ? L"one was pending" : L"its width changed", width,
                           locked != 0 ? L"; locked: marked pending again" : L"");
    }
}

std::wstring Describe(const Block& b)
{
    std::wstring where;
    for (std::size_t s = 0; s < b.setText.size(); ++s)
    {
        if (b.setText[s] != 0)
        {
            where += std::format(L"{}SetText in {} {}", where.empty() ? L"" : L", ", PlaceName(s), b.setText[s]);
        }
    }
    for (std::size_t s = 0; s < b.paint.size(); ++s)
    {
        if (b.paint[s] != 0)
        {
            where += std::format(L"{}Tick in {} {}", where.empty() ? L"" : L", ", PlaceName(s), b.paint[s]);
        }
    }
    std::wstring why;
    if (b.sameText != 0)
    {
        why += std::format(L"; same text {} (the game's compare skips only short texts)", b.sameText);
    }
    if (b.changedText != 0)
    {
        why += std::format(L"; text changed {}", b.changedText);
    }
    if (b.seatState != 0)
    {
        why += std::format(L"; on the run's player's own label state {}", b.seatState);
    }
    if (b.pending != 0)
    {
        why += std::format(L"; Tick found one pending {}", b.pending);
    }
    if (b.widthChanged != 0)
    {
        why += std::format(L"; width changed {} (last {:.1f} -> {:.1f})", b.widthChanged, b.widthFrom, b.widthTo);
    }
    if (b.locked != 0)
    {
        why += std::format(L"; locked {}", b.locked);
    }
    return std::format(L"'{}' ({} characters kept): {}{}", b.text, b.text.size(), where, why);
}

void Report(std::uint64_t frames)
{
    std::array<std::uint64_t, kPlaces> setText{}, paint{};
    std::vector<std::pair<std::uintptr_t, const Block*>> listed;
    for (auto& [key, block] : g_blocks)
    {
        for (std::size_t s = 0; s < kPlaces; ++s)
        {
            setText[s] += block.setText[s];
            paint[s] += block.paint[s];
        }
        const bool often = frames != 0 && block.total * 2 >= frames;
        if (often && !block.everyFrame)
        {
            block.everyFrame = true;
            platform::Log(L"label relayouts: text block {:#x} is laid out again about every frame ({} in {} frames): {}",
                          key, block.total, frames, Describe(block));
        }
        else if (!often && block.everyFrame && block.total * 10 < frames)
        {
            block.everyFrame = false;
            platform::Log(L"label relayouts: text block {:#x} ('{}') stopped relaying out every frame ({} in {} frames)",
                          key, block.text, block.total, frames);
        }
        if (block.total != 0)
        {
            listed.emplace_back(key, &block);
        }
    }
    if (!listed.empty() && platform::Enabled(platform::Level::Debug))
    {
        std::sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.second->total > b.second->total; });
        std::wstring blocks;
        for (std::size_t i = 0; i < listed.size() && i < kListedBlocks; ++i)
        {
            blocks += std::format(L"\n    {:#x} x{}: {}", listed[i].first, listed[i].second->total, Describe(*listed[i].second));
        }
        std::wstring totals;  // every place with a relayout this window
        for (const bool viaSetText : {true, false})
        {
            for (std::size_t s = 0; s < kPlaces; ++s)
            {
                if (const std::uint64_t count = viaSetText ? setText[s] : paint[s]; count != 0)
                {
                    totals += std::format(L"; {} in {} {}", viaSetText ? L"SetText" : L"Tick", PlaceName(s), count);
                }
            }
        }
        platform::LogDebug(L"label relayouts (5 s, {} frames): {} block(s){}{}{}", frames, listed.size(),
                           g_untracked != 0 ? std::format(L" (+{} relayouts untracked)", g_untracked) : L"", totals, blocks);
    }
    // Next window: the counts start again; a block reported every frame keeps its record (and its flag).
    for (auto it = g_blocks.begin(); it != g_blocks.end();)
    {
        if (it->second.everyFrame)
        {
            Block kept;
            kept.everyFrame = true;
            kept.text       = std::move(it->second.text);
            it->second      = std::move(kept);
            ++it;
        }
        else
        {
            it = g_blocks.erase(it);
        }
    }
    g_untracked = 0;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    if (!platform::ReplaceSlot(image, kTick, reinterpret_cast<std::uintptr_t>(&TickSlot), g_tickSlot))
    {
        platform::Log(L"label relayouts: the text block's Tick is not what the research read; only SetText relayouts are "
                      L"counted");
        return false;
    }
    platform::Log(L"label relayouts: watching (which text blocks are laid out again, where and why; a line when one "
                  L"relayouts about every frame)");
    return true;
}

void Remove() noexcept
{
    g_watching.store(false, std::memory_order_relaxed);
    if (g_tickSlot.Original() != 0 && !g_tickSlot.Restore())
    {
        platform::Log(L"label relayouts: the Tick slot was changed after us; left in place (it passes through)");
    }
}

void SetScope(Scope scope, int player) noexcept
{
    const bool named = player >= (scope == Scope::Paint ? 1 : 0) && player < players::kMaxPlayers;
    if (scope == Scope::Other || !named)
    {
        g_place = 0;
        return;
    }
    g_place = (scope == Scope::Run ? kRunPlace : kPaintPlace) + static_cast<std::size_t>(player);
}

Before BeforeSetText(const void* textBlock, const void* text) noexcept
{
    Before before;
    if (textBlock == nullptr || text == nullptr || !g_watching.load(std::memory_order_relaxed) ||
        !generation::OnGameThread())
    {
        return before;
    }
    const void*  newData = nullptr;
    int          newNum  = 0;
    std::uint8_t locked  = 0;
    before.read = Read(textBlock, kText, before.data) && Read(textBlock, kTextNum, before.num) &&
                  Read(textBlock, kLock, locked) && Read(text, 0, newData) && Read(text, sizeof(void*), newNum);
    before.locked = locked != 0;
    before.same   = before.read && before.num == newNum && SameCharacters(before.data, newData, newNum);
    return before;
}

void AfterSetText(const void* textBlock, const Before& before, bool seatState) noexcept
{
    const void* data = nullptr;
    int         num  = 0;
    if (!before.read || !Read(textBlock, kText, data) || !Read(textBlock, kTextNum, num) ||
        (data == before.data && num == before.num))
    {
        return;  // the game kept the text it had: no relayout
    }
    Block* const block = Track(textBlock);
    if (block == nullptr)
    {
        return;
    }
    block->text = Characters(data, num);
    if (before.locked)
    {
        ++block->locked;  // the relayout only marked itself pending: its Tick lays it out and is counted there
        return;
    }
    ++block->setText[g_place];
    ++block->total;
    ++(before.same ? block->sameText : block->changedText);
    block->seatState += seatState ? 1u : 0u;
    if (platform::Enabled(platform::Level::Trace))
    {
        platform::LogTrace(L"label relayouts: {} '{}' relaid out by SetText in {} ({}{}{})", textBlock, block->text,
                           PlaceName(g_place), before.same ? L"same text" : L"text changed",
                           seatState ? L"; the run's player's own state" : L"", before.locked ? L"; locked" : L"");
    }
}

void Tick(bool watch) noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    if (watch != g_watching.load(std::memory_order_relaxed))
    {
        g_watching.store(watch, std::memory_order_relaxed);
        g_blocks.clear();
        g_untracked = 0;
        g_windowEnd = 0;
    }
    if (!watch)
    {
        return;
    }
    const ULONGLONG     now   = GetTickCount64();
    const std::uint64_t frame = platform::frame_watch::FrameIndex();
    if (g_windowEnd == 0)
    {
        g_windowEnd   = now + kWindowMs;
        g_windowFrame = frame;
        return;
    }
    if (now < g_windowEnd)
    {
        return;
    }
    try
    {
        Report(frame - g_windowFrame);
    }
    catch (...)
    {
        g_blocks.clear();  // a summary that could not be built (out of memory) starts the counts again
        g_untracked = 0;
    }
    g_windowEnd   = now + kWindowMs;
    g_windowFrame = frame;
}

}  // namespace lss::hud::label_relayouts
