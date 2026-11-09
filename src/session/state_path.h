#pragma once

// A member's own chain of level states ahead of its current setting, walked as the game's data lays it out.
//
// A level state row names the member's AI setting (its Status: a run "MoveToTarget", a "Lead", a "Stay", a "Stable"
// stand, a setting group, ...) and the states it moves on to (NextStateChange, one per trigger, and the swaps its enter
// may take instead). A hold on a member blocks only the member's own arrival signal, so it forecloses every state
// beyond the current one; the walk lists them. Breadth first from the state the arrival leads to, each state once, a
// path's end (session/field_hold.h PathEnd) ending its branch (the member stands or follows again: what follows is a
// later beat's script), at most `maxLinks` states. The caller supplies the row lookup (the game's state-change tables in
// the mod, a map in the tests) and reads what the walk found: the first further move (the lead gate's test,
// session/lead_gate.h) and every move with its destination (where the path goes).
//
// Pure (host-tested in tests/state_path_test.cpp).

#include "session/field_hold.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lss::session::state_path {

/// A state row as the walk needs it.
struct Row
{
    std::wstring              status;                // the AI setting's name
    std::vector<std::wstring> next;                  // the states it moves on to (trigger order, then its swaps)
    std::uint64_t             destination  = 0;      // a move's destination name (the game's FName bits; 0: none)
    bool                      groupMoves   = false;  // a setting group holding a run or lead that names a trigger
    bool                      groupFollows = false;  // a setting group that follows the party again
};

/// One state reached.
struct Link
{
    std::wstring  name;
    std::wstring  status;             // empty while `read` is false
    std::uint64_t destination = 0;
    std::size_t   parent      = 0;    // the link it was reached from (the first is its own parent)
    bool          read        = false;
    bool          move        = false;  // a further move (field_hold::MoveSetting)
};

struct Walk
{
    std::vector<Link>          links;
    std::optional<std::size_t> firstMove;          // the first further move reached, breadth first
    bool                       truncated = false;  // a state beyond the limit was left unwalked
};

/// Walks from `first`; `lookup(name)` answers std::optional<Row> (nullopt: the row is unknown).
template <typename Lookup>
Walk WalkFrom(const std::wstring& first, Lookup&& lookup, std::size_t maxLinks)
{
    Walk walk;
    if (first.empty() || maxLinks == 0)
    {
        return walk;
    }
    walk.links.push_back(Link{.name = first});
    for (std::size_t i = 0; i < walk.links.size(); ++i)
    {
        std::optional<Row> row = lookup(walk.links[i].name);
        if (!row)
        {
            continue;
        }
        Link& link       = walk.links[i];
        link.read        = true;
        link.status      = row->status;
        link.destination = row->destination;
        link.move        = field_hold::MoveSetting(row->status, row->groupMoves);
        if (!walk.firstMove && link.move)
        {
            walk.firstMove = i;
        }
        if (field_hold::PathEnd(row->status, row->groupFollows))
        {
            continue;  // the member stands or follows again: this branch of the path ends here
        }
        for (std::wstring& next : row->next)
        {
            const bool seen = next.empty() || std::any_of(walk.links.begin(), walk.links.end(),
                                                          [&next](const Link& l) { return l.name == next; });
            if (seen)
            {
                continue;
            }
            if (walk.links.size() >= maxLinks)
            {
                walk.truncated = true;
                break;
            }
            walk.links.push_back(Link{.name = std::move(next), .parent = i});
        }
    }
    return walk;
}

/// A further move ahead, or a chain still going past the limit (read as one: missing a path stalls the level, handing
/// a placement to the AI only moves the member).
inline bool FurtherMove(const Walk& walk) noexcept
{
    return walk.firstMove.has_value() || walk.truncated;
}

/// The links from the first to `at`, parent by parent, in walking order.
inline std::vector<std::size_t> Trail(const Walk& walk, std::size_t at)
{
    std::vector<std::size_t> trail;
    if (at >= walk.links.size())
    {
        return trail;
    }
    for (;;)
    {
        trail.push_back(at);
        if (at == 0)
        {
            break;
        }
        at = walk.links[at].parent;
    }
    std::reverse(trail.begin(), trail.end());
    return trail;
}

}  // namespace lss::session::state_path
