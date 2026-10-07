#include "renderer/additional_family_output.h"

#include "platform/code_redirect.h"
#include "platform/engine_addresses.h"
#include "platform/image.h"
#include "platform/log.h"
#include "renderer/output_load_action_code.h"

#include <cstdint>
#include <span>

namespace lss::renderer {

namespace {

platform::CodeRedirect g_redirect;

}  // namespace

bool InstallAdditionalFamilyOutput(const platform::GameImage& image) noexcept
{
    // 1.005 compiled the store differently (its own bytes and code; output_load_action_code.h).
    const bool v1005 = platform::RunningBuild() == platform::BuildIndex::V1005;
    const std::span<const std::uint8_t> store =
        v1005 ? std::span<const std::uint8_t>(engine::kOutputLoadActionStoreBytes1005) : engine::kOutputLoadActionStoreBytes;
    const std::span<const std::uint8_t> code =
        v1005 ? std::span<const std::uint8_t>(kFirstUnlessAdditionalLoadAction1005) : kFirstUnlessAdditionalLoadAction;
    const auto site   = image.Rebase(engine::kOutputLoadActionStore, store.size());
    const auto resume = image.Rebase(engine::kOutputLoadActionResume);
    if (!site || !resume)
    {
        return false;
    }
    return g_redirect.Install(*site, store, code, *resume);
}

void RemoveAdditionalFamilyOutput() noexcept
{
    if (!g_redirect.Restore())
    {
        platform::Log(L"could not restore the final output's load-action instructions (changed after us); "
                      L"they are left as they are");
    }
}

}  // namespace lss::renderer
