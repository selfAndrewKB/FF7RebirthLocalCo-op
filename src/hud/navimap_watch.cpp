#include "hud/navimap_watch.h"

#include "platform/guarded.h"
#include "platform/log.h"
#include "players/unreal_access.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::navimap_watch {

namespace {

// RE-p2-own-minimap.md (this build).
constexpr std::uintptr_t kEndUISystem         = 0x1493496E0;  // g_EndUISystem (the object itself)
constexpr std::uint32_t  kAreaMapManager      = 0x108;
constexpr std::uint32_t  kNaviMapHolder       = 0x340;
constexpr std::uint32_t  kHolderInterface     = 0x20;   // the widget's native interface (weak pointer follows)
constexpr std::uint32_t  kHolderAutoRotate    = 0x178;  // u8: rotation from the camera (else +0x198)
constexpr std::uint32_t  kHolderFlag179       = 0x179;  // u8: gates the +0xC8/+0xD0 calls
constexpr std::uint32_t  kHolderYawOffset     = 0x194;  // float, added to the camera yaw
constexpr std::uint32_t  kHolderFixedRotation = 0x198;  // float
constexpr std::uint32_t  kInterfaceInObject   = 0x108;  // UEndNaviMapCircle/Octagon: interface at +0x108
constexpr std::uint32_t  kObjectRenderTexture = 0x128;  // UEndNaviMapCircle.RenderTexture (a Blueprint asset, or null)
constexpr std::uint32_t  kObjectSlateWidget   = 0x138;  // TSharedPtr<SEndNaviMapCircle>: object pointer first
constexpr std::uint32_t  kSlatePaintInterface = 0x2B0;  // its vtable says circle or octagon
constexpr std::uint32_t  kSlateContent        = 0x3B0;  // SEndNaviMap*
constexpr std::uint32_t  kSlateTarget         = 0x490;  // UTextureRenderTarget2D*
constexpr std::uintptr_t kCirclePaintVTable   = 0x14676E5D8;
constexpr std::uintptr_t kOctagonPaintVTable  = 0x14676E860;
// SEndNaviMap:
constexpr std::uint32_t kContentCentre       = 0x478;  // float x, y, z
constexpr std::uint32_t kContentPixelPerCm   = 0x484;
constexpr std::uint32_t kContentMapRotation  = 0x488;  // radians
constexpr std::uint32_t kContentViewRotation = 0x48C;  // radians
// UTextureRenderTarget2D (stock 4.26) and its resource:
constexpr std::uint32_t kTargetFlags91   = 0x91;
constexpr std::uint32_t kTargetGamma     = 0xE0;
constexpr std::uint32_t kTargetSizeX     = 0xE8;
constexpr std::uint32_t kTargetSizeY     = 0xEC;
constexpr std::uint32_t kTargetFormat    = 0x108;
constexpr std::uint32_t kTargetResource  = 0xA8;
constexpr std::uint32_t kResourceTexture = 0x10;  // the texture RHI the outer element samples
constexpr std::uint32_t kResourceTarget  = 0x90;  // the render-target RHI pool 0's consume binds
// UObjectBase:
constexpr std::uint32_t kObjectName  = 0x18;
constexpr std::uint32_t kObjectOuter = 0x20;

constexpr std::uint64_t kHeartbeatMs = 60000;
constexpr int           kMaxLines    = 300;

struct State
{
    void*         holder = nullptr;
    void*         object = nullptr;  // UEndNaviMapCircle/Octagon
    void*         slate  = nullptr;
    const wchar_t* kind  = L"none";
    void*         target = nullptr;
    void*         renderTexture = nullptr;
    std::uint8_t  autoRotate = 0, flag179 = 0;

