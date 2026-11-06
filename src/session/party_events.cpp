#include "session/party_events.h"

#include "facts/controller.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/call_site.h"
#include "players/generation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace lss::session::party_events {

namespace {

// Facts read from the database (RE-p2-session-levers.md §3.1, §3.2, §5; this build).
// OnLeaderTransition's one call of SwitchLeaderInternal: rcx = the party manager's controller (+0x140),
// rdx = the new leader, r8b = input-triggered, r9b = force.
constexpr std::uintptr_t kLeaderCall = 0x141204E09;
// 1.005 (RE-1005-field-2.md §3): that call is at 0x140F8B28D, %16 = 13, so it crosses a 16-byte block. There it is
// heard at SwitchLeaderInternal's entry instead (first instruction `mov [rsp+18h], rbx`, 5 bytes at %16 = 4; its only
// code caller is that call), by its return address; other calls (the mod's own, from party_member) pass through.
constexpr std::size_t kSwitchLeaderFirstInstruction = 5;
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


using SwitchLeaderFn  = void (*)(void* controller, void* character, bool inputTriggered, bool force);
using StatusChangedFn = void (*)(void* partyManager, std::uint8_t type, std::uint8_t before, std::uint8_t after);
using PartyAppliedFn  = void (*)(void* partyManager);

// Set once at install; cleared at Remove.
platform::CallRedirect g_leaderCall, g_statusCall, g_appliedCall;
platform::EntryRedirect g_leaderEntry;     // 1.005: the leader change, heard at the entry
std::uintptr_t          g_leaderReturn = 0;  // 1.005: live return address of OnLeaderTransition's call
Listener               g_listener{};

// Counted off the game thread, from either call.

/// True if the listener may hear a call now (logged, a few times, when not).
bool Hearable(const wchar_t* what) noexcept
{
    if (players::generation::OnGameThread())
    {
        return true;
    }
    platform::Log(L"party events: {} made {}; the session did not hear it", what,
                  players::generation::GameThreadKnown() ? L"off the game thread"
                                                : L"before the first engine tick (the game thread is not known yet)");
    return false;
}

void HearLeaderChange(void* controller, void* character) noexcept
{
    if (g_listener.leaderChanging != nullptr && Hearable(L"a leader change"))
    {
        g_listener.leaderChanging(g_listener.context, controller, character);
    }
}

/// Replaces OnLeaderTransition's call: the listener first, then the game's switch.
void SwitchLeader(void* controller, void* character, bool inputTriggered, bool force) noexcept
{
    HearLeaderChange(controller, character);
    reinterpret_cast<SwitchLeaderFn>(g_leaderCall.Original())(controller, character, inputTriggered, force);
}

/// 1.005: every call of SwitchLeaderInternal, at its entry. OnLeaderTransition's (by its return address) is heard
/// exactly as SwitchLeader hears it; the rest pass through.
void SwitchLeaderEntry(void* controller, void* character, bool inputTriggered, bool force) noexcept
{
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_leaderReturn)
    {
        HearLeaderChange(controller, character);
    }
    reinterpret_cast<SwitchLeaderFn>(g_leaderEntry.Original())(controller, character, inputTriggered, force);
}

/// The leader change: OnLeaderTransition's call redirected on the research build; on 1.005, the entry.
bool InstallLeader(const platform::GameImage& image, std::uintptr_t switchLeader, std::uintptr_t leaderAt) noexcept
{
    if (platform::RunningBuild() != platform::BuildIndex::V1005)
    {
        return g_leaderCall.Install(leaderAt, switchLeader, reinterpret_cast<std::uintptr_t>(&SwitchLeader));
    }
    if (!players::call_site::Calls(leaderAt, switchLeader))
    {
        return false;
    }
    g_leaderReturn = leaderAt + 5;  // before the install: the replacement runs the moment it is reachable
    const auto first = std::span(facts::kSwitchLeaderInternalBytes).first(kSwitchLeaderFirstInstruction);
    const bool ok = g_leaderEntry.Install(switchLeader, image.EntryInstruction(facts::kSwitchLeaderInternal.address, first),
                                          reinterpret_cast<std::uintptr_t>(&SwitchLeaderEntry));
    g_leaderReturn = ok ? g_leaderReturn : 0;
    return ok;
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
                    InstallLeader(image, *switchLeader, *leaderAt) &&
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
    platform::Log(L"party events: the further players hear the game's leader changes and party status changes{}",
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
    if (g_leaderEntry.IsInstalled() && !g_leaderEntry.Restore())
    {
        platform::Log(L"party events: the leader switch's entry was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::session::party_events
