#pragma once

// The lookups every camera module makes, once: a controller's End camera manager (checked by vtable and by its
// owner field naming the controller back), its Field and Battle operators (checked by vtable), the module in a slot
// of an operator's module table (TSharedPtr entries, 16 bytes each), which player's camera manager an operator
// belongs to, and both players' operators of one kind for the mirrors (player_two/field_camera.h,
// battle_camera.h, camera_distance.h). The facts are facts/cameras.h; the vtables are rebased once at install.
// Game thread, or any thread for the pure pointer walks (every read is guarded).

#include <cstdint>
#include <optional>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::camera_access {

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

/// Which player's camera manager owns `op` as its Field or Battle operator: 0 player 1, 1 player 2 (while player 2 is
/// in), -1 neither. By the controllers' camera-manager fields as they stand (needs no Install).
int PlayerOfOperator(const void* op) noexcept;

/// Player 1's and player 2's operators of one kind.
struct OperatorPair
{
    void* one;
    void* two;
};

using OperatorOf = void* (*)(const void* cameraManager) noexcept;

/// Both players' operators (`operatorOf`: FieldOperatorOf or BattleOperatorOf), while player 2 holds a party member
/// and the two are distinct; empty otherwise (a mirror then leaves the engine's call alone). Game thread.
std::optional<OperatorPair> CurrentPair(OperatorOf operatorOf) noexcept;

}  // namespace lss::player_two::camera_access
