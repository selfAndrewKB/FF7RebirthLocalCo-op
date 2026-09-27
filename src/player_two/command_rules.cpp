#include "player_two/command_rules.h"

#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/command_rules_code.h"
#include "player_two/party_member.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two::command_rules {

namespace {

// Facts read from the database (RE-p2-command-menu.md §4.2, §4.4, §10; this build).
// The chooser's HP call: Party_GetHpByType(u8 type), 0 for a knocked-out member.
constexpr std::uint8_t kHpBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                     0x00, 0x00, 0x00, 0xE8, 0x9C, 0x2B, 0x67, 0x01};
constexpr platform::FunctionFact kHp{L"Party_GetHpByType", 0x140A3C384, kHpBytes};
constexpr std::uintptr_t kChooserHpCall = 0x141836901;
// FieldCommandCtrl_IsMemberCommandable(ctrl, ordinal, flag): gated at its first instruction, which
// covers its five callers (one call site straddles a code block; player_two/command_rules_code.h).
constexpr std::uint8_t kCommandableBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x44, 0x88, 0x44,
                                              0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x02, 0x7B, 0x23, 0x01};
constexpr platform::FunctionFact kCommandable{L"FieldCommandCtrl_IsMemberCommandable", 0x140E77414, kCommandableBytes};
constexpr std::uintptr_t         kCommandableResume = 0x140E77419;  // after the replaced instruction
constexpr std::int32_t           kMenuOrdinals      = 8;            // command-menu ordinals searched for the member
// Party_GetCommandOrdinalType(unused, ordinal): the member type at a command-menu ordinal.
constexpr std::uint8_t kOrdinalTypeBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89,
                                              0x78, 0x20, 0x89, 0x50, 0x10, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
                                              0x57, 0x48, 0x8B, 0xEC, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x6C, 0xFD, 0x66,
                                              0x01};
constexpr platform::FunctionFact kOrdinalType{L"Party_GetCommandOrdinalType", 0x140A3F19C, kOrdinalTypeBytes};

// The command controller (EndUISystem+0xB8) and what the log reads of it (§1, §10 Stage 0).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kController  = 0xB8;
constexpr std::uint32_t  kMembers     = 0x34;   // int32 x4: leader, ally, shortcut target, ally pick
constexpr std::uint32_t  kRequests    = 0x570;  // TArray<int32> {data, num}
constexpr std::uint32_t  kHudMode     = 0x540;
constexpr std::uint32_t  kAllyMode    = 0x44C;
constexpr std::uint32_t  kReady       = 0x5E1;
constexpr std::array<std::int32_t, 7> kLoggedRequests{2, 3, 4, 19, 26, 36, 40};
// The game state's speed slots (GetEndGameState 0x140A2E4B4 reads *(0x1490FF2B8)+0x288, then checks
// its class; 12 floats at +0x3C0; slot 0 is the battle command speed the open menu lowers).
constexpr std::uintptr_t kGameMode      = 0x1490FF2B8;
constexpr std::uint32_t  kGameState     = 0x288;
constexpr std::uint32_t  kSpeedSlots    = 0x3C0;

constexpr int kMaxLines = 400;

using HpFn          = std::uint32_t (*)(std::uint8_t type);
using OrdinalTypeFn = std::uint8_t (*)(void* unused, std::int32_t ordinal);

// Set once at install.
platform::CallRedirect g_hpCall;
platform::CodeRedirect g_commandableGate;
OrdinalTypeFn          g_ordinalType = nullptr;
std::uintptr_t         g_endUISystem = 0;
std::uintptr_t         g_gameMode    = 0;

/// What the log shows, as raw values.
struct Snapshot
{
    std::array<std::int32_t, 4>                       members{};
    std::uint8_t                                      hudMode = 0, allyMode = 0, ready = 0;
    std::array<std::int32_t, kLoggedRequests.size()>  requests{};
    std::int32_t                                      speed = 0;  // slot 0, in thousandths
    bool operator==(const Snapshot&) const = default;
};

// The gate is read by the code at the test's entry (-1: no member refused), and written through
// SetGate: Remove may run off the game thread.
std::int32_t g_gate = -1;

// Game thread.
bool          g_installed     = false;
std::int32_t  g_forbiddenType = -1;  // player 2's member's type, or -1
std::uint32_t g_refusals      = 0;
Snapshot      g_logged{};
bool          g_loggedOnce = false;
int           g_lines = 0;

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    if (address != 0)
    {
        platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(T));
    }
    return value;
}

void SetGate(std::int32_t ordinal) noexcept
{
    std::atomic_ref<std::int32_t>(g_gate).store(ordinal, std::memory_order_relaxed);
}

std::int32_t Gate() noexcept
{
    return std::atomic_ref<std::int32_t>(g_gate).load(std::memory_order_relaxed);
}

/// The member type player 1 may not command (cached by Update): player 2's member's, while player 2
/// plays one.
std::optional<std::uint8_t> ForbiddenType() noexcept
{
    if (g_forbiddenType < 0 || !generation::OnGameThread() || !generation::Active() || generation::Member() == nullptr)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(g_forbiddenType);
}

/// The command-menu ordinal of member type `type`, or -1.
std::int32_t OrdinalOf(std::uint8_t type) noexcept
{
    for (std::int32_t ordinal = 0; ordinal < kMenuOrdinals; ++ordinal)
    {
        std::uint8_t at = 0xFF;
        if (!platform::CallGuarded(g_ordinalType, at, static_cast<void*>(nullptr), ordinal))
        {
            return -1;
        }
        if (at == type)
        {
            return ordinal;
        }
    }
    return -1;
}

