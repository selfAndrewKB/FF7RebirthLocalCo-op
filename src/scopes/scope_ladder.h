#pragma once

// Player 2's scopes opened in one order for one game call, all or nothing.
//
// Every "run this engine call as player 2" site opens the same ladder of scopes: the UI focus context
// first, then pad 2's sticks, player 2 as the primary controller, pad 2's buttons, player 2's target
// fields, the party leader and its type, and last the rules the call runs under. Each site used to
// write the ladder out by hand (the 2026-10 audit's D-3, E1-3 and C-10: seven copies and three
// bundles), each with its own order and refusal text. A Ladder opens the parts chosen by `which` in
// the order its parameter pack gives; at the first required part that does not hold it closes the
// parts opened before it, in reverse, and opens nothing after it (all or nothing: not held means
// nothing of it is open). A part may be optional: it is opened, and a refusal leaves it out without
// stopping the ladder, which is still held. The refused part names itself, with its own words when it
// has them.
//
// A Part is any type with
//   static constexpr unsigned       kBit;   the bit `which` selects it by
//   static constexpr const wchar_t* kName;  what it answers (the refusal, unless it says more)
//   explicit Part(const Args&);             opens it (Args is the ladder's one argument struct)
//   bool Held() const;                      whether it opened
// and may add `const wchar_t* Refusal() const` (why it did not open; null when it did).
//
// Pure: tests/scope_ladder_test.cpp pins the order and the all-or-nothing rule with recording parts.
// scopes/seat_ladder.h gives the parts over the modules' scope types.

#include <cstddef>
#include <optional>
#include <tuple>

namespace lss::scopes {

template <class Args, class... Parts>
class Ladder
{
public:
    /// Opens the parts of `which` in order; the `optional` ones (bits within `which`) may refuse without
    /// stopping it.
    Ladder(unsigned which, unsigned optional, const Args& args) noexcept
    {
        Open<0>(which, optional, args);
    }
    Ladder(unsigned which, const Args& args) noexcept : Ladder(which, 0u, args) {}
    ~Ladder()
    {
        Close<sizeof...(Parts)>();
    }
    Ladder(const Ladder&)            = delete;
    Ladder& operator=(const Ladder&) = delete;

    /// Every required part of `which` holds.
    bool Held() const noexcept { return held_; }

    /// Why the ladder is not held: the required part that refused (its own words when it has them, else
    /// its name); null when held.
    const wchar_t* Refusal() const noexcept { return refusal_; }

    /// The first optional part that refused (its words or its name), or null: for a site that counts it.
    const wchar_t* Noted() const noexcept { return noted_; }

    /// Part `P` was opened and holds.
    template <class P>
    bool Holds() const noexcept
    {
        return std::get<std::optional<P>>(parts_).has_value();
    }

    /// Part `P` while it holds, or null.
    template <class P>
    P* Get() noexcept
    {
        auto& part = std::get<std::optional<P>>(parts_);
        return part ? &*part : nullptr;
    }
    template <class P>
    const P* Get() const noexcept
    {
        const auto& part = std::get<std::optional<P>>(parts_);
        return part ? &*part : nullptr;
    }

private:
    template <std::size_t I>
    void Open(unsigned which, unsigned optional, const Args& args) noexcept
    {
        if constexpr (I < sizeof...(Parts))
        {
            using P = std::tuple_element_t<I, std::tuple<Parts...>>;
            if ((which & P::kBit) != 0)
            {
                auto& part = std::get<I>(parts_);
                part.emplace(args);
                if (!part->Held())
                {
                    const wchar_t* why = P::kName;
                    if constexpr (requires { part->Refusal(); })
                    {
                        if (part->Refusal() != nullptr)
                        {
                            why = part->Refusal();
                        }
                    }
                    part.reset();
                    if ((optional & P::kBit) == 0)
                    {
                        refusal_ = why;
                        Close<I>();  // the parts before it, in reverse: nothing of the ladder stays open
                        return;      // nothing after it opens; held_ stays false
                    }
                    if (noted_ == nullptr)
                    {
                        noted_ = why;  // optional: left out, the ladder goes on
                    }
                }
            }
            Open<I + 1>(which, optional, args);
        }
        else
        {
            held_ = true;
        }
    }

    template <std::size_t I>
    void Close() noexcept
    {
        if constexpr (I > 0)
        {
            std::get<I - 1>(parts_).reset();
            Close<I - 1>();
        }
    }

    std::tuple<std::optional<Parts>...> parts_;
    bool                                held_    = false;
    const wchar_t*                      refusal_ = nullptr;
    const wchar_t*                      noted_   = nullptr;
};

}  // namespace lss::scopes
