#pragma once

// The lookups every camera module makes, once: a controller's End camera manager (checked by vtable and by its
// owner field naming the controller back), its Field and Battle operators (checked by vtable), the module in a slot
// of an operator's module table (TSharedPtr entries, 16 bytes each), which seat's camera manager an operator or a
// camera manager is (players/seats.h: 0 player 1, then the seats), and player 1's and a seat's operators of one kind
// for the mirrors (camera/field_camera.h, battle/battle_camera.h, camera_distance.h). The facts
// are facts/cameras.h; the vtables are rebased once at install. Nothing here is kept per seat: every seat lookup reads
// the controllers' fields as they stand (players/generation.h).
// Game thread, or any thread for the pure pointer walks (every read is guarded).

#include "players/seats.h"

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::camera::camera_access {

/// Rebases the camera manager's and the two operators' vtables; false if any is not where the research read it.
/// Idempotent: every module that looks a camera up calls it.
bool Install(const platform::GameImage& image) noexcept;
bool Installed() noexcept;

/// A controller's End camera manager, if it is one and names the controller as its owner; null otherwise.
void* CameraManagerOf(const void* controller) noexcept;

/// The camera manager's Field operator (12) or Battle operator (11), checked by vtable; null otherwise.
void* FieldOperatorOf(const void* cameraManager) noexcept;
void* BattleOperatorOf(const void* cameraManager) noexcept;

/// The module in `slot` of an operator's module table, or null (no operator, no table, an empty slot).
const void* ModuleAt(const void* op, std::uint32_t slot) noexcept;

/// The seat whose entry in `owners` (indexed by the seat, 0 player 1's) is `object`, or -1; a null object or entry
/// never matches. Pure: the seat lookups below fill `owners` from the controllers.
constexpr int SeatIn(const void* const (&owners)[players::kMaxPlayers], const void* object) noexcept
{
    if (object == nullptr)
    {
        return -1;
    }
    for (int seat = 0; seat < players::kMaxPlayers; ++seat)
    {
        if (owners[seat] == object)
        {
            return seat;
        }
    }
    return -1;
}

/// Which seat's camera manager `cameraManager` is: 0 player 1's, a built seat's while its generation is active, -1
/// none. By the controllers' camera-manager fields as they stand (needs no Install).
int SeatOfCameraManager(const void* cameraManager) noexcept;

/// Which seat's camera manager owns `op` as its Field or Battle operator: 0 player 1's, a built seat's while its
/// generation is active, -1 none. By the controllers' camera-manager fields as they stand (needs no Install).
int SeatOfOperator(const void* op) noexcept;

/// Player 1's operator and one seat's, of one kind.
struct OperatorPair
{
    void* operatorOne;
    void* seatOperator;
};

using OperatorOf = void* (*)(const void* cameraManager) noexcept;

/// Player 1's and `seat`'s operators (`operatorOf`: FieldOperatorOf or BattleOperatorOf), while the seat's generation
/// is active, its player holds a party member and the two are distinct; empty otherwise (a mirror then leaves the
/// engine's call alone for that seat). Game thread.
std::optional<OperatorPair> CurrentPair(int seat, OperatorOf operatorOf) noexcept;

}  // namespace lss::camera::camera_access
