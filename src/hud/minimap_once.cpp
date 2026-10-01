#include "hud/minimap_once.h"

#include "hud/minimap_once_code.h"
#include "hud/second_image.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "players/unreal_access.h"

#include <cstdint>
#include <format>
#include <span>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud {

namespace {

// Facts read from the database (RE-oom-path.md, "Player 2's minimap paint: hook facts and the skip"; this
// build). Each widget: the replaced instruction, the acquire and submit calls the skip jumps over (checked
// so the block is the one the research read) and the instruction after the submit, where it lands.
struct WidgetSite
{
    const wchar_t*                 name;
    std::uintptr_t                 site;
    std::span<const std::uint8_t>  siteBytes;
    std::uintptr_t                 acquireCall;
    std::span<const std::uint8_t>  acquireBytes;
    std::uintptr_t                 submitCall;
    std::span<const std::uint8_t>  submitBytes;
    std::uintptr_t                 afterSubmit;
    std::span<const std::uint8_t>  afterSubmitBytes;
};

constexpr std::uint8_t kCircleAcquire[]      = {0xE8, 0xE1, 0x19, 0x00, 0x00};  // call FEndMenuRenderer__AcquireLayerSlot
constexpr std::uint8_t kCircleSubmit[]       = {0xE8, 0x24, 0x18, 0x00, 0x00};  // call FEndMenuRenderer__SubmitLayerSlot
constexpr std::uint8_t kCircleAfterSubmit[]  = {0x49, 0x8B, 0x8F, 0xE0, 0x01, 0x00, 0x00};  // mov rcx,[r15+1E0h]
constexpr std::uint8_t kOctagonAcquire[]     = {0xE8, 0x44, 0x84, 0x56, 0xFD};
constexpr std::uint8_t kOctagonSubmit[]      = {0xE8, 0x57, 0x82, 0x56, 0xFD};
constexpr std::uint8_t kOctagonAfterSubmit[] = {0x49, 0x8B, 0x8E, 0xE0, 0x01, 0x00, 0x00};  // mov rcx,[r14+1E0h]

constexpr WidgetSite kCircle{L"circle", 0x140FC6C95, kCircleSiteBytes, 0x140FC6CAE, kCircleAcquire,
                             0x140FC6F13, kCircleSubmit, 0x140FC6F18, kCircleAfterSubmit};
constexpr WidgetSite kOctagon{L"octagon", 0x143A60235, kOctagonSiteBytes, 0x143A6024B, kOctagonAcquire,
                              0x143A604E0, kOctagonSubmit, 0x143A604E5, kOctagonAfterSubmit};

// FEndMenuRenderer pool 0 (the render-to-texture pool), read for the log only.
constexpr std::uint32_t kPoolZeroSlots      = 40;    // FEndMenuLayerSlot* (40 bytes each)
constexpr std::uint32_t kPoolZeroSlotCount  = 48;    // int32
constexpr std::uint32_t kPoolZeroWrite      = 56;    // uint8
constexpr std::uint32_t kPoolZeroRead       = 57;    // uint8
constexpr std::uint32_t kSlotSize           = 40;
constexpr std::uint32_t kSlotActive         = 0;     // TArray<TSharedPtr<entry>>: data, +8 Num
constexpr std::uint32_t kSlotRecycled       = 16;
constexpr std::uint32_t kSlotState          = 32;    // 0 initial, 1 acquired, 2 submitted, 3 consuming, 4 consumed
constexpr std::uint32_t kSlotFrame          = 36;    // GFrameNumber at submit
constexpr std::uint32_t kEntryBatchData     = 144;
constexpr std::uint32_t kBatchVertexFree    = 8;     // VertexArrayFreeList Num
constexpr std::uint32_t kBatchIndexFree     = 24;
constexpr std::uint32_t kBatchVertexArrays  = 40;    // BatchVertexArrays Num
constexpr std::uint32_t kBatchIndexArrays   = 56;
constexpr int           kMaxSlots           = 4;
constexpr int           kMaxEntriesLogged   = 4;

constexpr ULONGLONG kLogPeriodMs = 10'000;

MinimapOnceFields      g_fields;  // read and counted into by the installed code, through its address
platform::CodeRedirect g_circleCode;
platform::CodeRedirect g_octagonCode;
bool                   g_installed = false;
bool                   g_on        = false;

// Game thread.
std::uint64_t g_playerTwoPasses = 0;
std::uint64_t g_logged[5]       = {};  // the fields' four counters and the passes, at the last line
ULONGLONG     g_logAt           = 0;

std::uint64_t Counter(const std::uint64_t& field) noexcept
{
    return reinterpret_cast<const volatile std::uint64_t&>(field);
}

bool Install(const platform::GameImage& image, platform::CodeRedirect& redirect, const WidgetSite& widget,
             std::span<const std::uint8_t> code) noexcept
{
    const auto site = image.Rebase(widget.site, widget.siteBytes.size());
    if (!site || !image.BytesMatch(widget.acquireCall, widget.acquireBytes) ||
        !image.BytesMatch(widget.submitCall, widget.submitBytes) ||
        !image.BytesMatch(widget.afterSubmit, widget.afterSubmitBytes))
    {
        platform::Log(L"minimap once: the {}'s paint is not what the research read; its map renders in both passes",
                      widget.name);
        return false;
    }
    if (!redirect.Install(*site, widget.siteBytes, code, *site + widget.siteBytes.size()))
    {
        platform::Log(L"minimap once: the {}'s site could not be redirected; its map renders in both passes", widget.name);
        return false;
    }
    return true;
}

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return address != 0 && platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

/// One reading of pool 0: its ring and each layer entry's batch arrays (in use and on the free lists).
std::wstring PoolZeroReading() noexcept
{
    const auto renderer = reinterpret_cast<std::uintptr_t>(MenuRenderer());
    std::uintptr_t slots = 0;
    std::int32_t   count = 0;
    std::uint8_t   write = 0, read = 0;
    if (renderer == 0 || !Read(renderer + kPoolZeroSlots, slots) || !Read(renderer + kPoolZeroSlotCount, count) ||
        !Read(renderer + kPoolZeroWrite, write) || !Read(renderer + kPoolZeroRead, read) || slots == 0 || count <= 0 ||
        count > kMaxSlots)
    {
        return L"pool 0 not readable";
    }
    std::wstring text = std::format(L"pool 0: write {} read {}; slots", write, read);
    std::uintptr_t entries[kMaxEntriesLogged] = {};
    int            entryCount                 = 0;
    for (int i = 0; i < count; ++i)
    {
        const std::uintptr_t slot  = slots + static_cast<std::uintptr_t>(i) * kSlotSize;
        std::int32_t         state = -1;
        std::uint32_t        frame = 0;
        Read(slot + kSlotState, state);
        Read(slot + kSlotFrame, frame);
        text += std::format(L" [{} frame {}]", state, frame);
        for (const std::uint32_t list : {kSlotActive, kSlotRecycled})
        {
            std::uintptr_t data = 0;
            std::int32_t   num  = 0;
            if (!Read(slot + list, data) || !Read(slot + list + 8, num) || data == 0 || num <= 0 || num > 16)
            {
                continue;
            }
            for (std::int32_t k = 0; k < num; ++k)
            {
                std::uintptr_t entry = 0;  // a TSharedPtr: the object first
                if (!Read(data + static_cast<std::uintptr_t>(k) * 16, entry) || entry == 0)
                {
                    continue;
                }
                bool seen = false;
                for (int e = 0; e < entryCount; ++e)
                {
                    seen = seen || entries[e] == entry;
                }
                if (!seen && entryCount < kMaxEntriesLogged)
                {
                    entries[entryCount++] = entry;
                }
            }
        }
    }
    for (int e = 0; e < entryCount; ++e)
    {
        std::uint64_t  name = 0;
        std::uintptr_t batch = 0;
        std::uint32_t  vertexArrays = 0, vertexFree = 0, indexArrays = 0, indexFree = 0;
        if (!Read(entries[e], name) || !Read(entries[e] + kEntryBatchData, batch) || batch == 0 ||
            !Read(batch + kBatchVertexArrays, vertexArrays) || !Read(batch + kBatchVertexFree, vertexFree) ||
            !Read(batch + kBatchIndexArrays, indexArrays) || !Read(batch + kBatchIndexFree, indexFree))
        {
            continue;
        }
        text += std::format(L"; {}: vertex arrays {} ({} free), index arrays {} ({} free)", players::unreal::NameText(name),
                            vertexArrays, vertexFree, indexArrays, indexFree);
    }
    return text;
}

}  // namespace

