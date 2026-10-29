#pragma once

// The traversal camera's state its units share (traversal/traversal_camera.h): the "inside a scoped call"
// flag, the executer and actor readers, each seat's last ladder module, and each unit's placement, removal and tick,
// which traversal_camera.cpp composes into the module's Install, Remove and Tick. Internal; not a public API.
//
// Per seat (players/seats.h): the ladder module and each unit's per-seat records, indexed by the seat (index 0, player
// 1's, unused); the seat comes from the executer's or move's character (SeatOfExecuter) or, on the anim worker, from
// climb_context's climber. One scoped call runs at a time on the game thread, whichever seat's: g_inside is one flag.

#include "traversal/traversal_facts.h"

#include "platform/image.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_scope.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::traversal::traversal_camera {

inline bool g_inside = false;  // inside a scoped call of a seat's (game thread)

/// Holds g_inside for one scoped call; reset on every way out.
struct Inside
{
    Inside() noexcept { g_inside = true; }
    ~Inside() { g_inside = false; }
    Inside(const Inside&)            = delete;
    Inside& operator=(const Inside&) = delete;
};

// Each seat's last ladder Start's module, by seat (written by the executers on the game thread; the collision watch,
// a dev tool, reads player 2's).
inline const void* g_ladderModule[players::kMaxPlayers] = {};

inline void* ObjectIn(const void* object, std::uint32_t field) noexcept
{
    return object != nullptr ? scopes::leader_scope::ObjectAt(static_cast<const std::byte*>(object) + field) : nullptr;
}

/// The built seat whose active member is the executer's character, or -1 (game thread; -1 on any other).
inline int SeatOfExecuter(void* executer) noexcept
{
    if (!players::generation::OnGameThread())
    {
        return -1;
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* member = players::generation::MemberOf(seat);
        if (member != nullptr && players::generation::Active(seat) && ObjectIn(executer, kExecCharacter) == member)
        {
            return seat;
        }
    }
    return -1;
}

inline std::wstring ActorName(const void* actor)
{
    return actor != nullptr ? players::NameOf(actor) : std::wstring(L"(no actor of the class)");
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

/// Game thread, per frame, per built seat: with the seat out, its ladder module and one-shot actor are forgotten.
void Tick(int seat, bool active) noexcept;

}  // namespace executers

namespace holders {

/// What the move classes' placement found, for traversal_camera.cpp's lines.
struct Placement
{
    std::wstring classes;             // "climbing wall yes, monkey bars yes, ..."
    bool         twoHolders = false;  // the seats' own holders are on (every hook placed)
    bool         hip        = false;  // the climbing wall's hip calls answer for each seat
};

/// Replaces the move classes' slots, the hip calls and the seats' holder redirects (each checked).
Placement Place(const platform::GameImage& image);

/// Releases every seat's holders and restores the slots; true when one was changed after us and is left in place.
bool Remove() noexcept;

/// Game thread, per frame, per built seat: an idle holder of the seat's is released; with the seat out, all of them.
void Tick(int seat, bool active) noexcept;

}  // namespace holders

}  // namespace lss::traversal::traversal_camera
