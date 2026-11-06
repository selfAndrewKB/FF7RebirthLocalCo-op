#include "camera/camera_distance.h"

#include "camera/camera_access.h"
#include "facts/cameras.h"
#include "facts/controller.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/call_site.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/split_settings_access.h"
#include "world/world_rules.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::camera::camera_distance {

namespace {

// Facts read from the database (RE-p2-battle-camera.md §6; this build).
constexpr std::uint8_t kPrimaryApplyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                               0x34, 0x5A, 0xCB, 0xFE, 0x48, 0x2B, 0xE0, 0x8A, 0xDA, 0x40, 0x8A, 0xF9};
constexpr platform::FunctionFact kPrimaryApply{L"PrimaryPCM_ApplyCameraDistanceOptions", 0x1433F94EC, kPrimaryApplyBytes};
constexpr std::uint8_t kManagerApplyBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xE2, 0x5B, 0x37, 0xFE, 0x48, 0x2B,
                                               0xE0, 0x4C, 0x8B, 0xD1, 0x45, 0x8A, 0xD8, 0x48, 0x8B, 0x89, 0xB0, 0x2C};
constexpr platform::FunctionFact kManagerApply{L"AEndPlayerCameraManager__ApplyCameraDistanceOptions", 0x143D39344,
                                               kManagerApplyBytes};
constexpr std::uintptr_t kSetterCall  = 0x142ADCBCA;  // in the option setter: the primary camera manager's apply
constexpr std::uintptr_t kConfirmCall = 0x14180A4D6;  // in the confirm-key handler: the per-camera-manager apply
// 1.005 (RE-1005-field-2.md §7): the setter was merged into OptionsMenu_SetOptionValue_CopyApplyCommit and its call is
// at 0x1410B9F6D, %16 = 13, so it crosses a 16-byte block. There it is heard at the primary apply's entry instead
// (first instruction `mov [rsp+8], rbx`, 5 bytes at %16 = 4; that call is its only caller), by its return address.
constexpr std::size_t kPrimaryApplyFirstInstruction = 5;

// Split framing (RE-p2-framing.md §1.4, §3, §6.1; RE-p2-battle-camera.md §6).
// GetSystemOptionData(): the global option struct (pure read: [[[g]+0x100]+0x28]+0x10).
constexpr std::uint8_t kOptionDataBytes[] = {0x48, 0x8B, 0x05, 0xF5, 0xCB, 0xB1, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x10,
                                             0x48, 0x8B, 0x80, 0x00, 0x01, 0x00, 0x00};
constexpr platform::FunctionFact kOptionData{L"GetSystemOptionData", 0x14087F15C, kOptionDataBytes};
constexpr std::uint32_t kOptionFieldLevel  = 0x36;  // Camera Distance: Out of Battle
constexpr std::uint32_t kOptionBattleLevel = 0x37;  // Camera Distance: In Battle
constexpr std::uint32_t kOpInitialized     = 0xD2;  // the operator's lazy init ran (it seeds the level from the option)
constexpr std::uint32_t kOpLevel           = 0x738;
constexpr std::int32_t  kMinOffset = -2, kMaxOffset = 5;
constexpr std::int32_t  kMinLevel  = 1, kMaxLevel = 10;  // the factor is linear; the menu's own range is not read
constexpr ULONGLONG     kLayoutReadMs = 1000;
constexpr std::uint8_t  kThreeColumns = 2;  // g_layout's value for three columns: no offset (each a full 16:9 view)
constexpr int           kMaxFramingLines = 200;

using OptionDataFn = const std::uint8_t* (*)();

using PrimaryApplyFn = void (*)(std::uint8_t field, std::uint8_t battle);
using ManagerApplyFn = void (*)(void* cameraManager, std::uint8_t field, std::uint8_t battle);

// Set once at install.
platform::CallRedirect g_setterCall;
platform::CallRedirect g_confirmCall;
platform::EntryRedirect g_primaryEntry;      // 1.005: the option setter's call, heard at the entry
std::uintptr_t          g_setterReturn = 0;  // 1.005: live return address of the option setter's call
ManagerApplyFn         g_managerApply        = nullptr;

// Game thread.

// Split framing, game thread.
OptionDataFn  g_optionData     = nullptr;
std::int32_t  g_offsets[2]     = {0, 0};  // top/bottom, side by side: the Options level unless asked
std::uint8_t  g_layout         = 0;       // players::SplitLayoutKind, read once a second while in; kThreeColumns
ULONGLONG     g_layoutReadAt   = 0;
bool          g_wasIn          = false;
int           g_framingLines   = 0;

void Line(const std::wstring& text) noexcept
{
    platform::Log(L"camera distance: {}", text);
}

using camera_access::CameraManagerOf;

