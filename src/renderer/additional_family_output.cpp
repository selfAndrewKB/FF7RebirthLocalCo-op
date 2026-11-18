#include "renderer/additional_family_output.h"

#include "facts/engine.h"
#include "platform/code_redirect.h"
#include "platform/command_registry.h"
#include "platform/image.h"
#include "platform/instruction_patch.h"
#include "platform/log.h"
#include "renderer/output_load_action_code.h"

#include <cstdint>
#include <span>
#include <variant>

namespace lss::renderer {

namespace {

platform::CodeRedirect      g_redirect;
platform::InstructionPatch  g_extraTargets;  // the tonemap's frame-generation targets: load, not clear

/// The tonemap's two frame-generation targets load instead of clear, so a later family's tonemap keeps the earlier
/// families' parts of the back buffer with frame generation on as it does with it off. Logs its outcome; the
/// back-buffer rule stands without it.
void InstallExtraTargetsLoad(const platform::GameImage& image) noexcept
{
    if (platform::RunningBuild() == platform::BuildIndex::V1004)
    {
        platform::Log(L"frame generation targets: not placed on 1.004 (its tonemap stores were never read); with "
                      L"frame generation on, a later player's part of the screen clears the earlier ones");
        return;
    }
    const auto site = image.Rebase(facts::kTonemapExtraTargetsLoadStore, sizeof(facts::kTonemapExtraTargetsClearBytes));
    const bool ok   = site && g_extraTargets.Apply(*site, facts::kTonemapExtraTargetsClearBytes,
                                                   facts::kTonemapExtraTargetsLoadBytes);
    if (ok)
    {
        platform::Log(L"frame generation targets: the tonemap's HUD-less and UI targets load instead of clear, so "
                      L"with frame generation on a later player's part of the screen keeps the earlier ones");
        return;
    }
    platform::LogWarning(L"frame generation targets: NOT placed (the tonemap's store is missing or is not the expected "
                         L"instruction); with frame generation on, a later player's part of the screen clears the "
                         L"earlier ones");
}

}  // namespace

bool InstallAdditionalFamilyOutput(const platform::GameImage& image) noexcept
{
    // 1.004 compiled the store differently (its own bytes and code; output_load_action_code.h).
    const bool v1004 = platform::RunningBuild() == platform::BuildIndex::V1004;
    const std::span<const std::uint8_t> store =
        v1004 ? std::span<const std::uint8_t>(facts::kOutputLoadActionStoreBytes1004) : facts::kOutputLoadActionStoreBytes;
    const std::span<const std::uint8_t> code =
        v1004 ? std::span<const std::uint8_t>(kFirstUnlessAdditionalLoadAction1004) : kFirstUnlessAdditionalLoadAction;
    const auto site   = image.Rebase(facts::kOutputLoadActionStore, store.size());
    const auto resume = image.Rebase(facts::kOutputLoadActionResume);
    if (!site || !resume || !g_redirect.Install(*site, store, code, *resume))
    {
        return false;
    }
    InstallExtraTargetsLoad(image);
    return true;
}

void RemoveAdditionalFamilyOutput() noexcept
{
    if (!g_extraTargets.Restore())
    {
        platform::Log(L"could not restore the tonemap's frame-generation target store (changed after us); left as it is");
    }
    if (!g_redirect.Restore())
    {
        platform::Log(L"could not restore the final output's load-action instructions (changed after us); "
                      L"they are left as they are");
    }
}

#if LSS_DEV

void RegisterAdditionalFamilyOutputCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"fg-targets-load",
                       .shape = control::Shape::OnOff,
                       .help  = L"test: the tonemap's frame-generation targets load (on, the mod's rule) or clear (off, "
                                L"the game's)",
                       .set   = [](const Value& v) {
                           const bool on = std::get<bool>(v);
                           if (!g_extraTargets.WasApplied())
                           {
                               return Outcome{false, L"the frame-generation target store was never placed"};
                           }
                           const bool done = on ? (g_extraTargets.IsApplied() || g_extraTargets.Reapply())
                                                : g_extraTargets.Restore();
                           platform::Log(L"frame generation targets: {} (the console){}",
                                         on ? L"load (the mod's rule)" : L"clear (the game's own store)",
                                         done ? L"" : L": FAILED, the instruction changed after us");
                           return Outcome{done, done ? L"" : L"the instruction changed after us"};
                       },
                       .get = [] { return Value{g_extraTargets.IsApplied()}; }});
}

#endif

}  // namespace lss::renderer
