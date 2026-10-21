#pragma once

// The synergy partner's decisions (player_two/synergy_partner.h) as pure functions over values read from the game,
// and the table it keeps, tested on the host (tests/synergy_rules_test.cpp). The module keeps the engine reads, the
// redirects and the log lines.

#include "player_two/party_lease.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace lss::player_two::synergy_rules {

using Clock = std::chrono::steady_clock;

inline constexpr std::uint8_t kNoType  = 16;    // the game's "no character type"
inline constexpr std::uint8_t kNoMatch = 0xFF;  // a leader type no character's type equals (types are 0..16)
inline constexpr std::size_t  kHeld    = 4;     // characters handed to their AI at once: at most the two humans' (spare)
inline constexpr int          kRefused = -1;    // a member end's player: T3 refused a remembered character

enum class EndSeen : std::uint8_t
{
    Never,
    GameThread,
};

/// A human's character the game handed to its AI for a synergy approach, and whose controller gets it back.
struct Held
{
    const void*       character  = nullptr;
    std::uint64_t     weak       = 0;
    int               player     = 0;               // 1 or 2; 0 = free
    std::uint8_t      type       = kNoType;         // its player type (the give-back test's answer)
    Clock::time_point since{};                      // the hand-over of this approach
    bool              warned     = false;
    EndSeen           endSeen    = EndSeen::Never;  // a member end of it ran (and did not give it back)
    bool              recordSeen = false;           // the live link record naming it as the partner was found
    LinkRecordView    record{};                     // that record as last read
    Clock::time_point goneSince{};                  // that record not found since (zero while found)
    Clock::time_point givenAt{};                    // the tick gave it back after a member end off the game thread
                                                    // (zero otherwise): Confirm checks the possession
};

// ---- the hand-over (T1 / T2): "is the partner the player's character?" ----

enum class HandOver
{
    GamesOwn,    // no partner, no session, or Classic mode: the game's answer
    AiPartner,   // the partner is driven by its AI: the game's own approach
    Lost,        // its player no longer owns it (a give-back under way): the game's answer
    NoType,      // no readable player type: the game's answer
    Kept,        // a parry-window skill's stance: kept by its player until the counter's own request
    HandedOver,  // answered as the player's character; remembered for the give-back
};

struct HandOverAnswer
{
    HandOver     what   = HandOver::GamesOwn;
    std::uint8_t answer = 0;
    std::uint8_t type   = kNoType;  // the partner's type when it was read
};

/// `known`: a partner is named, the session is bound and Classic mode is off. `player`: who holds the partner (0: its
/// AI). `lost()` and `typeOf()` are read only when the decision needs them, in this order.
template <class LostFn, class TypeFn>
HandOverAnswer DecideHandOver(std::uint8_t original, bool known, int player, LostFn&& lost, TypeFn&& typeOf,
                              bool window) noexcept
{
    if (!known)
    {
        return {HandOver::GamesOwn, original};
    }
    if (player == 0)
    {
        return {HandOver::AiPartner, original};
    }
    if (lost())
    {
        return {HandOver::Lost, original};
    }
    const std::uint8_t type = typeOf();
    if (type >= kNoType)
    {
        return {HandOver::NoType, original, type};
    }
    if (window)
    {
        // Kept by its player: a game's own yes (the leader type is the partner's) waits for the counter's request too.
        return {HandOver::Kept, static_cast<std::uint8_t>(original == type ? kNoMatch : original), type};
    }
    return {HandOver::HandedOver, type, type};
}

// ---- the give-back (P): the controller the member goes back to ----

enum class GiveBackTo
{
    Original,   // the game's own answer
    PlayerOne,  // player 1's controller (null if there is none)
    Player,     // `player`'s controller; the game's own answer if that player has none
};

struct GiveBack
{
    GiveBackTo to     = GiveBackTo::Original;
    int        player = 0;
};

/// `endPlayer`: the member end's player (kRefused, 0 unknown, 1 or 2). Off the game thread a primary read that names
/// player 2's controller becomes player 1's. A refused give-back never lands on player 2's controller: player 1's
/// instead, when there is one and the member is not player 2's. A remembered character goes to its player; player 2's
/// member to player 2 in any case.
constexpr GiveBack DecideGiveBack(bool offThread, int endPlayer, bool originalIsPlayerTwos, bool hasPlayerOne,
                                  bool playerTwosMember) noexcept
{
    if (offThread)
    {
        return {originalIsPlayerTwos ? GiveBackTo::PlayerOne : GiveBackTo::Original, 0};
    }
    if (endPlayer == kRefused)
    {
        return {originalIsPlayerTwos && hasPlayerOne && !playerTwosMember ? GiveBackTo::PlayerOne : GiveBackTo::Original,
                0};
    }
    return {GiveBackTo::Player, endPlayer == 0 && playerTwosMember ? 2 : endPlayer};
}

// ---- the tick's notes: a member end that ran off the game thread ----

enum class NoteAction
{
    NotRemembered,  // the character is not remembered: logged only
    Stale,          // an end of an earlier approach, or one given back already
    AlreadyHeld,    // its player drives it already: forgotten
    GameGave,       // the game queued its own give-back: Confirm checks it
    GiveBack,       // the tick gives it back, if WhyNotGiveBack allows
};

/// `playerHolds()` is read only when the decision needs it.
template <class HoldsFn>
NoteAction DecideNote(bool remembered, bool beforeHandOver, bool givenAlready, HoldsFn&& playerHolds,
                      bool gameGave) noexcept
{
    if (!remembered)
    {
        return NoteAction::NotRemembered;
    }
    if (beforeHandOver || givenAlready)
    {
        return NoteAction::Stale;
    }
    if (playerHolds())
    {
        return NoteAction::AlreadyHeld;
    }
    return gameGave ? NoteAction::GameGave : NoteAction::GiveBack;
}

// ---- the tables ----

/// The slot to record `character` in: its own (a live entry naming it), else a free one, else the oldest.
template <class Entry, std::size_t N>
Entry& SlotFor(std::array<Entry, N>& entries, const void* character) noexcept
{
    auto own = std::find_if(entries.begin(), entries.end(), [character](const Entry& entry) {
        return entry.player != 0 && entry.character == character;
    });
    if (own != entries.end())
    {
        return *own;
    }
    auto free = std::find_if(entries.begin(), entries.end(), [](const Entry& entry) { return entry.player == 0; });
    if (free != entries.end())
    {
        return *free;
    }
    return *std::min_element(entries.begin(), entries.end(),
                             [](const Entry& a, const Entry& b) { return a.since < b.since; });
}

}  // namespace lss::player_two::synergy_rules