/// The game's per-camera-manager apply for each further player's camera manager, after the game's own apply on
/// `applied` (the primary camera manager).
void ApplyToSeats(const void* applied, std::uint8_t field, std::uint8_t battle) noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* manager = players::generation::Active(seat) ? CameraManagerOf(players::generation::ControllerOf(seat)) : nullptr;
        if (manager == nullptr || manager == applied)
        {
            continue;
        }
        const bool ok = platform::CallGuarded(g_managerApply, manager, field, battle);
        Line(std::format(L"field {} battle {} applied to {}'s camera manager too{}", field, battle,
                         players::SeatName(seat), ok ? L"" : L" (FAULTED)"));
    }
}

/// Replaces the option setter's call of the primary camera manager's apply.
void PrimaryApply(std::uint8_t field, std::uint8_t battle) noexcept
{
    reinterpret_cast<PrimaryApplyFn>(g_setterCall.Original())(field, battle);
    ApplyToSeats(CameraManagerOf(players::generation::PlayerOneController()), field, battle);
}

/// 1.005: every call of the primary camera manager's apply, at its entry; the option setter's (by its return address)
/// reaches player 2's camera manager too, exactly as PrimaryApply does.
void PrimaryApplyEntry(std::uint8_t field, std::uint8_t battle) noexcept
{
    const bool setter = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_setterReturn;
    reinterpret_cast<PrimaryApplyFn>(g_primaryEntry.Original())(field, battle);
    if (setter)
    {
        ApplyToSeats(CameraManagerOf(players::generation::PlayerOneController()), field, battle);
    }
}

/// The option setter's call: redirected on the research build; on 1.005, heard at the primary apply's entry.
bool InstallSetter(const platform::GameImage& image, std::uintptr_t primary) noexcept
{
    const auto site = image.Rebase(kSetterCall, 5);
    if (!site)
    {
        return false;
    }
    if (platform::RunningBuild() != platform::BuildIndex::V1005)
    {
        return g_setterCall.Install(*site, primary, reinterpret_cast<std::uintptr_t>(&PrimaryApply));
    }
    if (!players::call_site::Calls(*site, primary))
    {
        return false;
    }
    g_setterReturn   = *site + 5;  // before the install: the replacement runs the moment it is reachable
    const auto first = std::span(kPrimaryApplyBytes).first(kPrimaryApplyFirstInstruction);
    const bool ok    = g_primaryEntry.Install(primary, image.EntryInstruction(kPrimaryApply.address, first),
                                              reinterpret_cast<std::uintptr_t>(&PrimaryApplyEntry));
    g_setterReturn   = ok ? g_setterReturn : 0;
    return ok;
}

/// Replaces the confirm-key handler's call of the per-camera-manager apply (on the primary one).
void ManagerApply(void* cameraManager, std::uint8_t field, std::uint8_t battle) noexcept
{
    reinterpret_cast<ManagerApplyFn>(g_confirmCall.Original())(cameraManager, field, battle);
    ApplyToSeats(cameraManager, field, battle);
}

template <class... Args>
void FramingLine(std::wformat_string<Args...> text, Args&&... args) noexcept
{
    if (g_framingLines < kMaxFramingLines)
    {
        ++g_framingLines;
        try
        {
            platform::Log(L"framing: {}", std::format(text, std::forward<Args>(args)...));
        }
        catch (...)
        {
        }
    }
}

/// The options' field and battle levels, if the option data can be read.
bool OptionLevels(std::uint8_t& field, std::uint8_t& battle) noexcept
{
    const std::uint8_t* options = nullptr;
    return g_optionData != nullptr && platform::CallGuarded(g_optionData, options) && options != nullptr &&
           platform::CopyGuarded(&field, options + kOptionFieldLevel, 1) &&
           platform::CopyGuarded(&battle, options + kOptionBattleLevel, 1);
}

/// Drives one camera manager's field operator to level through the game's per-camera-manager setter,
/// if the operator is initialised and holds another level.
void DriveTo(void* cameraManager, int seat, std::uint8_t level, std::uint8_t battle) noexcept
{
    void* op = ReadPointer(cameraManager, facts::kCameraManagerFieldOperator);
    std::uint8_t initialized = 0, now = 0;
    if (op == nullptr || !platform::CopyGuarded(&initialized, static_cast<std::uint8_t*>(op) + kOpInitialized, 1) ||
        initialized == 0 || !platform::CopyGuarded(&now, static_cast<std::uint8_t*>(op) + kOpLevel, 1) || now == level)
    {
        return;
    }
    const bool ok = platform::CallGuarded(g_managerApply, cameraManager, level, battle);
    FramingLine(L"{}'s field camera distance level {} -> {} (battle {}){}", players::SeatName(seat), now, level,
                battle, ok ? L"" : L" (FAULTED)");
}

}  // namespace

bool FramingAvailable() noexcept
{
    return g_managerApply != nullptr && g_optionData != nullptr && camera_access::Installed();
}

void SetLayoutOffsets(std::int32_t topBottom, std::int32_t sideBySide) noexcept
{
    g_offsets[0] = std::clamp(topBottom, kMinOffset, kMaxOffset);
    g_offsets[1] = std::clamp(sideBySide, kMinOffset, kMaxOffset);
    FramingLine(L"layout offsets: top/bottom {:+}, side by side {:+} steps of the Options level", g_offsets[0], g_offsets[1]);
}