void NoteRefusal(const wchar_t* where, std::uint8_t type) noexcept
{
    ++g_refusals;
    if (g_refusals <= 10 || g_refusals % 600 == 0)
    {
        platform::Log(L"command rules: {} refused player 2's member (type {}; {} time(s))", where, type, g_refusals);
    }
}

/// Replaces the chooser's HP call: player 2's member reads as knocked out, so ally mode skips it.
std::uint32_t ChooserHp(std::uint8_t type) noexcept
{
    const auto forbidden = ForbiddenType();
    if (forbidden && type == *forbidden)
    {
        NoteRefusal(L"ally mode", type);
        return 0;
    }
    return reinterpret_cast<HpFn>(g_hpCall.Original())(type);
}



}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto hp          = platform::FindFunction(image, kHp);
    const auto commandable = platform::FindFunction(image, kCommandable);
    const auto ordinalType = platform::FindFunction(image, kOrdinalType);
    const auto system      = image.Rebase(kEndUISystem, kController + sizeof(void*));
    const auto mode        = image.Rebase(kGameMode, sizeof(void*));
    g_endUISystem = system ? *system : 0;
    g_gameMode    = mode ? *mode : 0;
    // The HP lever and the command test together, or neither: ally mode and the menu must agree. The
    // gate's code reads g_gate in this DLL: pin it first.
    bool ok = hp && commandable && ordinalType && platform::PinThisModule();
    if (ok)
    {
        g_ordinalType = reinterpret_cast<OrdinalTypeFn>(*ordinalType);
        const auto site = image.Rebase(kChooserHpCall, 5);
        ok = site && g_hpCall.Install(*site, *hp, reinterpret_cast<std::uintptr_t>(&ChooserHp));
    }
    if (ok)
    {
        const auto code   = CommandGateCode(g_gate);
        const auto resume = image.Rebase(kCommandableResume);
        ok = resume && g_commandableGate.Install(*commandable, kCommandGateEntryBytes, code, *resume);
    }
    if (!ok)
    {
        Remove();
    }
    g_installed = ok;
    platform::Log(L"command rules: {}", ok ? L"player 1 cannot command player 2's member (ally mode, the command test)"
                                           : L"UNAVAILABLE (a check failed): player 1 can command player 2's member");
    return ok;
}

void Remove() noexcept
{
    g_installed = false;
    SetGate(-1);  // before the code goes: a caller inside it sees no member refused
    if (g_commandableGate.IsInstalled() && !g_commandableGate.Restore())
    {
        platform::Log(L"command rules: the command test's entry was changed after us; left in place (it passes through)");
    }
    if (g_hpCall.IsInstalled() && !g_hpCall.Restore())
    {
        platform::Log(L"command rules: the chooser's call was changed after us; left in place (it passes through)");
    }
}

void Update() noexcept
{
    // Read every tick: the member's menu ordinal moves when the party order changes.
    const void* member =
        g_installed && generation::OnGameThread() && generation::Active() ? generation::Member() : nullptr;
    const auto         type    = member != nullptr ? party_member::PlayerTypeOf(member) : std::nullopt;
    const std::int32_t ordinal = type ? OrdinalOf(*type) : -1;
    const std::int32_t newType = type ? *type : -1;
    if (newType != g_forbiddenType || ordinal != Gate())
    {
        platform::Log(L"command rules: player 1 may not command type {} (menu ordinal {})", newType, ordinal);
    }
    g_forbiddenType = newType;
    SetGate(ordinal);
}

void Watch() noexcept
{
    if (g_lines >= kMaxLines || g_endUISystem == 0 || !generation::OnGameThread())
    {
        return;
    }
    const auto ctrl = Field<std::uintptr_t>(g_endUISystem + kController);
    if (ctrl == 0)
    {
        return;
    }
    Snapshot now;
    platform::CopyGuarded(now.members.data(), reinterpret_cast<const void*>(ctrl + kMembers), sizeof(now.members));
    now.hudMode  = Field<std::uint8_t>(ctrl + kHudMode);
    now.allyMode = Field<std::uint8_t>(ctrl + kAllyMode);
    now.ready    = Field<std::uint8_t>(ctrl + kReady);
    const auto requests = Field<std::uintptr_t>(ctrl + kRequests);
    const auto count    = Field<std::int32_t>(ctrl + kRequests + 8);
    for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
    {
        const std::int32_t index = kLoggedRequests[i];
        now.requests[i] = requests != 0 && index < count ? Field<std::int32_t>(requests + 4 * static_cast<std::uint32_t>(index)) : -1;
    }
    const auto gameState = Field<std::uintptr_t>(Field<std::uintptr_t>(g_gameMode) + kGameState);
    now.speed = static_cast<std::int32_t>(Field<float>(gameState != 0 ? gameState + kSpeedSlots : 0) * 1000.0f);
    if (g_loggedOnce && now == g_logged)
    {
        return;
    }
    g_loggedOnce = true;
    g_logged     = now;
    ++g_lines;
    try
    {
        std::wstring text = std::format(L"command menu: members leader {} ally {} shortcut {} pick {} | hud {} ally mode {} "
                                        L"ready {} | requests",
                                        now.members[0], now.members[1], now.members[2], now.members[3], now.hudMode,
                                        now.allyMode, now.ready);
        for (std::size_t i = 0; i < kLoggedRequests.size(); ++i)
        {
            text += std::format(L" {}:{}", kLoggedRequests[i], now.requests[i]);
        }
        platform::Log(L"{} | battle command speed {:.3f}", text, now.speed / 1000.0);
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::command_rules
