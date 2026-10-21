#include "player_two/framing_watch.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/guarded.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::player_two::framing_watch {

namespace {

// RE-p2-framing.md (this build). The Field operator (FEndModularCameraOperator, camera manager +0x2CB0):
constexpr std::uint32_t kOpInitialized   = 0xD2;   // u8: its lazy init ran (seeds the level from the option)
constexpr std::uint32_t kOpLayers        = 0x108;  // 8 setting layers, 0 Debug .. 7 Default
constexpr std::uint32_t kLayerStride     = 0x98;
constexpr std::uint32_t kLayerCount      = 8;
constexpr std::uint32_t kLayerWeight     = 0x30;  // float
constexpr std::uint32_t kLayerActive     = 0x35;  // u8
constexpr std::uint32_t kLayerRow        = 0x68;  // FName: the CameraSetting row
constexpr std::uint32_t kOpStateFov      = 0x600;  // the blended state: FOV, Distance, Height
constexpr std::uint32_t kOpStateAim      = 0x620;  // AimOffset X, Y, Z
constexpr std::uint32_t kOpPosePitch     = 0x50;
constexpr std::uint32_t kOpPoseFov       = 0x5C;
constexpr std::uint32_t kOpPoseDistance  = 0x80;
constexpr std::uint32_t kOpPoseHeight    = 0x94;
constexpr std::uint32_t kOpPoseAim       = 0x84;   // the pose's AimOffset X, Y (lateral: the shoulder), Z
using facts::kOperatorModules;
constexpr std::uint32_t kModuleSlots     = 25;
constexpr std::uint32_t kModuleStride    = 16;
constexpr std::uint32_t kModuleActive    = 0x44;   // u8
constexpr float         kAimStepCm       = 15.0f;  // an aim offset change this large writes a line
constexpr std::uint64_t kMinChangeGapMs  = 250;
constexpr std::uint32_t kOpLevel         = 0x738;  // u8: the Camera Distance level
constexpr std::uint32_t kOpLevelBlending = 0x744;  // u8
constexpr std::uint32_t kOpPitchMin      = 0x748;
constexpr std::uint32_t kOpPitchMax      = 0x74C;

constexpr const wchar_t* kLayerNames[kLayerCount] = {L"Debug", L"Override", L"FieldAction", L"Chocobo",
                                                     L"InDoor", L"Location", L"Blend",    L"Default"};

// EndParam__Get_CameraLengthFieldScale{Length,Height,FOV}: a one-time key init, then the game's EndParam
// float read with default 0 (rcx unused). Entry: mov eax,28h; call __chkstk; sub rsp,rax.
constexpr std::uint8_t kScaleLengthBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD6, 0x97, 0xA1, 0x00, 0x48, 0x2B, 0xE0};
constexpr std::uint8_t kScaleHeightBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x92, 0x97, 0xA1, 0x00, 0x48, 0x2B, 0xE0};
constexpr std::uint8_t kScaleFovBytes[]    = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x4E, 0x97, 0xA1, 0x00, 0x48, 0x2B, 0xE0};
constexpr platform::FunctionFact kScaleFacts[] = {
    {L"EndParam CameraLengthFieldScaleLength", 0x141695750, kScaleLengthBytes},
    {L"EndParam CameraLengthFieldScaleHeight", 0x141695794, kScaleHeightBytes},
    {L"EndParam CameraLengthFieldScaleFOV", 0x1416957D8, kScaleFovBytes},
};

constexpr std::uint64_t kHeartbeatMs = 120000;

using ScaleFn = float (*)(void* unused);

struct Framing
{
    bool          op = false;
    std::uint8_t  initialized = 0, level = 0, blending = 0;
    std::uint8_t  active[kLayerCount]{};
    float         weight[kLayerCount]{};
    std::uint64_t row[kLayerCount]{};
    float         state[3]{}, aim[3]{};
    float         pitch = 0, poseFov = 0, poseDistance = 0, poseHeight = 0, pitchMin = 0, pitchMax = 0;
    float         poseAim[3]{};
    std::uint32_t modules = 0;  // bit per active module slot

