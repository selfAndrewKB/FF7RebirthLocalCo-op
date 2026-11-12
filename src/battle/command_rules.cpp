#include "battle/command_rules.h"

#include "battle/command_rules_code.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "session/party_member.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace lss::battle::command_rules {

namespace {

// Facts read from the database (research/RE-p2-command-menu.md; this build).
// The chooser's HP call: Party_GetHpByType(u8 type), 0 for a knocked-out member.
constexpr std::uint8_t kHpBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                     0x00, 0x00, 0x00, 0xE8, 0x9C, 0x2B, 0x67, 0x01};
constexpr platform::FunctionFact kHp{L"Party_GetHpByType", 0x140A3C384, kHpBytes};
constexpr std::uintptr_t kChooserHpCall = 0x141836901;
// FieldCommandCtrl_IsMemberCommandable(ctrl, ordinal, flag): gated at its first instruction, which
// covers its five callers (one call site straddles a code block; battle/command_rules_code.h).
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

// The refused ordinals, one bit each (bit n: ordinal n; 0 refuses none), read by the code at the test's entry and
// written through SetMask: Remove may run off the game thread. The five callers pass an ordinal from 0 to the party
// count less one (at most 16), or -1 for none, which the bit test reads as bit 31, never set here
// (research/RE-seats-step6-battle.md).
std::uint32_t g_mask = 0;

// Game thread.
bool          g_installed      = false;
std::uint32_t g_forbiddenTypes = 0;  // the refused member types, one bit each (types below 32)
std::uint32_t g_refusals       = 0;

void SetMask(std::uint32_t mask) noexcept
{
    std::atomic_ref<std::uint32_t>(g_mask).store(mask, std::memory_order_relaxed);
}

std::uint32_t Mask() noexcept
{
    return std::atomic_ref<std::uint32_t>(g_mask).load(std::memory_order_relaxed);
}

/// One bit for `value` (0..31), or none.
std::uint32_t Bit(std::int32_t value) noexcept
{
    return value >= 0 && value < 32 ? 1u << value : 0u;
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

/// The refused types and ordinals for one pass: every playing seat's member but `except`'s (0: none excepted), and
/// `alsoType` (player 1's leader in a seat's pass; -1 none).
struct Refused
{
    std::uint32_t types = 0, ordinals = 0;
};
Refused RefusedFor(int except, std::int32_t alsoType) noexcept
{
    Refused refused;
    const auto add = [&refused](std::int32_t type) {
        if (type >= 0)
        {
            refused.types |= Bit(type);
            refused.ordinals |= Bit(OrdinalOf(static_cast<std::uint8_t>(type)));
        }
    };
    add(alsoType);
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        const void* member = seat != except && players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        if (const auto type = member != nullptr ? session::party_member::PlayerTypeOf(member) : std::nullopt)
        {
            add(*type);
        }
    }
    return refused;
}

void NoteRefusal(const wchar_t* where, std::uint8_t type) noexcept
{
    ++g_refusals;
    if (g_refusals <= 10 || g_refusals % 600 == 0)
    {
        platform::Log(L"command rules: {} refused member type {} ({} time(s))", where, type, g_refusals);
    }
}

/// Replaces the chooser's HP call: a refused member reads as knocked out, so ally mode skips it.
std::uint32_t ChooserHp(std::uint8_t type) noexcept
{
    if (players::generation::OnGameThread() && players::generation::AnyActive() && (g_forbiddenTypes & Bit(type)) != 0)
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
    // gate's code reads g_mask in this DLL: pin it first.
    bool ok = hp && commandable && ordinalType && platform::PinThisModule();
    if (ok)
    {
        g_ordinalType = reinterpret_cast<OrdinalTypeFn>(*ordinalType);
        const auto site = image.Rebase(kChooserHpCall, 5);
        ok = site && g_hpCall.Install(*site, *hp, reinterpret_cast<std::uintptr_t>(&ChooserHp));
    }
    if (ok)
    {
        const auto code   = CommandGateMaskCode(g_mask);
        const auto resume = image.Rebase(kCommandableResume);
        ok = resume && g_commandableGate.Install(*commandable, kCommandGateEntryBytes, code, *resume);
    }
    if (!ok)
    {
        Remove();
    }
    g_installed = ok;
    platform::Log(L"command rules: {}",
                  ok ? L"no player commands another player's member (ally mode, the command test)"
                     : L"UNAVAILABLE (a check failed): player 1 can command players 2 and 3's members");
    return ok;
}

void Remove() noexcept
{
    g_installed = false;
    SetMask(0);  // before the code goes: a caller inside it sees no member refused
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
    // Read every tick: a member's menu ordinal moves when the party order changes.
    const Refused refused = g_installed && players::generation::OnGameThread() ? RefusedFor(0, -1) : Refused{};
    if (refused.types != g_forbiddenTypes || refused.ordinals != Mask())
    {
        platform::Log(L"command rules: player 1 may not command member types {:#x} (menu ordinals {:#x})", refused.types,
                      refused.ordinals);
    }
    g_forbiddenTypes = refused.types;
    SetMask(refused.ordinals);
}

bool Installed() noexcept
{
    return g_installed;
}

bool Refuses(int ordinal) noexcept
{
    return (Mask() & Bit(ordinal)) != 0;
}

SeatRules::SeatRules(int seat, std::uint8_t leaderType) noexcept
    : savedOrdinals_(Mask()), savedTypes_(g_forbiddenTypes)
{
    const Refused refused = RefusedFor(seat, leaderType);
    g_forbiddenTypes      = refused.types;
    SetMask(refused.ordinals);
}

SeatRules::~SeatRules()
{
    g_forbiddenTypes = savedTypes_;
    SetMask(savedOrdinals_);
}

}  // namespace lss::battle::command_rules
