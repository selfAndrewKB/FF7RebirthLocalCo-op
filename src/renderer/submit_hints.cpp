#include "renderer/submit_hints.h"

#include "facts/engine.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/instruction_patch.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "platform/rip_relative.h"
#include "platform/verified_code.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>

namespace lss::renderer {

namespace {

// `lea rax,[FRHICommandSubmitCommandsHint vtable]; mov [rcx],rax` where the parallel-pass dispatch builds its hint node.
// Research build; 1.005 0x141018B78. The opcode is checked, and the displacement must reach kSubmitCommandsHintVTable.
constexpr std::uint8_t   kLeaOpcode[]  = {0x48, 0x8D, 0x05};
constexpr platform::FunctionFact kHintNodeLea{L"the parallel dispatch's hint-node lea", 0x14190784C, kLeaOpcode};
using facts::kSubmitCommandsHintVTable;
using facts::kSubmitCommandsHintExecute;
constexpr std::uint8_t   kRetZero[]    = {0xC2, 0x00, 0x00};
// slot 0 of the vtable 8 bytes on; 1.005 0x140743B50
constexpr platform::FunctionFact kNullsub{L"the hint vtable's nullsub", 0x14076B370, kRetZero};
constexpr std::uint8_t   kVtableStep   = 8;

struct Site
{
    std::uintptr_t lowByte = 0;  // the displacement's low byte, the one byte written
    std::uint8_t   stock   = 0;
    std::uint8_t   skip    = 0;  // stock + kVtableStep
};

std::optional<Site>        g_site;
platform::InstructionPatch g_patch;  // the byte, in while player 2 is in (platform/instruction_patch.h)
bool                       g_skipping = false;
bool                       g_followed = false;  // player 2 in, as last followed

std::optional<Site> Locate(const platform::GameImage& image) noexcept
{
    const auto lea     = image.Rebase(kHintNodeLea.address, sizeof(kLeaOpcode) + 4);
    const auto vtable  = image.Rebase(kSubmitCommandsHintVTable, 2 * sizeof(void*));
    const auto execute = image.Rebase(kSubmitCommandsHintExecute);
    const auto nullsub = platform::FindFunction(image, kNullsub);
    if (!lea || !vtable || !execute || !nullsub || !platform::FindFunction(image, kHintNodeLea))
    {
        return std::nullopt;
    }
    // The slots are read from the live vtable: a hand-found address translates only as itself, not at +8.
    std::int32_t   displacement = 0;
    std::uintptr_t slots[2]     = {};
    const auto     lowByte      = *lea + sizeof(kLeaOpcode);
    if (image.RipTarget(kHintNodeLea.address, kLeaOpcode) != vtable || !platform::Read(lowByte, displacement) ||
        !platform::Read(*vtable, slots) || slots[0] != *execute || slots[1] != *nullsub ||
        !platform::LowByteFits(displacement, kVtableStep))  // the +8 must stay in the low byte (one byte written)
    {
        return std::nullopt;
    }
    Site site;
    site.lowByte = lowByte;
    site.stock   = static_cast<std::uint8_t>(displacement);
    site.skip    = static_cast<std::uint8_t>(site.stock + kVtableStep);
    return site;
}

}  // namespace

void InstallSubmitHintSkip(const platform::GameImage& image) noexcept
{
    if (platform::FlagFile(L"submit_hints_on.txt"))
    {
        platform::Log(L"GPU submissions: parallel passes keep their submit hints (submit_hints_on.txt)");
        return;
    }
    if (platform::WatchOnly())
    {
        platform::Log(L"GPU submissions: parallel passes keep their submit hints (watch_only.txt: the game runs unmodified)");
        return;
    }
    g_site = Locate(image);
    if (g_site)
    {
        platform::Log(L"GPU submissions: the parallel passes' submit hint found; skipped while a further player is in");
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
    // The first skip applies the patch; later ones apply the same byte again. Either way the byte must still be
    // the one Locate read.
    const bool swapped = skip ? (g_patch.Reapply() || g_patch.Apply(g_site->lowByte, std::span(&g_site->stock, 1),
                                                                     std::span(&g_site->skip, 1)))
                              : g_patch.Restore();
    if (!swapped)
    {
        // Someone else changed the block: stand aside for the session rather than write over them.
        platform::Log(L"GPU submissions: the submit hint's code changed under us; left alone from now on");
        g_site.reset();
        g_skipping = false;  // the byte is no longer ours: the reading says stock
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

bool SubmitHintSkipOn() noexcept
{
    return g_skipping;
}

void RemoveSubmitHintSkip() noexcept
{
    SetSubmitHintSkip(false);
}

void FollowViews(bool severalViews) noexcept
{
    if (severalViews != g_followed)
    {
        g_followed = severalViews;
        SetSubmitHintSkip(severalViews);
    }
}

#if LSS_DEV

void RegisterSubmitHintCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"submit-hints",
                       .shape = control::Shape::Word,
                       .words = {L"skip", L"stock"},
                       .help  = L"parallel passes without their own GPU submission (skip), or the game's; skip while "
                                L"a further player is in, set again at the next join or leave",
                       .set   = [](const Value& v) {
                           const bool skip = std::get<std::wstring>(v) == L"skip";
                           if (!SubmitHintSkipInstalled())
                           {
                               return Outcome{false, L"the submit-hint skip is not installed (see the install log)"};
                           }
                           SetSubmitHintSkip(skip);
                           return Outcome{true, skip ? L"parallel passes skip their submit hint"
                                                     : L"the game's submit hints"};
                       },
                       .get = [] { return Value{std::wstring(SubmitHintSkipOn() ? L"skip" : L"stock")}; }});
}

#endif

}  // namespace lss::renderer
