#include "renderer/null_view_probe.h"

#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "renderer/null_view_probe_code.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace lss::renderer {

namespace {

// Facts read from the database (RE-p2-main-menu-crash.md; this build). The site's compare is
// replaced; the bytes before it (the view load, its null test and the handle store) and the game's
// empty-element path are checked too, since the code relies on their registers.
constexpr std::uintptr_t kSite          = 0x140816AF3;
constexpr std::uintptr_t kResume        = 0x140816AFA;
constexpr std::uintptr_t kViewLoad      = 0x140816AE2;
constexpr std::uint8_t   kViewLoadBytes[] = {0x48, 0x85, 0xF6, 0x0F, 0x84, 0x58, 0x82, 0x0D, 0x02,  // test rsi,rsi / je 0x1428EED43
                                             0x48, 0x8B, 0x46, 0x10,                               // mov rax, [rsi+10h]
                                             0x4A, 0x89, 0x04, 0x3A};                              // mov [rdx+r15], rax
constexpr std::uintptr_t kEmptyPath     = 0x1428EED43;
constexpr std::uint8_t   kEmptyPathBytes[] = {0x49, 0x8B, 0x40, 0x20,        // mov rax, [r8+20h]
                                              0x48, 0x8B, 0x48, 0x08,        // mov rcx, [rax+8]
                                              0x4A, 0x89, 0x0C, 0x3A,        // mov [rdx+r15], rcx
                                              0xE9, 0x50, 0x7E, 0xF2, 0xFD};  // jmp 0x140816BA4
constexpr std::uintptr_t kEmptyElement  = 0x140816BA4;
constexpr std::uint8_t   kEmptyElementBytes[] = {0x8B, 0x94, 0x24, 0x8C, 0x00, 0x00, 0x00};  // mov edx, [rsp+8Ch]
// The entry's one store of the stage (ebx = dl, movzx at 0x14081686A) to [rsp+0C0h], which the
// function never writes again; the code reads the stage there.
constexpr std::uintptr_t kStageStore      = 0x1408168C4;
constexpr std::uint8_t   kStageStoreBytes[] = {0x48, 0x89, 0x9C, 0x24, 0xC0, 0x00, 0x00, 0x00};  // mov [rsp+0C0h], rbx
constexpr std::uintptr_t kStageLoad       = 0x14081686A;
constexpr std::uint8_t   kStageLoadBytes[]  = {0x0F, 0xB6, 0xDA};  // movzx ebx, dl

constexpr std::uint64_t kFullLogHits = 20;  // every hit up to here, then at powers of ten

platform::CodeRedirect             g_redirect;
NullViewProbe                      g_probe;  // never freed: the code refers to it
std::optional<platform::GameImage> g_image;  // for naming vtables in the log
std::uint64_t                      g_loggedHits = 0;

/// The probe's code writes the fields from another thread; they are read as whole 8-byte values.
std::uint64_t Load(std::uint64_t& field) noexcept
{
    return std::atomic_ref<std::uint64_t>(field).load(std::memory_order_relaxed);
}

/// An address as IDA writes it when it lies in the game image, else 0.
std::uint64_t IdaAddress(std::uint64_t live) noexcept
{
    return g_image ? g_image->IdaAddressOf(live).value_or(0) : 0;
}

std::uint64_t ReadQword(std::uint64_t at) noexcept
{
    std::uint64_t value = 0;
    return at != 0 && platform::CopyGuarded(&value, reinterpret_cast<const void*>(at), sizeof(value)) ? value : 0;
}

/// Logs while the count is small, then each time it passes a power of ten.
bool ShouldLog(std::uint64_t previous, std::uint64_t hits) noexcept
{
    if (previous < kFullLogHits)
    {
        return true;
    }
    for (std::uint64_t power = 100; power <= hits; power *= 10)
    {
        if (previous < power)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

bool InstallNullViewProbe(const platform::GameImage& image) noexcept
{
    const auto site         = image.Rebase(kSite, sizeof(kNullViewProbeSiteBytes));
    const auto resume       = image.Rebase(kResume);
    const auto emptyElement = image.Rebase(kEmptyElement);
    if (!site || !resume || !emptyElement || !image.BytesMatch(kViewLoad, kViewLoadBytes) ||
        !image.BytesMatch(kEmptyPath, kEmptyPathBytes) || !image.BytesMatch(kEmptyElement, kEmptyElementBytes) ||
        !image.BytesMatch(kStageStore, kStageStoreBytes) || !image.BytesMatch(kStageLoad, kStageLoadBytes))
    {
        platform::Log(L"dead-view probe: the resource-array bind is not what the research read; not installed");
        return false;
    }
    g_image         = image;
    const auto code = NullViewProbeCode(g_probe, *emptyElement);
    if (!g_redirect.Install(*site, kNullViewProbeSiteBytes, code, *resume))
    {
        platform::Log(L"dead-view probe: the site could not be redirected; not installed");
        return false;
    }
    platform::Log(L"dead-view probe: a shader-resource view with no descriptor handle is logged and bound as an empty "
                  L"element (the game's null descriptor) instead of reaching the driver");
    return true;
}

void RemoveNullViewProbe() noexcept
{
    if (g_redirect.IsInstalled() && !g_redirect.Restore())
    {
        platform::Log(L"dead-view probe: the site was changed after us; left in place");
    }
}

void PollNullViewProbe(bool playerTwoIn) noexcept
{
    const std::uint64_t hits = Load(g_probe.hits);
    if (hits == g_loggedHits)
    {
        return;
    }
    if (!ShouldLog(g_loggedHits, hits))
    {
        return;
    }
    const std::uint64_t previous = g_loggedHits;
    g_loggedHits                 = hits;
    const std::uint64_t view     = Load(g_probe.view);
    const std::uint64_t resource = Load(g_probe.resource);
    const std::uint64_t vtable   = Load(g_probe.vtable);
    platform::Log(L"dead-view probe: {} view(s) with no descriptor handle so far (+{} since the last line; player 2 {}); "
                  L"latest: view 0x{:X} (vtable 0x{:X} = ida 0x{:X}), resource 0x{:X} (vtable ida 0x{:X}), stage {}, "
                  L"slot {}, element {}; bound as an empty element",
                  hits, hits - previous, playerTwoIn ? L"in" : L"out", view, vtable, IdaAddress(vtable), resource,
                  IdaAddress(ReadQword(resource)), Load(g_probe.stage), Load(g_probe.slot), Load(g_probe.element));
}

}  // namespace lss::renderer
