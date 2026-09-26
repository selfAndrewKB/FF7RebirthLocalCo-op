#include "player_two/party_switch.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"

#include <cstdint>
#include <iterator>

namespace lss::player_two::party_switch {

namespace {

// Facts read from the database (RE-p2-party-switch.md §Q1, §Q4, recipe; this build).
// The two component getters the switch asks per member, and their calls in the cycle and CanSwitch.
constexpr std::uint8_t kPlayerComponentBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x08, 0xF7, 0x6B, 0x01};
constexpr std::uint8_t kAiComponentBytes[]     = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x02, 0xF0, 0x6B, 0x01};
constexpr platform::FunctionFact kPlayerComponent{L"EndCharacter_GetBattlePlayerControllerComponent", 0x1409EF81C,
                                                  kPlayerComponentBytes};
constexpr platform::FunctionFact kAiComponent{L"EndCharacter_GetAIControllerComponent", 0x1409EFF24, kAiComponentBytes};
constexpr std::uintptr_t kCycleCall     = 0x14158BAB0;  // in BattleLeader_CycleSwitch: rcx = the candidate
constexpr std::uintptr_t kCanSwitchCall = 0x14158BD82;  // in CanSwitch: rcx = the member counted

// The switch widget's "cannot switch" test, bool(u8 player type), and its two calls; the game's
// lookup of a character by player type (a leaf), PartyManager_GetCharacterByPlayerType(0, type).
constexpr std::uint8_t kDisabledBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x80,
                                           0x00, 0x00, 0x00, 0xE8, 0x38, 0xD3, 0x17, 0x01};
constexpr std::uint8_t kByTypeBytes[]   = {0x4C, 0x8B, 0x05, 0xA5, 0xA6, 0x96, 0x08, 0x44,
                                           0x0F, 0xB6, 0xC9, 0x4D, 0x85, 0xC0, 0x0F, 0x84};
constexpr platform::FunctionFact kDisabled{L"CommandSwitchHud_IsMemberSwitchDisabled", 0x140F31BE8, kDisabledBytes};
constexpr platform::FunctionFact kByType{L"PartyManager_GetCharacterByPlayerType", 0x140A31634, kByTypeBytes};
constexpr std::uintptr_t kDisabledCalls[] = {0x140F318C3, 0x140F31BB5};

using ComponentFn = void* (*)(void* character);
using DisabledFn  = std::uint8_t (*)(std::uint8_t type);
using ByTypeFn    = void* (*)(void* unused, std::uint8_t type);

// Set once at install.
platform::CallRedirect g_cycleCall;
platform::CallRedirect g_canSwitchCall;
platform::CallRedirect g_disabledCalls[2];
ByTypeFn               g_byType = nullptr;

// Game thread.
std::uint32_t g_refusals       = 0;  // candidates the switch was told to skip
std::uint32_t g_loggedRefusals = 0;
bool          g_greyLogged     = false;

/// Player 2's member while player 2 plays, else null.
void* PlayerTwoMember() noexcept
{
    return generation::OnGameThread() && generation::Active() ? generation::Member() : nullptr;
}

/// Logs the first refusals, then every 600th (a downed leader retries the switch every frame).
void NoteRefusal(const wchar_t* where) noexcept
{
    ++g_refusals;
    if (g_refusals <= 10 || g_refusals - g_loggedRefusals >= 600)
    {
        g_loggedRefusals = g_refusals;
        platform::Log(L"party switch: {} skipped player 2's member ({} time(s) so far)", where, g_refusals);
    }
}

/// Replaces the cycle's component call: none for player 2's member (the game skips it).
void* CycleComponent(void* character) noexcept
{
    void* member = PlayerTwoMember();
    if (member != nullptr && character == member)
    {
        NoteRefusal(L"the switch");
        return nullptr;
    }
    return reinterpret_cast<ComponentFn>(g_cycleCall.Original())(character);
}

/// Replaces CanSwitch's component call: player 2's member is not counted.
void* CountedComponent(void* character) noexcept
{
    void* member = PlayerTwoMember();
    if (member != nullptr && character == member)
    {
        return nullptr;
    }
    return reinterpret_cast<ComponentFn>(g_canSwitchCall.Original())(character);
}

/// Replaces the widget's test: player 2's member's portrait reads as not switchable.
std::uint8_t SwitchDisabled(std::uint8_t type) noexcept
{
    void* member    = PlayerTwoMember();
    void* character = nullptr;
    if (member != nullptr && platform::CallGuarded(g_byType, character, static_cast<void*>(nullptr), type) &&
        character == member)
    {
        if (!g_greyLogged)
        {
            g_greyLogged = true;
            platform::Log(L"party switch: the switch widget greys player 2's member (type {})", type);
        }
        return 1;
    }
    return reinterpret_cast<DisabledFn>(g_disabledCalls[0].Original())(type);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto player   = platform::FindFunction(image, kPlayerComponent);
    const auto ai       = platform::FindFunction(image, kAiComponent);
    const auto cycle    = image.Rebase(kCycleCall, 5);
    const auto count    = image.Rebase(kCanSwitchCall, 5);
    // The cycle and its count together, or neither: a count that disagrees with the cycle would let
    // the switch be offered with no one to switch to.
    bool switching = player && ai && cycle && count &&
                     g_cycleCall.Install(*cycle, *player, reinterpret_cast<std::uintptr_t>(&CycleComponent));
    if (switching && !g_canSwitchCall.Install(*count, *ai, reinterpret_cast<std::uintptr_t>(&CountedComponent)))
    {
        if (!g_cycleCall.Restore())
        {
            platform::Log(L"party switch: the cycle's call could not be put back; it stays (it passes through)");
        }
        switching = false;
    }
    // The widget greys player 2's member only when the switch really skips it.
    const auto disabled = platform::FindFunction(image, kDisabled);
    const auto byType   = platform::FindFunction(image, kByType);
    bool       widget   = switching && disabled && byType;
    if (widget)
    {
        g_byType = reinterpret_cast<ByTypeFn>(*byType);
        for (std::size_t i = 0; widget && i < std::size(kDisabledCalls); ++i)
        {
            const auto site = image.Rebase(kDisabledCalls[i], 5);
            widget = site && g_disabledCalls[i].Install(*site, *disabled, reinterpret_cast<std::uintptr_t>(&SwitchDisabled));
        }
        if (!widget)
        {
            for (auto& call : g_disabledCalls)
            {
                if (call.IsInstalled() && !call.Restore())
                {
                    platform::Log(L"party switch: a widget call could not be put back; it stays (it passes through)");
                }
            }
        }
    }
    platform::Log(L"party switch: {}; the switch widget {}",
                  switching ? L"player 1's switch skips player 2's member"
                            : L"UNAVAILABLE (a check failed): player 1 can switch onto player 2's member",
                  widget ? L"greys player 2's member" : L"shows player 2's member as switchable (a check failed)");
    return switching;
}

void Remove() noexcept
{
    for (auto* call : {&g_disabledCalls[1], &g_disabledCalls[0], &g_canSwitchCall, &g_cycleCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"party switch: a switch call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::party_switch
