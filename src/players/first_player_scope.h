#pragma once

// A seat's controller first in the world's player controller list for one call, so "the first local player" the game
// asks for is the seat's; player 1's order put back after (research/RE-seats-step6-world-rides.md).
//
// UWorld::GetFirstLocalPlayerFromControllers walks the list for the first controller whose player is local: player
// 1's, always. A game call that projects a point through it (the world items' screen-visibility request, the chocobo
// jet gauge's placement) sees player 1's view for every player. The scope swaps the list's first entry with the
// seat's (two Set ranges of platform/byte_scope.h, each holding the other's), so the call's one reader takes the
// seat's controller; it opens only when the first entry resolves to player 1's controller and a later one to the
// seat's, and restores only if the list is still where it was and names them swapped (a list the engine moved or
// changed in between is left alone). A join appends to the list and a leave closes it up in order, so a seat's place
// depends on who joined and left before it (player 1 always first); the seat's entry is found by its controller,
// never by its position. Held by world/field_pickup and hud/field/jet_gauges. Game thread.

#include "facts/engine.h"
#include "platform/byte_scope.h"
#include "platform/guarded.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/weak_object.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace lss::players {

class FirstPlayerScope
{
public:
    explicit FirstPlayerScope(int seat) noexcept
    {
        using Kind  = platform::ByteContext::Kind;
        using Range = platform::ByteContext::Range;
        void*         one        = generation::PlayerOneController();
        void*         controller = generation::ControllerOf(seat);
        auto*         entries    = Entries(controller);
        const auto    at         = entries != nullptr ? EntryOf(controller, entries) : 0;
        std::uint64_t first = 0, mine = 0;
        if (one == nullptr || entries == nullptr || at <= 0 || weak_object::ObjectAt(entries) != one ||
            !platform::CopyGuarded(&first, entries, sizeof(first)) ||
            !platform::CopyGuarded(&mine, entries + sizeof(std::uint64_t) * at, sizeof(mine)))
        {
            return;
        }
        const Range swapped[] = {{reinterpret_cast<std::uintptr_t>(entries), sizeof(mine), Kind::Set},
                                 {reinterpret_cast<std::uintptr_t>(entries + sizeof(std::uint64_t) * at), sizeof(first), Kind::Set}};
        if (!context_.Define(swapped))
        {
            return;
        }
        std::memcpy(context_.Value(0).data(), &mine, sizeof(mine));
        std::memcpy(context_.Value(1).data(), &first, sizeof(first));
        one_        = one;
        controller_ = controller;
        at_         = at;
        held_       = context_.Open();
    }
    ~FirstPlayerScope()
    {
        if (!held_)
        {
            return;
        }
        // The list as it stands after the call (which may have changed it): put back only if it is the same list and
        // still names them swapped; else forgotten (nothing of the old order written anywhere).
        auto* entries = Entries(controller_);
        if (reinterpret_cast<std::uintptr_t>(entries) == context_.RangeAt(0).live &&
            weak_object::ObjectAt(entries) == controller_ &&
            weak_object::ObjectAt(entries + sizeof(std::uint64_t) * at_) == one_)
        {
            context_.Close();
        }
        else
        {
            context_.Forget();
        }
    }
    FirstPlayerScope(const FirstPlayerScope&)            = delete;
    FirstPlayerScope& operator=(const FirstPlayerScope&) = delete;

    bool Held() const noexcept { return held_; }

private:
    /// The most entries searched for a seat's controller (a bound on a list read from the game; it holds the player
    /// controllers [ASSUMPTION that nothing else is in it]).
    static constexpr std::int32_t kMaxSearched = 16;

    /// The world the controller lives in (controller -> level -> world), or null.
    static const void* WorldOf(const void* controller) noexcept
    {
        return ReadPointer(ReadPointer(controller, facts::kObjectOuter), facts::kObjectOuter);
    }

    /// The world's controller list entries, or null when it has fewer than two.
    static std::uint8_t* Entries(const void* controller) noexcept
    {
        const void* world = WorldOf(controller);
        if (controller == nullptr || world == nullptr ||
            ReadField<std::int32_t>(world, facts::kWorldPlayerControllerCount) < 2)
        {
            return nullptr;
        }
        return static_cast<std::uint8_t*>(ReadPointer(world, facts::kWorldPlayerControllers));
    }

    /// The index of `controller`'s entry in the list (1.., never 0: that one must be player 1's), or 0 if not found.
    static std::int32_t EntryOf(const void* controller, const std::uint8_t* entries) noexcept
    {
        const auto count = std::min(ReadField<std::int32_t>(WorldOf(controller), facts::kWorldPlayerControllerCount),
                                    kMaxSearched);
        for (std::int32_t i = 1; i < count; ++i)
        {
            if (weak_object::ObjectAt(entries + sizeof(std::uint64_t) * i) == controller)
            {
                return i;
            }
        }
        return 0;
    }

    platform::ByteStore<2, 2 * sizeof(std::uint64_t)> context_;
    void*                                             one_        = nullptr;
    void*                                             controller_ = nullptr;
    std::int32_t                                      at_         = 0;  // the seat's entry
    bool                                              held_       = false;
};

}  // namespace lss::players
