#include "renderer/async_compute.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "renderer/before_device.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>

namespace lss::renderer {

namespace {

// r.D3D12.AllowAsyncCompute's int (0 in the file). Research build; 1.005 through the build table.
constexpr std::uintptr_t kAllowAsyncCompute = 0x1492EC9E4;

// GSupportsEfficientAsyncCompute (bool). Research build; 1.005 through the build table.
constexpr std::uintptr_t kSupportsEfficientAsyncCompute = 0x1491163C2;

// In the D3D12 device initialisation: `cmp dword [AllowAsyncCompute],ebx` (ebx 0), and after the vendor rule
// `mov byte [GSupportsEfficientAsyncCompute],bl`. Research build; 1.005 through the build table. Each is checked
// up to its displacement, and the displacement must reach the address above.
constexpr std::uintptr_t kSettingReadSite    = 0x144940C81;
constexpr std::uint8_t   kSettingReadBytes[] = {0x39, 0x1D};
constexpr std::uintptr_t kFlagStoreSite      = 0x144940CB1;
constexpr std::uint8_t   kFlagStoreBytes[]   = {0x88, 0x1D};

/// True if the RIP-relative instruction at `site` (opcode bytes `opcode`, then a 32-bit displacement that ends the
/// instruction) addresses `target`.
bool Reaches(const platform::GameImage& image, std::uintptr_t site, std::span<const std::uint8_t> opcode,
             std::uintptr_t target) noexcept
{
    const auto wanted = image.Rebase(target);
    return wanted && image.RipTarget(site, opcode) == wanted;
}

struct Live
{
    std::uintptr_t setting;  // r.D3D12.AllowAsyncCompute
    std::uintptr_t flag;     // GSupportsEfficientAsyncCompute
};

std::optional<Live> Locate(const platform::GameImage& image) noexcept
{
    if (!Reaches(image, kSettingReadSite, kSettingReadBytes, kAllowAsyncCompute) ||
        !Reaches(image, kFlagStoreSite, kFlagStoreBytes, kSupportsEfficientAsyncCompute))
    {
        return std::nullopt;
    }
    return Live{*image.Rebase(kAllowAsyncCompute, 4), *image.Rebase(kSupportsEfficientAsyncCompute)};
}

}  // namespace

void ApplyAsyncComputeOn() noexcept
{
    if (!platform::FlagFile(L"async_compute_on.txt"))
    {
        platform::LogDebug(L"D3D12 async compute: the game's setting kept (no async_compute_on.txt)");
        return;
    }
    if (platform::WatchOnly())
    {
        platform::Log(L"D3D12 async compute: the game's setting kept (watch_only.txt: the game runs unmodified)");
        return;
    }
    const auto image = platform::GameImage::Locate();
    const auto found = image ? Locate(*image) : std::nullopt;
    if (!found)
    {
        platform::Log(L"D3D12 async compute: left alone (the device's setting read or flag store differs in this build)");
        return;
    }
    if (!NoD3D12DeviceYet(image->Base()))
    {
        platform::Log(L"D3D12 async compute: left alone (the game has already called D3D12CreateDevice, or its "
                      L"delay-import slot was not found)");
        return;
    }
    std::int32_t value = -1;
    if (!platform::CopyGuarded(&value, reinterpret_cast<const void*>(found->setting), sizeof(value)) || value != 0)
    {
        platform::Log(L"D3D12 async compute: left alone (r.D3D12.AllowAsyncCompute reads {}, not the file's 0)", value);
        return;
    }
    const std::int32_t on = 1;
    if (!platform::WriteGuarded(reinterpret_cast<void*>(found->setting), &on, sizeof(on)))
    {
        platform::Log(L"D3D12 async compute: left alone (r.D3D12.AllowAsyncCompute could not be written)");
        return;
    }
    platform::Log(L"D3D12 async compute: r.D3D12.AllowAsyncCompute set to 1 before the D3D12 device exists; the "
                  L"device's vendor rule decides (async_compute_on.txt)");
}

void ReportAsyncCompute(const platform::GameImage& image) noexcept
{
    const auto found = Locate(image);
    if (!found)
    {
        return;
    }
    std::int32_t setting = -1;
    std::uint8_t flag    = 0xFF;
    const bool   read    = platform::CopyGuarded(&setting, reinterpret_cast<const void*>(found->setting), sizeof(setting)) &&
                      platform::CopyGuarded(&flag, reinterpret_cast<const void*>(found->flag), sizeof(flag));
    if (!read)
    {
        platform::Log(L"D3D12 async compute after start-up: unreadable");
        return;
    }
    platform::Log(L"D3D12 async compute after start-up: {} (r.D3D12.AllowAsyncCompute {}, GSupportsEfficientAsyncCompute {})",
                  flag == 1 ? L"on" : flag == 0 ? L"off" : L"unknown", setting, flag);
}

}  // namespace lss::renderer
