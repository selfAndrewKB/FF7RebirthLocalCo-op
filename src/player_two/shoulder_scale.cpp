#include "player_two/shoulder_scale.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "player_two/world_rules.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "views/player_view_point.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::player_two::shoulder_scale {

namespace {

// FEndModularCameraOperator__UpdateModules_Vt30: `mov rcx,rbx (op) ; lea rdx,[rbp+67h] ; vmovaps xmm2,xmm10 (dt) ;
// call FEndModularCameraOperator__FinalizePose` at 0x140A61C20 (bytes E8 37 B0 03 00, one 16-byte block), after
// the modules' ApplyToPose. FinalizePose(op, stick, dt) applies the stick and computes the location from the pose
// at op+0x40 (checked by disassembly, 2026-10-01).
constexpr std::uintptr_t kFinalizeCall = 0x140A61C20;
constexpr std::uintptr_t kFinalizePose = 0x140A9CC5C;
constexpr std::uint8_t   kFinalizePoseBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                 0xC8, 0x22, 0x61, 0x01, 0x48, 0x2B, 0xE0};
constexpr std::uint32_t  kPoseAimY        = 0x88;  // op+0x40 pose, +0x48 AimOffset Y (lateral)
constexpr std::uint32_t  kBattleOperator  = 0x2CA8;
constexpr float          kMinRatio = 0.25f, kMaxRatio = 4.0f;  // a ratio outside this is not trusted
constexpr float          kMaxScale = 1.0f;  // only ever softened: a wider half keeps the game's offset (the user, 2026-10-01)

using FinalizeFn = void (*)(void* op, void* stick, float deltaTime);

platform::CallRedirect g_call;
bool                   g_installed = false;
std::atomic<bool>      g_on{true};
std::atomic<float>     g_minimum{0.7f};  // the user's pick, 2026-10-01 (side by side ×0.7 rather than the width's ×0.5)
std::atomic<bool>      g_shown{false};
float                  g_logged[2] = {0.0f, 0.0f};

/// Player p's factor: the half's width against the camera's, kept within the minimum and 1 (only ever softened).
float Scale(int p) noexcept
{
    const float ratio = views::SplitWidthRatio(p == 1);
    return world_rules::ShoulderScale(ratio, g_minimum.load(std::memory_order_relaxed), kMaxScale);
}

/// 0 or 1 for an operator of player 1's or player 2's camera manager, -1 otherwise.
int PlayerOf(const void* op) noexcept
{
    void* const controllers[2] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    for (int p = 0; p < 2; ++p)
    {
        void* pcm = ReadPointer(controllers[p], facts::kControllerCameraManager);
        if (pcm != nullptr && (ReadPointer(pcm, facts::kCameraManagerFieldOperator) == op || ReadPointer(pcm, kBattleOperator) == op))
        {
            return p;
        }
    }
    return -1;
}

void Finalize(void* op, void* stick, float deltaTime) noexcept
{
    if (g_on.load(std::memory_order_relaxed) && g_shown.load(std::memory_order_relaxed) && generation::Active() &&
        generation::OnGameThread())
    {
        const int p = PlayerOf(op);
        if (p >= 0)
        {
            const float ratio = Scale(p);
            auto*       aimY  = reinterpret_cast<std::byte*>(op) + kPoseAimY;
            float       value = 0.0f;
            if (ratio >= kMinRatio && ratio <= kMaxRatio && platform::CopyGuarded(&value, aimY, sizeof(value)) &&
                std::isfinite(value))
            {
                value *= ratio;
                std::memcpy(aimY, &value, sizeof(value));
            }
        }
    }
    reinterpret_cast<FinalizeFn>(g_call.Original())(op, stick, deltaTime);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto site   = image.Rebase(kFinalizeCall, 5);
    const auto target = image.Rebase(kFinalizePose);
    g_installed       = site && target && image.BytesMatch(kFinalizePose, kFinalizePoseBytes) &&
                  g_call.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&Finalize));
    platform::Log(L"shoulder scale: {}", g_installed ? L"each half's camera shoulder offset scaled to its width (on)"
                                                     : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Remove() noexcept
{
    if (g_call.IsInstalled() && !g_call.Restore())
    {
        platform::Log(L"shoulder scale: the call was changed after us; left in place (it passes through when off)");
    }
    g_installed = false;
}

bool Installed() noexcept
{
    return g_installed;
}

void SetOn(bool on) noexcept
{
    g_on.store(on, std::memory_order_relaxed);
    g_logged[0] = g_logged[1] = 0.0f;
    platform::Log(L"shoulder scale: {}", on ? L"on" : L"off (the game's offset as it is)");
}

void SetMinimum(float minimum) noexcept
{
    g_minimum.store(std::fmin(std::fmax(minimum, 0.25f), 1.0f), std::memory_order_relaxed);
    g_logged[0] = g_logged[1] = 0.0f;
    platform::Log(L"shoulder scale: at least x{:.2f}", g_minimum.load(std::memory_order_relaxed));
}

void Poll(bool splitShown) noexcept
{
    g_shown.store(splitShown, std::memory_order_relaxed);
    if (!g_installed || !splitShown || !g_on.load(std::memory_order_relaxed))
    {
        return;
    }
    for (int p = 0; p < 2; ++p)
    {
        const float ratio = Scale(p);
        if (std::fabs(ratio - g_logged[p]) > 0.01f)
        {
            g_logged[p] = ratio;
            platform::Log(L"shoulder scale: player {}'s camera shoulder offset x{:.2f} (its half's width against the camera's, "
                          L"x{:.2f}, kept within {:.2f}..1)",
                          p + 1, ratio, views::SplitWidthRatio(p == 1), g_minimum.load(std::memory_order_relaxed));
        }
    }
}

}  // namespace lss::player_two::shoulder_scale
