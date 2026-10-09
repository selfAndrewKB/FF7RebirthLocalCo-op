#pragma once

// Whether the game has created its D3D12 device yet. Device settings the engine reads once, at device creation
// (residency_off.h, async_compute.h), can only be changed before that.

#include <cstdint>

namespace lss::renderer {

/// True if the exe has never called D3D12CreateDevice: its delay-import slot for it still points inside the exe (the
/// loader fills it on the first call). False if the slot was not found. d3d12.dll itself may be loaded earlier by
/// others (seen live), so the module's presence says nothing.
bool NoD3D12DeviceYet(std::uintptr_t imageBase) noexcept;

}  // namespace lss::renderer