bool InstallMinimapOnce(const platform::GameImage& image) noexcept
{
    // The installed code reads g_fields, which lives in this DLL: it must never unload while that code can run.
    if (!platform::PinThisModule())
    {
        platform::Log(L"minimap once: this DLL could not be pinned; the minimap renders in both passes");
        return false;
    }
    const auto circleAfter  = image.Rebase(kCircle.afterSubmit);
    const auto octagonAfter = image.Rebase(kOctagon.afterSubmit);
    const bool circle  = circleAfter && Install(image, g_circleCode, kCircle, CircleSkipCode(g_fields, *circleAfter));
    const bool octagon = octagonAfter && Install(image, g_octagonCode, kOctagon, OctagonSkipCode(g_fields, *octagonAfter));
    g_installed = circle || octagon;
    g_on        = g_installed;
    if (g_installed)
    {
        platform::Log(L"minimap once: installed (circle {}, octagon {}); player 2's HUD pass skips the minimap's own map "
                      L"render, player 1's pass renders it once per frame",
                      circle ? L"yes" : L"no", octagon ? L"yes" : L"no");
    }
    return g_installed;
}

void RemoveMinimapOnce() noexcept
{
    if (!g_installed)
    {
        return;
    }
    g_on = false;
    bool all = true;
    for (platform::CodeRedirect* code : {&g_circleCode, &g_octagonCode})
    {
        all = (!code->IsInstalled() || code->Restore()) && all;
    }
    if (!all)
    {
        platform::Log(L"minimap once: a site was changed after us; left in place (it passes through with the switch off)");
    }
    g_installed = false;
}

