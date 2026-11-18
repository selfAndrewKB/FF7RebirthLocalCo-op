#pragma once

// The game's DLSS Frame Generation through NVIDIA Streamline: the function pointers the game loaded from
// sl.interposer.dll, Streamline's structs as the game builds them, and the game's per-view reset gates. Read in IDA
// (research/RE-p2-framegen.md, RE-p2-framegen-2.md, RE-p2-framegen-4.md); the structs are Streamline's own (its
// public sl.h, sl_consts.h, sl_dlss_g.h).

#include <cstddef>
#include <cstdint>

namespace lss::facts {

// Each global holds a pointer into sl.interposer.dll that the game loads at every call (`call qword ptr [rip+g]`,
// `mov rax, [rip+g]`), never caching it, filled once when the game creates its D3D12 device (SL_LoadInterposerProcs,
// SL_InitD3D12DeviceAndFeatures). The globals hold function pointers, not functions.
inline constexpr std::uintptr_t kSlSetTag          = 0x1491F72C8;  // slSetTag(viewport, tags, count, command list)
inline constexpr std::uintptr_t kSlSetConstants    = 0x1491F72D8;  // slSetConstants(constants, frame token, viewport)
inline constexpr std::uintptr_t kSlFreeResources   = 0x1491F72C0;  // slFreeResources(feature, viewport)
inline constexpr std::uintptr_t kSlDLSSGSetOptions = 0x1491F7350;  // slDLSSGSetOptions(viewport, options)

// The frame generation method's state bytes (the interface's IsEnabled and SetActiveThisFrame slots read and write them).
inline constexpr std::uintptr_t kFgEnabled         = 0x146EF8A28;  // u8: DLSS-G initialised
inline constexpr std::uintptr_t kFgActiveThisFrame = 0x146EF8A2A;  // u8: r.FrameGeneration.Method != 0 at the last draw

// sl::ViewportHandle
inline constexpr std::size_t kSlViewportId = 0x20;  // u32

// sl::ResourceTag, 0x40 bytes; the game tags one at a time
inline constexpr std::size_t   kSlTagSize        = 0x40;
inline constexpr std::size_t   kSlTagResource    = 0x20;  // sl::Resource*
inline constexpr std::size_t   kSlTagType        = 0x28;  // u32 sl::BufferType
inline constexpr std::size_t   kSlTagLifecycle   = 0x2C;  // u32 sl::ResourceLifecycle
inline constexpr std::size_t   kSlTagExtent      = 0x30;  // u32 top, left, width, height
inline constexpr std::size_t   kSlResourceNative = 0x28;  // ID3D12Resource* in sl::Resource
inline constexpr std::uint32_t kSlOnlyValidNow   = 0;     // sl::ResourceLifecycle::eOnlyValidNow

// sl::BufferType values the game tags (FG_MapTagToSLBufferType)
inline constexpr std::uint32_t kSlDepth         = 0;
inline constexpr std::uint32_t kSlMotionVectors = 1;
inline constexpr std::uint32_t kSlHudLessColor  = 2;
inline constexpr std::uint32_t kSlUiColorAlpha  = 23;
inline constexpr std::uint32_t kSlBackbuffer    = 53;

// sl::FrameToken: its first virtual is `operator uint32_t`, the frame index
inline constexpr std::size_t kSlFrameTokenIndexSlot = 0;

// sl::DLSSGOptions, 0x70 bytes
inline constexpr std::size_t   kSlOptionsSize    = 0x70;
inline constexpr std::size_t   kSlOptionsMode    = 0x20;  // u32 sl::DLSSGMode
inline constexpr std::size_t   kSlOptionsFrames  = 0x24;  // u32 numFramesToGenerate
inline constexpr std::size_t   kSlOptionsFlags   = 0x28;  // u32 sl::DLSSGFlags
inline constexpr std::size_t   kSlOptionsDynamic = 0x2C;  // u32 dynamic resolution width, height
inline constexpr std::uint32_t kSlModeOff        = 0;     // sl::DLSSGMode::eOff

// sl::Constants, 0x1C8 bytes (the five matrices from 0x20)
inline constexpr std::size_t kSlConstantsSize   = 0x1C8;
inline constexpr std::size_t kSlConstantsJitter = 0x160;  // float2 jitterOffset
inline constexpr std::size_t kSlConstantsPos    = 0x178;  // float3 cameraPos
inline constexpr std::size_t kSlConstantsFwd    = 0x19C;  // float3 cameraFwd
inline constexpr std::size_t kSlConstantsFov    = 0x1B0;  // float cameraFOV (radians)
inline constexpr std::size_t kSlConstantsAspect = 0x1B4;  // float cameraAspectRatio
inline constexpr std::size_t kSlConstantsReset  = 0x1BF;  // u8 reset (sl::Boolean)

// The per-view reset gates: the DLSS-G constants' reset and the velocity history's reset are each set only when the
// view's family has `+0x70 == 0`, `cmp dword [reg+70h], 0` then `jnz +5` past the set. The mod sets `+0x70` on every
// family after the first, so without a change only the first view ever resets on a camera cut. The mod writes
// `cmp eax, eax; nop` over the 4-byte compare (zero flag always set, the jump never taken), so each view's reset follows
// its own camera cut. The compare, not the jump, because the constants' jump crosses a 16-byte block. Checked whole:
// the compare and the jump after it.
inline constexpr std::uintptr_t kFgConstantsResetGate  = 0x142E9035B;  // in FStreamlineDLSSFG_Render_RenderThread
inline constexpr std::uint8_t   kFgConstantsResetBytes[] = {0x83, 0x78, 0x70, 0x00, 0x75, 0x05};
inline constexpr std::uintptr_t kFgVelocityResetGate   = 0x144831066;  // in FG_AddVelocityPass_TemporalAAIntermediate
inline constexpr std::uint8_t   kFgVelocityResetBytes[]  = {0x83, 0x79, 0x70, 0x00, 0x75, 0x05};
inline constexpr std::size_t    kFgResetGateCompareSize  = 4;  // the compare the mod replaces
inline constexpr std::uint8_t   kFgResetGateAlwaysEqual[] = {0x39, 0xC0, 0x66, 0x90};  // cmp eax, eax; nop

}  // namespace lss::facts
