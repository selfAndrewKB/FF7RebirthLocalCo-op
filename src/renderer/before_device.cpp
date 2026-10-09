#include "renderer/before_device.h"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::renderer {

namespace {

/// The exe's delay-import slot for d3d12.dll's D3D12CreateDevice (ordinal 101, or by name), or null. The loader
/// fills it on the exe's first call; until then it holds the exe's own resolver stub.
const std::uintptr_t* CreateDeviceSlot(std::uintptr_t base) noexcept
{
    const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dir.VirtualAddress == 0)
    {
        return nullptr;
    }
    for (auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress); d->DllNameRVA != 0; ++d)
    {
        if (d->Attributes.RvaBased == 0 || _stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), "d3d12.dll") != 0)
        {
            continue;
        }
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->ImportNameTableRVA);
        const auto* slots = reinterpret_cast<const std::uintptr_t*>(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData != 0; ++names, ++slots)
        {
            const bool byOrdinal = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal);
            if ((byOrdinal && IMAGE_ORDINAL64(names->u1.Ordinal) == 101) ||
                (!byOrdinal && std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name,
                                           "D3D12CreateDevice") == 0))
            {
                return slots;
            }
        }
    }
    return nullptr;
}

}  // namespace

bool NoD3D12DeviceYet(std::uintptr_t base) noexcept
{
    const auto* slot = CreateDeviceSlot(base);
    if (slot == nullptr)
    {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return *slot >= base && *slot < base + nt->OptionalHeader.SizeOfImage;
}

}  // namespace lss::renderer
