#pragma once

// Each further player's own UI focus context (RE-p2-command-menu-input.md §2, §3.3, §11, pass S-CMD2). Every
// seat has its own saved context and key guide (indexed by the seat, index 0 unused: player 1's
// context is the game's own, in place outside a scope). One scope at a time, whichever seat's: the seats swap in turn,
// never one inside another's.
//
// UI actions reach one focus path only, kept by Square's input object (EndUISystem[2]) with a stack of
// focus layers; the game has one such context. A seat's command menu (another FieldCommandCtrl)
// calls the same focus functions as player 1's when it is built, opens and closes, so for the length
// of those calls the seat's context is swapped in: the focus path, the layers, the cause byte, the
// capture map (the held actions' capturers: each player's own, as the command menu captures every held
// Attack, Unique and Guard) and the menu owner byte (+0x64, per-pad input state the member selecter
// writes) are exchanged with the seat's saved copy (empty at first), the input
// object's menu-core delegate is unbound (a further player never runs player 1's cinema handler), and the UI
// system's command controller slot names the seat's. Weak-pointer counts travel with the bytes, so
// nothing is added or released. The scope is refused inside another scope or without the input
// object. One byte context (platform/byte_scope.h) holds it all: the five regions as Exchange
// ranges, the delegate and the two UI system slots as Set ranges (a slot the game wrote inside the
// scope keeps the game's value, noted). Game thread only.

#include "platform/byte_scope.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::battle::focus_scope {

bool Install(const platform::GameImage& image) noexcept;

// A seat's context as copied: the path, the layers, the cause, the capture map and the owner byte; and the
// menu-core delegate's size.
inline constexpr std::size_t kContextSize  = 32 + 16 + 1 + 0x50 + 1;
inline constexpr std::size_t kDelegateSize = 16;

/// `seat`'s key guide (another KeyGuide53 built with the game's constructor), or null: while set, the seat's scope
/// also names it in the UI system's key guide slot (EndUISystem[53]), so the seat's controller drives its own.
void SetKeyGuide(int seat, void* keyGuide) noexcept;

/// A seat's saved context: its path length and layer count (for the log).
struct Summary
{
    std::int32_t pathCount = 0, layerCount = 0;
};
Summary ContextOf(int seat) noexcept;

/// The focus path length in the input object now (inside a scope: that seat's live one).
std::int32_t LivePathCount() noexcept;

/// Player 1's top focus layer is the command menu's own (priority -1000): no pause menu, popup or
/// other modal layer above it. Outside a scope.
bool PlayerOneTopIsCommandMenu() noexcept;

class FocusScope
{
public:
    /// Swaps in `seat`'s context with `controller` (the seat's command controller) as the UI system's.
    FocusScope(int seat, void* controller) noexcept;
    ~FocusScope();
    FocusScope(const FocusScope&)            = delete;
    FocusScope& operator=(const FocusScope&) = delete;

    bool Entered() const noexcept { return entered_; }
    /// Why the scope was refused (null when entered).
    const wchar_t* Refusal() const noexcept { return refusal_; }
    /// The seat's context is not kept at the end (it is reset to empty): the game faulted inside the
    /// scope, possibly mid-change. What it held is leaked, never replayed.
    void Abandon() noexcept { abandon_ = true; }

private:
    // The five regions, the delegate, the controller slot and the key guide slot.
    platform::ByteStore<8, kContextSize + kDelegateSize + 2 * sizeof(std::uintptr_t)> context_;
    int                                                                               seat_    = 0;
    bool                                                                              entered_ = false;
    bool                                                                              abandon_ = false;
    const wchar_t*                                                                    refusal_ = nullptr;
};

}  // namespace lss::battle::focus_scope
