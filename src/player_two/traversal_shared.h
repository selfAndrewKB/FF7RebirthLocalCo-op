#pragma once

// The traversal camera's state its units share (player_two/traversal_camera.h): the "inside a scoped call" flag,
// the executer and actor readers, player 2's last ladder module, and each unit's placement, removal and tick, which
// traversal_camera.cpp composes into the module's Install, Remove and Tick. Internal; not a public API.

#include "player_two/traversal_facts.h"

#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "platform/image.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::player_two::traversal_camera {

inline bool g_inside = false;  // inside a scoped call of player 2's (game thread)

/// Holds g_inside for one scoped call; reset on every way out.
struct Inside
{
    Inside() noexcept { g_inside = true; }
    ~Inside() { g_inside = false; }
    Inside(const Inside&)            = delete;
    Inside& operator=(const Inside&) = delete;
};

// Player 2's last ladder Start's module (written by the executers on the game thread; the collision watch reads it).
inline const void* g_ladderModule = nullptr;

inline void* ObjectIn(const void* object, std::uint32_t field) noexcept
{
    return object != nullptr ? leader_scope::ObjectAt(static_cast<const std::byte*>(object) + field) : nullptr;
}

/// The executer's character is player 2's member (game thread).
inline bool PlayerTwos(void* executer) noexcept
{
    void* member = generation::Member();
    return member != nullptr && generation::Active() && generation::OnGameThread() &&
           ObjectIn(executer, kExecCharacter) == member;
}

inline std::wstring ActorName(const void* actor)
{
    return actor != nullptr ? NameOf(actor) : std::wstring(L"(no actor of the class)");
}

inline std::uintptr_t Live(const platform::GameImage& image, std::uintptr_t research, std::size_t size) noexcept
{
    const auto at = image.Rebase(research, size);
    return at ? *at : 0;
}

namespace executers {

/// What the executer classes' placement found, for traversal_camera.cpp's lines.
struct Placement
{
    std::wstring classes;               // "ladder yes, squeeze NO, ..."
    bool         oneShotLeave = false;  // the one-shot actor's leave slot replaced
    bool         ladderExit   = false;  // the game's exit-blend request found
    bool         squeezeExit  = false;
    bool         crawlOff     = false;
};

/// Replaces the executers' slots and finds the exit calls (each checked).
Placement Place(const platform::GameImage& image);

/// Restores the executers' slots; true when one was changed after us and is left in place.
bool Remove() noexcept;

/// Game thread, per frame: with player 2 out, the ladder module and the one-shot's actor are forgotten.
void Tick(bool active) noexcept;

}  // namespace executers

namespace holders {

/// What the move classes' placement found, for traversal_camera.cpp's lines.
struct Placement
{
    std::wstring classes;             // "climbing wall yes, monkey bars yes, ..."
    bool         twoHolders = false;  // player 2's own holders are on (every hook placed)
    bool         hip        = false;  // the climbing wall's hip calls answer for player 2
};

/// Replaces the move classes' slots, the hip calls and player 2's holder redirects (each checked).
Placement Place(const platform::GameImage& image);

/// Releases player 2's holders and restores the slots; true when one was changed after us and is left in place.
bool Remove() noexcept;

/// Game thread, per frame: an idle holder of player 2's is released; with player 2 out, all of them.
void Tick(bool active) noexcept;

}  // namespace holders

}  // namespace lss::player_two::traversal_camera
