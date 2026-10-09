#include "renderer/residency_off.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "renderer/before_device.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace lss::renderer {

namespace {

// GEnableResidencyManagement (1 in the file). Research build; 1.005 0x146DDD290.
constexpr std::uintptr_t kResidencyGate = 0x146FAA3B0;

// FD3D12Adapter's device creation reads the setting and clears the gate: `cmp [rax+r9*4],r14d; jne +7;
// mov byte [gate],r14b`. Research build; 1.005 0x1446B8BB3. Checked up to the store's displacement.
constexpr std::uintptr_t kGateStoreSite    = 0x144924B9B;
constexpr std::uint8_t   kGateStoreBytes[] = {0x46, 0x39, 0x34, 0x88, 0x75, 0x07, 0x44, 0x88, 0x35};

std::optional<std::uintptr_t> GateAddress(const platform::GameImage& image) noexcept
{
    // The store `44 88 35 disp32` is the last instruction of the checked bytes plus its displacement; that displacement
    // must reach the gate (as async_compute's Reaches does).
    constexpr std::size_t kStoreOffset = sizeof(kGateStoreBytes) - 3;
    const auto            gate         = image.Rebase(kResidencyGate);
    const auto            site         = image.Rebase(kGateStoreSite, sizeof(kGateStoreBytes) + 4);
    std::int32_t          displacement = 0;
    if (!gate || !site || !image.BytesMatch(kGateStoreSite, kGateStoreBytes) ||
        !platform::CopyGuarded(&displacement, reinterpret_cast<const void*>(*site + sizeof(kGateStoreBytes)),
                               sizeof(displacement)))
    {
        return std::nullopt;
    }
    const std::uintptr_t next = *site + kStoreOffset + 3 + sizeof(displacement);
    return next + displacement == *gate ? gate : std::nullopt;
}

int ReadGate(std::uintptr_t gate) noexcept
{
    std::uint8_t value = 0;
    return platform::CopyGuarded(&value, reinterpret_cast<const void*>(gate), 1) ? value : -1;
}

}  // namespace

void ApplyResidencyOff() noexcept
{
    std::error_code missing;
    if (std::filesystem::exists(platform::ModFolder() / L"residency_on.txt", missing))
    {
        platform::Log(L"D3D12 residency management: kept on (residency_on.txt)");
        return;
    }
    if (std::filesystem::exists(platform::ModFolder() / L"watch_only.txt", missing))
    {
        platform::Log(L"D3D12 residency management: kept on (watch_only.txt: the game runs unmodified)");
        return;
    }
    const auto image = platform::GameImage::Locate();
    const auto gate  = image ? GateAddress(*image) : std::nullopt;
    if (!gate)
    {
        platform::Log(L"D3D12 residency management: left on (the device's setting read or the gate differs in this build)");
        return;
    }
    // d3d12.dll itself may already be loaded by others (seen live: before the mod starts); the exe's own slot says
    // whether the game has created a device.
    if (!NoD3D12DeviceYet(image->Base()))
    {
        platform::Log(L"D3D12 residency management: left on (the game has already called D3D12CreateDevice, or its "
                      L"delay-import slot was not found)");
        return;
    }
    if (const int value = ReadGate(*gate); value != 1)
    {
        platform::Log(L"D3D12 residency management: left alone (the gate reads {}, not the file's 1)", value);
        return;
    }
    const std::uint8_t off = 0;
    if (!platform::WriteGuarded(reinterpret_cast<void*>(*gate), &off, sizeof(off)))
    {
        platform::Log(L"D3D12 residency management: left on (the gate could not be written)");
        return;
    }
    platform::Log(L"D3D12 residency management: off for this session (cleared before the D3D12 device exists; "
                  L"residency_on.txt keeps it)");
}

void ReportResidency() noexcept
{
    const auto image = platform::GameImage::Locate();
    const auto gate  = image ? GateAddress(*image) : std::nullopt;
    if (gate)
    {
        const int value = ReadGate(*gate);
        platform::Log(L"D3D12 residency management after start-up: {} (gate {})",
                      value == 0 ? L"off" : value == 1 ? L"on" : L"unreadable", value);
    }
}

}  // namespace lss::renderer