    /// What a change line is written for.
    bool SameRowsAs(const Framing& other) const noexcept
    {
        if (op != other.op || initialized != other.initialized || level != other.level || modules != other.modules)
        {
            return false;
        }
        for (int a = 0; a < 3; ++a)
        {
            if (std::fabs(poseAim[a] - other.poseAim[a]) >= kAimStepCm)
            {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < kLayerCount; ++i)
        {
            if (active[i] != other.active[i] || (active[i] != 0 && row[i] != other.row[i]))
            {
                return false;
            }
        }
        return true;
    }
};

// Game thread.
ScaleFn  g_scale[3]    = {};
bool     g_scalesLogged = false;
bool     g_wasShown     = false;
std::uint64_t g_lastLineMs = 0;
Framing  g_last[2];

Framing Take(void* controller) noexcept
{
    Framing f;
    void* pcm = ReadPointer(controller, facts::kControllerCameraManager);
    void* op  = ReadPointer(pcm, facts::kCameraManagerFieldOperator);
    f.op      = op != nullptr;
    if (!f.op)
    {
        return f;
    }
    Read(op, kOpInitialized, f.initialized);
    Read(op, kOpLevel, f.level);
    Read(op, kOpLevelBlending, f.blending);
    for (std::uint32_t i = 0; i < kLayerCount; ++i)
    {
        const std::uint32_t layer = kOpLayers + kLayerStride * i;
        Read(op, layer + kLayerActive, f.active[i]);
        Read(op, layer + kLayerWeight, f.weight[i]);
        Read(op, layer + kLayerRow, f.row[i]);
    }
    Read(op, kOpStateFov, f.state);
    Read(op, kOpStateAim, f.aim);
    Read(op, kOpPosePitch, f.pitch);
    Read(op, kOpPoseFov, f.poseFov);
    Read(op, kOpPoseDistance, f.poseDistance);
    Read(op, kOpPoseHeight, f.poseHeight);
    Read(op, kOpPitchMin, f.pitchMin);
    Read(op, kOpPitchMax, f.pitchMax);
    Read(op, kOpPoseAim, f.poseAim);
    const auto* modules = static_cast<const std::byte*>(ReadPointer(op, kOperatorModules));
    for (std::uint32_t slot = 0; modules != nullptr && slot < kModuleSlots; ++slot)
    {
        const void*  module = ReadPointer(modules + slot * kModuleStride, 0);
        std::uint8_t on     = 0;
        if (Read(module, kModuleActive, on) && on != 0)
        {
            f.modules |= 1u << slot;
        }
    }
    return f;
}

std::wstring ModuleList(std::uint32_t mask)
{
    std::wstring list;
    for (std::uint32_t slot = 0; slot < kModuleSlots; ++slot)
    {
        if ((mask & (1u << slot)) != 0)
        {
            list += std::format(L"{}{}", list.empty() ? L"" : L" ", slot);
        }
    }
    return list.empty() ? std::wstring(L"none") : list;
}

std::wstring Describe(const Framing& f)
{
    if (!f.op)
    {
        return L"no Field operator";
    }
    std::wstring layers;
    for (std::uint32_t i = 0; i < kLayerCount; ++i)
    {
        if (f.active[i] != 0)
        {
            layers += std::format(L"{}{} {} (w {:.2f})", layers.empty() ? L"" : L", ", kLayerNames[i],
                                  players::unreal::NameText(f.row[i]), f.weight[i]);
        }
    }
    return std::format(L"init {} level {}{} | rows: {} | blended FOV {:.1f} distance {:.0f} height {:.0f} aim ({:.0f}, "
                       L"{:.0f}, {:.0f}) | pose FOV {:.1f} distance {:.0f} height {:.0f} aim ({:.0f}, {:.0f}, {:.0f}) pitch {:.1f} (limits "
                       L"{:.0f}..{:.0f}) | active modules {}",
                       f.initialized, f.level, f.blending != 0 ? L" (blending)" : L"", layers.empty() ? L"none" : layers,
                       f.state[0], f.state[1], f.state[2], f.aim[0], f.aim[1], f.aim[2], f.poseFov, f.poseDistance,
                       f.poseHeight, f.poseAim[0], f.poseAim[1], f.poseAim[2], f.pitch, f.pitchMin, f.pitchMax, ModuleList(f.modules));
}

void LogScales() noexcept
{
    g_scalesLogged = true;
    float k[3]     = {};
    bool  ok[3]    = {};
    for (int i = 0; i < 3; ++i)
    {
        ok[i] = g_scale[i] != nullptr && platform::CallGuarded(g_scale[i], k[i], static_cast<void*>(nullptr));
    }
    auto text = [&](int i) { return ok[i] ? std::format(L"{:.4f}", k[i]) : std::wstring(L"?"); };
    try
    {
        platform::Log(L"framing: the Camera Distance step per level above 1 (EndParam CameraLengthFieldScale): distance "
                      L"+{}, height +{}, FOV +{} (times the row's value)",
                      text(0), text(1), text(2));
    }
    catch (...)
    {
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    int found = 0;
    for (int i = 0; i < 3; ++i)
    {
        const auto address = platform::FindFunction(image, kScaleFacts[i]);
        g_scale[i]         = address ? reinterpret_cast<ScaleFn>(*address) : nullptr;
        found += address ? 1 : 0;
    }
    platform::Log(L"framing: watch installed (read-only); the Camera Distance step getters {}",
                  found == 3 ? L"checked" : L"NOT all found (their values are not logged)");
}

void Tick(bool splitShown) noexcept
{
    if (!generation::OnGameThread() || !generation::Active() || !splitShown)
    {
        g_wasShown = false;
        return;
    }
    if (!g_scalesLogged)
    {
        LogScales();
    }
    if (!platform::Enabled(platform::Level::Debug))
    {
        g_wasShown = false;  // the watch's lines are Debug: read nothing; a raised level starts with a full line
        return;
    }
    void* const controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    Framing     now[2];
    const bool  wasShown = g_wasShown;
    bool        changedOne[2];
    for (int p = 0; p < 2; ++p)
    {
        now[p]        = Take(controllers[p]);
        changedOne[p] = !g_wasShown || !now[p].SameRowsAs(g_last[p]);
    }
    const bool changed = changedOne[0] || changedOne[1];
    g_wasShown = true;
    const std::uint64_t nowMs = GetTickCount64();
    if ((!changed && nowMs - g_lastLineMs < kHeartbeatMs) || (changed && wasShown && nowMs - g_lastLineMs < kMinChangeGapMs))
    {
        return;
    }
    g_lastLineMs = nowMs;
    try
    {
        for (int p = 0; p < 2; ++p)
        {
            if (changed && !changedOne[p])
            {
                continue;  // a change line for the player whose framing changed; the heartbeat has both
            }
            platform::LogDebug(L"framing: player {} {}: {}", p + 1, changed ? L"changed" : L"now", Describe(now[p]));
            g_last[p] = now[p];
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::framing_watch
