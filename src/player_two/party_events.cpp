#include "player_two/party_events.h"

#include "platform/call_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <atomic>
#include <cstdint>

namespace lss::player_two::party_events {

namespace {

// Facts read from the database (RE-p2-session-levers.md §3.1, §3.2, §5; this build).
// OnLeaderTransition's one call of SwitchLeaderInternal: rcx = the party manager's controller (+0x140),
// rdx = the new leader, r8b = input-triggered, r9b = force.
constexpr std::uintptr_t kLeaderCall = 0x141204E09;
// The status commit's handler: PartyManager_OnPlayerStatusChanged(party manager, type dl, old r8b,
// new r9b), called once, from the status TFunction's thunk.
constexpr std::uint8_t kStatusChangedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                                0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00,
                                                0x00, 0xE8, 0x5E, 0xCC, 0x8A, 0x00};
constexpr platform::FunctionFact kStatusChanged{L"PartyManager_OnPlayerStatusChanged", 0x1418022B8,
                                                kStatusChangedBytes};
constexpr std::uintptr_t kStatusCall = 0x1418022AB;
// AEndPartyCoordinator_Tick's one call of PartyManager_ApplyPartyDataAndRefresh(party manager), made
// once a party stream finished (RE-p2-member-change.md; its only code caller).
constexpr std::uint8_t kPartyAppliedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56,
                                               0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C};
constexpr platform::FunctionFact kPartyApplied{L"PartyManager_ApplyPartyDataAndRefresh", 0x1411BBAD8, kPartyAppliedBytes};
constexpr std::uintptr_t         kAppliedCall = 0x140F7F5A6;

constexpr int kMaxLines = 40;

using SwitchLeaderFn  = void (*)(void* controller, void* character, bool inputTriggered, bool force);
using StatusChangedFn = void (*)(void* partyManager, std::uint8_t type, std::uint8_t before, std::uint8_t after);
using PartyAppliedFn  = void (*)(void* partyManager);

// Set once at install; cleared at Remove.
platform::CallRedirect g_leaderCall, g_statusCall, g_appliedCall;
Listener               g_listener{};

// Counted off the game thread, from either call.
std::atomic<int> g_lines{0};

/// True if the listener may hear a call now (logged, a few times, when not).
bool Hearable(const wchar_t* what) noexcept
{
    if (generation::OnGameThread())
    {
        return true;
    }
    if (g_lines.fetch_add(1) < kMaxLines)
    {
        platform::Log(L"party events: {} made off the game thread; the session did not hear it", what);
    }
    return false;
}

/// Replaces OnLeaderTransition's call: the listener first, then the game's switch.
void SwitchLeader(void* controller, void* character, bool inputTriggered, bool force) noexcept
{
    if (g_listener.leaderChanging != nullptr && Hearable(L"a leader change"))
    {
        g_listener.leaderChanging(g_listener.context, controller, character);
    }
    reinterpret_cast<SwitchLeaderFn>(g_leaderCall.Original())(controller, character, inputTriggered, force);
}

/// Replaces the status thunk's call: the game's handler, then the listener.
void StatusChanged(void* partyManager, std::uint8_t type, std::uint8_t before, std::uint8_t after) noexcept
{
    reinterpret_cast<StatusChangedFn>(g_statusCall.Original())(partyManager, type, before, after);
    if (g_listener.statusChanged != nullptr && Hearable(L"a party status change"))
    {
        g_listener.statusChanged(g_listener.context, type, before, after);
    }
}

/// Replaces the coordinator's call: the game's apply, then the listener.
void PartyApplied(void* partyManager) noexcept
{
    reinterpret_cast<PartyAppliedFn>(g_appliedCall.Original())(partyManager);
    if (g_listener.partyApplied != nullptr && Hearable(L"a party data apply"))
    {
        g_listener.partyApplied(g_listener.context);
    }
}

}  // namespace

bool Install(const platform::GameImage& image, const Listener& listener) noexcept
{
    const auto switchLeader = platform::FindFunction(image, facts::kSwitchLeaderInternal);
    const auto statusChanged = platform::FindFunction(image, kStatusChanged);
    const auto leaderAt = image.Rebase(kLeaderCall, 5), statusAt = image.Rebase(kStatusCall, 5);
    g_listener  = listener;
    const bool ok = switchLeader && statusChanged && leaderAt && statusAt &&
                    g_leaderCall.Install(*leaderAt, *switchLeader, reinterpret_cast<std::uintptr_t>(&SwitchLeader)) &&
                    g_statusCall.Install(*statusAt, *statusChanged, reinterpret_cast<std::uintptr_t>(&StatusChanged));
    if (!ok)
    {
        Remove();
        platform::Log(L"party events: UNAVAILABLE (a check failed): only the session's slow checks follow the party");
        return false;
    }
    const auto applied   = platform::FindFunction(image, kPartyApplied);
    const auto appliedAt = image.Rebase(kAppliedCall, 5);
    const bool heard     = applied && appliedAt &&
                       g_appliedCall.Install(*appliedAt, *applied, reinterpret_cast<std::uintptr_t>(&PartyApplied));
    platform::Log(L"party events: player 2 hears the game's leader changes and party status changes{}",
                  heard ? L", and each party data apply (squad and formation changes)"
                        : L"; NOT the party data applies (a check failed: squad changes are found by the slow check)");
    return true;
}

void Remove() noexcept
{
    g_listener = {};  // a call left in place (changed after us) then reaches no listener
    for (auto* call : {&g_appliedCall, &g_statusCall, &g_leaderCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"party events: a call was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::player_two::party_events
