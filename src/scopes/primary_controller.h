#pragma once

// Running one engine call as if a seat's player (players/seats.h) were the primary player.
//
// Square's party manager holds one "primary" player controller (+0x140, an FWeakObjectPtr), and a
// large part of the game's camera and party code asks it for "the" player: the camera update reads
// the stick input through it, the camera helpers find "the" camera manager through it, and the
// party-member switch writes two bytes to "the" controller. Called for a seat's player, that code would
// read and write player 1's objects.
//
// A Scope points the field at its seat's controller for its lifetime and puts player 1's back
// afterwards (scopes/swap_window.h). It opens only if the field names player 1's controller
// when it starts (or the seat's, for a scope inside the seat's own), and it restores only if the field
// still names the seat's when it ends; if the engine wrote the field in between, that newer value is
// kept and every seat's generation is faulted, because the mod no longer knows who the primary player
// is. Seats swap in turn, never one inside another's: a scope for one seat inside another seat's is
// refused (NotPlayerOne). A mirror's player (views/mirror_player.h) can be held the same way, from player 1's only, so
// a placement that projects through "the" player places player 1's widgets from the mirror's own camera.

#include "players/seats.h"
#include "scopes/swap_window.h"

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::scopes::primary_controller {

/// Finds the party manager and the weak-pointer functions. False (logged) if any check fails.
bool Install(const platform::GameImage& image) noexcept;

bool Installed() noexcept;

/// Prepares `seat`'s scopes for player 1's controller and the seat's. False if the seat's weak value cannot be built.
bool Bind(int seat, void* playerOneController, void* controller) noexcept;

void Unbind(int seat) noexcept;

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
        SeatStale,          // the seat's controller is no longer the object the weak value named
        NotPlayerOne,       // the field named neither player 1 nor the seat (another seat's scope is open)
        Changed,            // the field changed between the check and the swap
        Misaligned,         // the field cannot be written atomically
    };

    /// `seat`'s player as the primary player.
    explicit Scope(int seat) noexcept;
    /// A mirror's `controller` as the primary player, if the field names player 1's `playerOne` (never inside a
    /// seat's scope, nor inside another of its own: both refused NotPlayerOne).
    Scope(void* playerOne, void* controller) noexcept;
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

    /// True while the primary controller is the seat's (swapped here or by an enclosing scope).
    bool Held() const noexcept { return refusal_ == Refusal::None; }
    Refusal Why() const noexcept { return refusal_; }

private:
    /// The field from `current` to `replacement`; the refusal from the window's answer.
    void Swap(std::uint64_t* field, std::uint64_t current, std::uint64_t replacement) noexcept;

    SwapWindow window_;
    Refusal    refusal_ = Refusal::NotBound;
};

const wchar_t* Describe(Scope::Refusal refusal) noexcept;

}  // namespace lss::scopes::primary_controller
