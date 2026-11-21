#pragma once

// Players 2 and 3's camera responsiveness (research/RE-camera-responsiveness-per-player.md). The game's Responsiveness
// option (1 to 5) is one byte of its system options, read every frame in two places: each controller's input step (the
// right stick turns by the level times the controller's rate) and the camera modules' stick smoothing in each camera
// manager's update (which divides that level back out). A seat's input step (input/pad_sticks) and its camera manager's
// update (camera/field_camera) each run inside Scope(seat), which holds the seat's own level in that byte for the call
// and puts player 1's back after (platform/byte_scope.h, Hold: a game write inside is undone and counted). Player 1's
// option, its row and every other reader stay the game's. The levels are the settings file's responsiveness options
// (platform/settings.h), also set by the console; a change applies from the seat's next frame. Logged at Info when a level is set, a warning once if the game wrote the byte
// inside a seat's scope. Game thread.

#include <string>

namespace lss::platform {
class GameImage;
}

namespace lss::camera::seat_responsiveness {

/// Finds the game's option getter, reads the levels from the settings file and registers their console commands. False
/// if the getter is not what the research read (the seats then turn at player 1's level).
bool Install(const platform::GameImage& image) noexcept;

/// The seat's level in the game's Responsiveness byte for the scope's life, player 1's put back after. Holds nothing
/// for player 1, for a seat without an option of its own, off the game thread or inside the same seat's open scope.
class Scope
{
public:
    explicit Scope(int seat) noexcept;
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int  seat_ = 0;
    bool held_ = false;
};

/// For the console: player 1's byte, players 2 and 3's levels, the scopes held and the game writes undone.
std::wstring Reading();

}  // namespace lss::camera::seat_responsiveness
