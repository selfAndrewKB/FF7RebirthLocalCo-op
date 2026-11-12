#pragma once

// The game's two party funnels, heard by the session (research/RE-p2-session-levers.md):
//   leader   the one call of AEndPlayerController::SwitchLeaderInternal, inside OnLeaderTransition.
//            Every leader change of the game's controller passes it (party switches, scripts, quests,
//            vehicles, battle start and end, KO switches, party data applies). The listener hears it
//            before the game's call runs, so a seat can let go of a member the game is about to
//            make player 1's leader.
//   status   the one call of PartyManager_OnPlayerStatusChanged, from the party manager's status
//            commit. Every deployment change passes it (joined, reserve, away from battle). The
//            listener hears it after the game's handler ran.
//   applied  the party coordinator's one call of PartyManager_ApplyPartyDataAndRefresh, made when its
//            party stream finished (RE-p2-member-change.md): after new party data was written and every
//            character it needs was created, destroyed or re-specced. The in-field squad switch and the
//            menu formation change write the statuses directly (no status commit), so only this call
//            reports them; it also follows field loads, scripts and equipment or costume changes. The
//            listener hears it after the game's call ran.
// The mod's own take and give-back call SwitchLeaderInternal directly, not through here.
//
// The calls are redirected (platform/call_redirect.h); the game's call always runs. Game thread
// only: a call from another thread passes through unheard (logged).

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::session::party_events {

struct Listener
{
    /// Before the game makes `character` the leader of `controller` (a null character releases).
    void (*leaderChanging)(void* context, void* controller, void* character) noexcept = nullptr;
    /// After the game changed `playerType`'s party status from `before` to `after`.
    void (*statusChanged)(void* context, std::uint8_t playerType, std::uint8_t before, std::uint8_t after) noexcept =
        nullptr;
    /// After the game applied new party data (its party stream finished).
    void (*partyApplied)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

/// Redirects the leader and status calls, both or neither. False (logged) if a check fails; the
/// session then keeps only its backstop checks. The applied call is installed on its own (logged).
bool Install(const platform::GameImage& image, const Listener& listener) noexcept;

/// Puts the calls back.
void Remove() noexcept;

}  // namespace lss::session::party_events