    bool SameAs(const State& o) const noexcept
    {
        return holder == o.holder && object == o.object && slate == o.slate && kind == o.kind && target == o.target &&
               renderTexture == o.renderTexture && autoRotate == o.autoRotate && flag179 == o.flag179;
    }
};

// Set at install, then read (game thread).
std::uintptr_t g_system        = 0;
std::uintptr_t g_circleVTable  = 0;
std::uintptr_t g_octagonVTable = 0;
bool           g_installed     = false;
State          g_last;
bool           g_seen          = false;
std::uint64_t  g_lastLineMs    = 0;
int            g_lines         = 0;

template <class T>
bool Read(const void* object, std::uint32_t offset, T& out) noexcept
{
    return object != nullptr && platform::CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

void* Pointer(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    Read(object, offset, value);
    return value;
}

/// "Outer.Name" from the object's own FNames (guarded reads).
std::wstring ObjectText(const void* object)
{
    if (object == nullptr)
    {
        return L"null";
    }
    std::uint64_t name = 0, outerName = 0;
    Read(object, kObjectName, name);
    const void* outer = Pointer(object, kObjectOuter);
    Read(outer, kObjectName, outerName);
    return std::format(L"{}.{} ({:#x})", outer != nullptr ? players::unreal::NameText(outerName) : L"-",
                       players::unreal::NameText(name), reinterpret_cast<std::uintptr_t>(object));
}

State Take() noexcept
{
    State s;
    void* manager = nullptr;
    platform::CopyGuarded(&manager, reinterpret_cast<const void*>(g_system + kAreaMapManager), sizeof(manager));
    s.holder = Pointer(manager, kNaviMapHolder);
    if (s.holder == nullptr)
    {
        return s;
    }
    Read(s.holder, kHolderAutoRotate, s.autoRotate);
    Read(s.holder, kHolderFlag179, s.flag179);
    const auto iface = reinterpret_cast<std::uintptr_t>(Pointer(s.holder, kHolderInterface));
    if (iface == 0)
    {
        return s;
    }
    s.object = reinterpret_cast<void*>(iface - kInterfaceInObject);
    s.slate  = Pointer(s.object, kObjectSlateWidget);
    s.renderTexture = Pointer(s.object, kObjectRenderTexture);
    const auto vtable = reinterpret_cast<std::uintptr_t>(Pointer(s.slate, kSlatePaintInterface));
    s.kind   = s.slate == nullptr ? L"no Slate widget"
               : vtable == g_circleVTable  ? L"circle"
               : vtable == g_octagonVTable ? L"octagon"
                                           : L"unknown kind";
    s.target = Pointer(s.slate, kSlateTarget);
    return s;
}

std::wstring TargetText(const State& s)
{
    std::int32_t  sizeX = 0, sizeY = 0;
    std::uint8_t  format = 0, flags91 = 0;
    float         gamma = 0.0f;
    Read(s.target, kTargetSizeX, sizeX);
    Read(s.target, kTargetSizeY, sizeY);
    Read(s.target, kTargetFormat, format);
    Read(s.target, kTargetFlags91, flags91);
    Read(s.target, kTargetGamma, gamma);
    void* resource = Pointer(s.target, kTargetResource);
    return std::format(L"target {} {}x{} format {} gamma {:.2f} +0x91 {:#x}, resource {:#x} (texture RHI {:#x}, target RHI "
                       L"{:#x}); the Blueprint's RenderTexture {}",
                       ObjectText(s.target), sizeX, sizeY, format, gamma, flags91, reinterpret_cast<std::uintptr_t>(resource),
                       reinterpret_cast<std::uintptr_t>(Pointer(resource, kResourceTexture)),
                       reinterpret_cast<std::uintptr_t>(Pointer(resource, kResourceTarget)),
                       s.renderTexture == nullptr ? std::wstring(L"null (the widget made its own target)")
                                                  : ObjectText(s.renderTexture));
}

std::wstring ContentText(const State& s)
{
    void* content = Pointer(s.slate, kSlateContent);
    float centre[3]{}, pixelPerCm = 0, mapRotation = 0, viewRotation = 0, yawOffset = 0, fixedRotation = 0;
    Read(content, kContentCentre, centre);
    Read(content, kContentPixelPerCm, pixelPerCm);
    Read(content, kContentMapRotation, mapRotation);
    Read(content, kContentViewRotation, viewRotation);
    Read(s.holder, kHolderYawOffset, yawOffset);
    Read(s.holder, kHolderFixedRotation, fixedRotation);
    constexpr float kDegrees = 57.2957795f;
    return std::format(L"centre ({:.0f}, {:.0f}, {:.0f}) pixel/cm {:.4f} map rotation {:.1f} deg view rotation {:.1f} deg; "
                       L"holder auto-rotate {} +0x179 {} yaw offset {:.1f} fixed rotation {:.1f}",
                       centre[0], centre[1], centre[2], pixelPerCm, mapRotation * kDegrees, viewRotation * kDegrees,
                       s.autoRotate, s.flag179, yawOffset, fixedRotation);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto system  = image.Rebase(kEndUISystem, kAreaMapManager + sizeof(void*));
    const auto circle  = image.Rebase(kCirclePaintVTable, sizeof(void*));
    const auto octagon = image.Rebase(kOctagonPaintVTable, sizeof(void*));
    g_installed        = system && circle && octagon;
    if (g_installed)
    {
        g_system        = *system;
        g_circleVTable  = *circle;
        g_octagonVTable = *octagon;
    }
    platform::Log(L"minimap watch: {}", g_installed ? L"installed (read-only)" : L"UNAVAILABLE (an address check failed)");
}

void Tick(bool splitShown) noexcept
{
    if (!g_installed || g_lines >= kMaxLines || !platform::Enabled(platform::Level::Debug))
    {
        return;  // a read-only watch: nothing to read when its lines are not written
    }
    const State now     = Take();
    const bool  changed = !g_seen || !now.SameAs(g_last);
    const std::uint64_t nowMs = GetTickCount64();
    const bool  heartbeat = splitShown && now.slate != nullptr && nowMs - g_lastLineMs >= kHeartbeatMs;
    if (!changed && !heartbeat)
    {
        return;
    }
    g_seen       = true;
    g_last       = now;
    g_lastLineMs = nowMs;
    ++g_lines;
    try
    {
        if (now.holder == nullptr)
        {
            platform::LogDebug(L"minimap watch: no navimap holder");
        }
        else if (now.slate == nullptr)
        {
            platform::LogDebug(L"minimap watch: holder {:#x}, no minimap widget registered (compass areas)",
                          reinterpret_cast<std::uintptr_t>(now.holder));
        }
        else if (changed)
        {
            platform::LogDebug(L"minimap watch: {} widget {}, Slate {:#x}; {}; {}", now.kind, ObjectText(now.object),
                          reinterpret_cast<std::uintptr_t>(now.slate), TargetText(now), ContentText(now));
        }
        else
        {
            platform::LogDebug(L"minimap watch: {}", ContentText(now));
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::hud::navimap_watch
