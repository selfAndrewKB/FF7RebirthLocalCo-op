#include "player_two/battle/command_rules.h"

#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"
#include "player_two/battle/command_rules_code.h"
#include "player_two/generation.h"
#include "player_two/session/party_member.h"
#include "players/seats.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace lss::player_two::command_rules {

namespace {

// Facts read from the database (RE-p2-command-menu.md §4.2, §4.4, §10; this build).
// The chooser's HP call: Party_GetHpByType(u8 type), 0 for a knocked-out member.
constexpr std::uint8_t kHpBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                     0x00, 0x00, 0x00, 0xE8, 0x9C, 0x2B, 0x67, 0x01};
constexpr platform::FunctionFact kHp{L"Party_GetHpByType", 0x140A3C384, kHpBytes};
constexpr std::uintptr_t kChooserHpCall = 0x141836901;
// FieldCommandCtrl_IsMemberCommandable(ctrl, ordinal, flag): gated at its first instruction, which
// covers its five callers (one call site straddles a code block; player_two/battle/command_rules_code.h).
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

using HpFn          = std::uint32_t (*)(std::uint8_t type);
using OrdinalTypeFn = std::uint8_t (*)(void* unused, std::int32_t ordinal);

// Set once at install.
platform::CallRedirect g_hpCall;
platform::CodeRedirect g_commandableGate;
OrdinalTypeFn          g_ordinalType = nullptr;

// The gate is read by the code at the test's entry (-1: no member refused), and written through
// SetGate: Remove may run off the game thread.
std::int32_t g_gate = -1;

// The rules are player 2's only. The gate's code compares one ordinal (22 bytes: `cmp edx, [gate]`); with a third
// player, player 1 must refuse two members and each seat's pass player 1's leader and the other seat's member, so the
// gate must hold a set. A set code fits the same 22 bytes (CommandGateMaskCode, host-tested) but reads an ordinal
// modulo 32 where this one compares exactly, so it may replace this one only once IDA shows the five callers never
// pass an ordinal outside 0..31; the HP lever must then refuse the same set (ally mode and the menu must agree).
constexpr int kRulesSeat = players::kPlayerTwoSeat;
static_assert(players::kSeatsBuilt == 1, "the command gate holds one ordinal (player 2's member): confirm the callers' "
                                         "ordinal range in IDA, install CommandGateMaskCode with a mask per pass, and "
                                         "widen the HP lever alike before seat 2 is built");

// Game thread.
bool          g_installed     = false;
std::int32_t  g_forbiddenType = -1;  // player 2's member's type, or -1
std::uint32_t g_refusals      = 0;

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
    if (g_forbiddenType < 0 || !generation::OnGameThread() || !generation::Active(kRulesSeat) ||
        generation::MemberOf(kRulesSeat) == nullptr)
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
    const void* member = g_installed && generation::OnGameThread() && generation::Active(kRulesSeat)
                             ? generation::MemberOf(kRulesSeat)
                             : nullptr;
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

PlayerTwoRules::PlayerTwoRules(std::uint8_t leaderType) noexcept : savedGate_(Gate()), savedType_(g_forbiddenType)
{
    g_forbiddenType = leaderType;
    SetGate(OrdinalOf(leaderType));
}

PlayerTwoRules::~PlayerTwoRules()
{
    g_forbiddenType = savedType_;
    SetGate(savedGate_);
}

}  // namespace lss::player_two::command_rules