bool MinimapOnceInstalled() noexcept
{
    return g_installed;
}

void SetMinimapOnce(bool on) noexcept
{
    g_on = on && g_installed;
    platform::Log(L"minimap once: {}", g_on ? L"on (player 1's pass alone renders the map)"
                                             : L"off (the map renders in both passes, as the game code runs)");
}

bool MinimapOnceOn() noexcept
{
    return g_on;
}

void BeginMinimapPlayerTwoPass() noexcept
{
    ++g_playerTwoPasses;
    reinterpret_cast<volatile std::uint64_t&>(g_fields.skip) = g_on ? 1 : 0;
}

void EndMinimapPlayerTwoPass() noexcept
{
    reinterpret_cast<volatile std::uint64_t&>(g_fields.skip) = 0;
}

void PollMinimapOnce() noexcept
{
    if (!g_installed)
    {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_logAt)
    {
        return;
    }
    g_logAt = now + kLogPeriodMs;
    const std::uint64_t counts[5] = {Counter(g_fields.circlePainted), Counter(g_fields.circleSkipped),
                                     Counter(g_fields.octagonPainted), Counter(g_fields.octagonSkipped), g_playerTwoPasses};
    const bool painted = counts[0] != g_logged[0] || counts[1] != g_logged[1] || counts[2] != g_logged[2] ||
                         counts[3] != g_logged[3];
    if (!painted)
    {
        return;  // no minimap on screen this period (the open world shows a compass)
    }
    platform::Log(L"minimap once ({}): in the last period the circle's map rendered {} and skipped {} times, the "
                  L"octagon's rendered {} and skipped {}; player 2's HUD passes {}; {}",
                  g_on ? L"on" : L"off", counts[0] - g_logged[0], counts[1] - g_logged[1], counts[2] - g_logged[2],
                  counts[3] - g_logged[3], counts[4] - g_logged[4], PoolZeroReading());
    for (int i = 0; i < 5; ++i)
    {
        g_logged[i] = counts[i];
    }
}

}  // namespace lss::hud