std::int32_t LayoutOffset(int layout) noexcept
{
    return layout == 0 || layout == 1 ? g_offsets[layout] : 0;
}

void TickFraming(int columns) noexcept
{
    if (!FramingAvailable() || !players::generation::OnGameThread())
    {
        return;
    }
    const bool in = columns >= 2 && players::generation::AnyActive();
    if (!in && !g_wasIn)
    {
        return;
    }
    std::uint8_t field = 0, battle = 0;
    if (!OptionLevels(field, battle))
    {
        return;
    }
    if (!in)
    {
        g_wasIn = false;  // the last further player left: player 1's camera back to the Options level
        DriveTo(CameraManagerOf(players::generation::PlayerOneController()), 0, field, battle);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (columns >= 3)
    {
        g_layout = kThreeColumns;
    }
    else if (!g_wasIn || g_layout == kThreeColumns || now - g_layoutReadAt >= kLayoutReadMs)
    {
        g_layoutReadAt = now;
        if (const auto layout = players::split_settings::TwoPlayerLayout(); layout && *layout <= 1)
        {
            g_layout = *layout;
        }
    }
    g_wasIn = true;
    const std::int32_t offset = LayoutOffset(g_layout);
    const std::int32_t target = world::world_rules::DistanceLevel(static_cast<std::int32_t>(field), offset, kMinLevel, kMaxLevel);
    const std::uint8_t level  = static_cast<std::uint8_t>(target);
    DriveTo(CameraManagerOf(players::generation::PlayerOneController()), 0, level, battle);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat))
        {
            DriveTo(CameraManagerOf(players::generation::ControllerOf(seat)), seat, level, battle);
        }
    }
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto primary = platform::FindFunction(image, kPrimaryApply);
    const auto manager = platform::FindFunction(image, kManagerApply);
    if (!primary || !manager || !camera_access::Install(image))
    {
        platform::Log(L"camera distance: UNAVAILABLE (a check failed); player 2's camera keeps the distance it started "
                      L"with");
        return false;
    }
    g_managerApply        = reinterpret_cast<ManagerApplyFn>(*manager);
    const auto options    = platform::FindFunction(image, kOptionData);
    g_optionData          = options ? reinterpret_cast<OptionDataFn>(*options) : nullptr;
    const bool setter  = InstallSetter(image, *primary);
    const bool confirm = g_confirmCall.Install(image, kConfirmCall, *manager,
                                               reinterpret_cast<std::uintptr_t>(&ManagerApply));
    platform::Log(L"camera distance: the Options distance reaches player 2's camera from the option setter {} and the "
                  L"confirm key {}",
                  setter ? L"yes" : L"NO (check failed)", confirm ? L"yes" : L"NO (check failed)");
    platform::Log(L"framing: split framing through the per-camera-manager distance setter {} (top/bottom {:+}, side by side {:+} steps)",
                  FramingAvailable() ? L"available" : L"UNAVAILABLE (the option data check failed)", g_offsets[0], g_offsets[1]);
    return setter || confirm;
}

void Remove() noexcept
{
    for (auto* call : {&g_setterCall, &g_confirmCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"camera distance: a call was changed after us; left in place (it passes through)");
        }
    }
    if (g_primaryEntry.IsInstalled() && !g_primaryEntry.Restore())
    {
        platform::Log(L"camera distance: the primary apply's entry was changed after us; left in place (it passes through)");
    }
}

#if LSS_DEV

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    // The two layouts' framing offsets, one entry each (one argument per command): each sets its own and keeps the
    // other layout's as it is.
    for (const int layout : {0, 1})
    {
        control::Register({.name  = layout == 0 ? L"framing-offset.top-bottom" : L"framing-offset.side-by-side",
                           .shape = control::Shape::Integer,
                           .min   = -2,
                           .max   = 5,
                           .help  = layout == 0 ? L"field-camera distance steps added to the Options level while "
                                                  L"player 2 is in and the screen splits top and bottom (0 at start)"
                                                : L"field-camera distance steps added to the Options level while "
                                                  L"player 2 is in and the screen splits side by side (0 at start)",
                           .set   = [layout](const Value& v) {
                               if (!FramingAvailable())
                               {
                                   return Outcome{false, L"split framing is not available (see the install log)"};
                               }
                               const auto steps = static_cast<std::int32_t>(std::get<std::int64_t>(v));
                               SetLayoutOffsets(layout == 0 ? steps : LayoutOffset(0),
                                                layout == 1 ? steps : LayoutOffset(1));
                               return Outcome{true, L"each player's field camera follows the Options distance plus "
                                                    L"the layout's steps while player 2 is in"};
                           },
                           .get = [layout] { return Value{std::int64_t{LayoutOffset(layout)}}; }});
    }
}

#endif

}  // namespace lss::camera::camera_distance
