#include "renderer/submit_hints.h"

#include "platform/code_memory.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_folder.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace lss::renderer {

namespace {

// `lea rax,[FRHICommandSubmitCommandsHint vtable]; mov [rcx],rax` where the parallel-pass dispatch builds its hint node.
// Research build; 1.005 0x141018B78. The opcode is checked, and the displacement must reach kHintVtable.
constexpr std::uintptr_t kHintNodeLea  = 0x14190784C;
constexpr std::uint8_t   kLeaOpcode[]  = {0x48, 0x8D, 0x05};
constexpr std::uintptr_t kHintVtable   = 0x146602920;  // 1.005 0x145AD4AB0
constexpr std::uintptr_t kHintExecute  = 0x140D00988;  // its slot 0; 1.005 0x140AE3E90
constexpr std::uintptr_t kNullsub      = 0x14076B370;  // slot 0 of the vtable 8 bytes on; 1.005 0x140743B50
constexpr std::uint8_t   kRetZero[]    = {0xC2, 0x00, 0x00};
constexpr std::uint8_t   kVtableStep   = 8;

struct Site
{
    std::uintptr_t      block = 0;  // the aligned 16-byte block holding the displacement's low byte
    platform::CodeBlock stock{};
    platform::CodeBlock skip{};
};

std::optional<Site> g_site;
bool                g_skipping = false;

std::optional<Site> Locate(const platform::GameImage& image) noexcept
{
    const auto lea     = image.Rebase(kHintNodeLea, sizeof(kLeaOpcode) + 4);
    const auto vtable  = image.Rebase(kHintVtable, 2 * sizeof(void*));
    const auto execute = image.Rebase(kHintExecute);
    const auto nullsub = image.Rebase(kNullsub, sizeof(kRetZero));
    if (!lea || !vtable || !execute || !nullsub || !image.BytesMatch(kHintNodeLea, kLeaOpcode) ||
        !image.BytesMatch(kNullsub, kRetZero))
    {
        return std::nullopt;
    }
    // The slots are read from the live vtable: a hand-found address translates only as itself, not at +8.
    std::int32_t   displacement = 0;
    std::uintptr_t slots[2]     = {};
    const auto     lowByte      = *lea + sizeof(kLeaOpcode);
    if (!platform::CopyGuarded(&displacement, reinterpret_cast<const void*>(lowByte), sizeof(displacement)) ||
        !platform::CopyGuarded(slots, reinterpret_cast<const void*>(*vtable), sizeof(slots)) ||
        *lea + sizeof(kLeaOpcode) + sizeof(displacement) + displacement != *vtable || slots[0] != *execute ||
        slots[1] != *nullsub ||
        (displacement & 0xFF) > 0xFF - kVtableStep)  // the +8 must stay in the low byte (one byte written)
    {
        return std::nullopt;
    }
    Site site;
    site.block = lowByte & ~(platform::kCodeBlockSize - 1);
    if (!platform::CopyGuarded(site.stock.data(), reinterpret_cast<const void*>(site.block), site.stock.size()))
    {
        return std::nullopt;
    }
    site.skip = site.stock;
    site.skip[lowByte - site.block] = static_cast<std::uint8_t>(site.skip[lowByte - site.block] + kVtableStep);
    return site;
}

}  // namespace

void InstallSubmitHintSkip() noexcept
{
    std::error_code missing;
    if (std::filesystem::exists(platform::ModFolder() / L"submit_hints_on.txt", missing))
    {
        platform::Log(L"GPU submissions: parallel passes keep their submit hints (submit_hints_on.txt)");
        return;
    }
    if (std::filesystem::exists(platform::ModFolder() / L"watch_only.txt", missing))
    {
        platform::Log(L"GPU submissions: parallel passes keep their submit hints (watch_only.txt: the game runs unmodified)");
        return;
    }
    const auto image = platform::GameImage::Locate();
    g_site           = image ? Locate(*image) : std::nullopt;
    if (g_site)
    {
        platform::Log(L"GPU submissions: the parallel passes' submit hint found; skipped while player 2 is in");
    }
    else
    {
        platform::Log(L"GPU submissions: the parallel passes' submit hint differs in this build; left as the game has it");
    }
}

bool SubmitHintSkipInstalled() noexcept
{
    return g_site.has_value();
}

void SetSubmitHintSkip(bool skip) noexcept
{
    if (!g_site || skip == g_skipping)
    {
        return;
    }
    const bool swapped = skip ? platform::SwapCodeBlock(g_site->block, g_site->stock, g_site->skip)
                              : platform::SwapCodeBlock(g_site->block, g_site->skip, g_site->stock);
    if (!swapped)
    {
        // Someone else changed the block: stand aside for the session rather than write over them.
        platform::Log(L"GPU submissions: the submit hint's code changed under us; left alone from now on");
        g_site.reset();
        return;
    }
    g_skipping = skip;
    if (skip)
    {
        platform::Log(L"GPU submissions: parallel passes skip their submit hint (one submission fewer each)");
    }
    else
    {
        platform::Log(L"GPU submissions: parallel passes submit at their hint again (the game's way)");
    }
}

void RemoveSubmitHintSkip() noexcept
{
    SetSubmitHintSkip(false);
}

}  // namespace lss::renderer
