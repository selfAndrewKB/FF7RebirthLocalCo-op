#include "player_two/camera/shoulder_scale.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/camera/camera_access.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/world/world_rules.h"
#include "players/seats.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <array>
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
constexpr std::uint8_t   kFinalizePoseBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                 0xC8, 0x22, 0x61, 0x01, 0x48, 0x2B, 0xE0};
constexpr platform::FunctionFact kFinalizePose{L"the camera operator's FinalizePose", 0x140A9CC5C, kFinalizePoseBytes};
constexpr std::uint32_t  kPoseAimY        = 0x88;  // op+0x40 pose, +0x48 AimOffset Y (lateral)
constexpr float          kMinRatio = 0.25f, kMaxRatio = 4.0f;  // a ratio outside this is not trusted
constexpr float          kMaxScale = 1.0f;  // only ever softened: a wider half keeps the game's offset (the user, 2026-10-01)

using FinalizeFn = void (*)(void* op, void* stick, float deltaTime);

platform::CallRedirect g_call;
bool                   g_installed = false;
std::atomic<bool>      g_on{true};
std::atomic<float>     g_minimum{0.7f};  // the user's pick, 2026-10-01 (side by side ×0.7 rather than the width's ×0.5)
std::atomic<bool>      g_shown{false};
std::array<float, players::kMaxPlayers>              g_logged{};  // by seat (players/seats.h)
static_assert(players::kMaxPlayers == 3, "one ratio per player below");
std::array<std::atomic<float>, players::kMaxPlayers> g_ratio{1.0f, 1.0f, 1.0f};  // each view's width against its camera's


/// Seat p's factor: its view's width against the camera's, kept within the minimum and 1 (only ever softened).
float Scale(int p) noexcept
{
    const float ratio = g_ratio[static_cast<std::size_t>(p)].load(std::memory_order_relaxed);
    return world_rules::ShoulderScale(ratio, g_minimum.load(std::memory_order_relaxed), kMaxScale);
}

void Finalize(void* op, void* stick, float deltaTime) noexcept
{
    if (g_on.load(std::memory_order_relaxed) && g_shown.load(std::memory_order_relaxed) && generation::AnyActive() &&
        generation::OnGameThread())
    {
        const int p = camera_access::SeatOfOperator(op);  // player 1's, or a built seat's while it is active
        if (p >= 0 && p < players::kMaxPlayers)
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
    const auto target = platform::FindFunction(image, kFinalizePose);
    g_installed       = site && target &&
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
    g_logged = {};
    platform::Log(L"shoulder scale: {}", on ? L"on" : L"off (the game's offset as it is)");
}

bool On() noexcept
{
    return g_on.load(std::memory_order_relaxed);
}

void SetMinimum(float minimum) noexcept
{
    g_minimum.store(std::fmin(std::fmax(minimum, 0.25f), 1.0f), std::memory_order_relaxed);
    g_logged = {};
    platform::Log(L"shoulder scale: at least x{:.2f}", g_minimum.load(std::memory_order_relaxed));
}

float Minimum() noexcept
{
    return g_minimum.load(std::memory_order_relaxed);
}

void Poll(bool splitShown, const std::array<float, players::kMaxPlayers>& ratios) noexcept
{
    for (std::size_t p = 0; p < ratios.size(); ++p)
    {
        g_ratio[p].store(ratios[p], std::memory_order_relaxed);
    }
    g_shown.store(splitShown, std::memory_order_relaxed);
    if (!g_installed || !splitShown || !g_on.load(std::memory_order_relaxed))
    {
        return;
    }
    for (int p = 0; p <= players::kSeatsBuilt; ++p)
    {
        const float ratio = Scale(p);
        if (std::fabs(ratio - g_logged[static_cast<std::size_t>(p)]) > 0.01f)
        {
            g_logged[static_cast<std::size_t>(p)] = ratio;
            platform::Log(L"shoulder scale: {}'s camera shoulder offset x{:.2f} (its view's width against the camera's, "
                          L"x{:.2f}, kept within {:.2f}..1)",
                          players::SeatName(p), ratio, g_ratio[static_cast<std::size_t>(p)].load(std::memory_order_relaxed),
                          g_minimum.load(std::memory_order_relaxed));
        }
    }
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"shoulder-scale",
                       .shape = control::Shape::OnOff,
                       .help  = L"each half's camera shoulder offset scaled to its width (on at start)",
                       .set   = [](const Value& v) {
                           if (!Installed())
                           {
                               return Outcome{false, L"the shoulder scale is not installed (see the install log)"};
                           }
                           const bool on = std::get<bool>(v);
                           SetOn(on);
                           return Outcome{true, on ? L"each half's camera shoulder offset scaled to its width"
                                                   : L"the game's shoulder offset as it is"};
                       },
                       .get = [] { return Value{Installed() && On()}; }});
    control::Register({.name     = L"shoulder-scale-min",
                       .shape    = control::Shape::Float,
                       .minFloat = 0.25,
                       .maxFloat = 1.0,
                       .help     = L"the smallest shoulder factor (0.7 at start; side by side's width alone gives "
                                   L"0.5)",
                       .set      = [](const Value& v) {
                           if (!Installed())
                           {
                               return Outcome{false, L"the shoulder scale is not installed (see the install log)"};
                           }
                           SetMinimum(static_cast<float>(std::get<double>(v)));
                           return Outcome{true, L"the narrowest half's shoulder offset is softened no further than "
                                                L"this"};
                       },
                       .get = [] { return Value{control::FloatReading(Minimum())}; }});
}

#endif

}  // namespace lss::player_two::shoulder_scale
