#pragma once

// Running one engine call as if player 2 were the primary player.
//
// Square's party manager holds one "primary" player controller (+0x140, an FWeakObjectPtr), and a
// large part of the game's camera and party code asks it for "the" player: the camera update reads
// the stick input through it, the camera helpers find "the" camera manager through it, and the
// party-member switch writes two bytes to "the" controller. Called for player 2, that code would
// read and write player 1's objects.
//
// A Scope points the field at player 2's controller for its lifetime and puts player 1's back
// afterwards (player_two/scopes/swap_window.h). It opens only if the field names player 1's controller
// when it starts, and it restores only if the field still names player 2's when it ends; if the
// engine wrote the field in between, that newer value is kept and the player-2 generation is
// faulted, because the mod no longer knows who the primary player is.

#include "player_two/scopes/swap_window.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::primary_controller {

/// Finds the party manager and the weak-pointer functions. False (logged) if any check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// Prepares scopes for these two controllers. False if player 2's weak value cannot be built.
bool Bind(void* playerOneController, void* playerTwoController) noexcept;

void Unbind() noexcept;

/// The engine's FWeakObjectPtr value {ObjectIndex, SerialNumber} for `object`, made by the game's
/// own FWeakObjectPtr::operator= (which gives the object a serial number if it has none), and
/// checked to resolve back to it. 0 if not installed or it does not resolve.
std::uint64_t WeakValueOf(void* object) noexcept;

/// True if the party manager's primary controller is `controller` right now.
bool Is(const void* controller) noexcept;

/// The primary controller as the engine resolves it (null if none).
void* Current() noexcept;

class Scope
{
public:
    enum class Refusal
    {
        None,
        NotBound,
        NoPartyManager,
        PlayerTwoStale,     // player 2's controller is no longer the object the weak value named
        NotPlayerOne,       // the field named neither player
        Changed,            // the field changed between the check and the swap
        Misaligned,         // the field cannot be written atomically
    };

    Scope() noexcept;
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

    /// True while the primary controller is player 2's (swapped here or by an enclosing scope).
    bool Held() const noexcept { return refusal_ == Refusal::None; }
    Refusal Why() const noexcept { return refusal_; }

private:
    SwapWindow window_;
    Refusal    refusal_ = Refusal::NotBound;
};

const wchar_t* Describe(Scope::Refusal refusal) noexcept;

}  // namespace lss::player_two::primary_controller
