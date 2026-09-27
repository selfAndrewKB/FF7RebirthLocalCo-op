#pragma once

// Player 2's own UI focus context (RE-p2-command-menu-input.md §2, §3.3, §11, pass S-CMD2).
//
// UI actions reach one focus path only, kept by Square's input object (EndUISystem[2]) with a stack of
// focus layers; the game has one such context. Player 2's command menu (a second FieldCommandCtrl)
// calls the same focus functions as player 1's when it is built, opens and closes, so for the length
// of those calls player 2's context is swapped in: the focus path, the layers, the cause byte and the
// capture map (the held actions' capturers: each player's own, as the command menu captures every held
// Attack, Unique and Guard) are exchanged with player 2's saved copy (empty at first), the input
// object's menu-core delegate is unbound (player 2 never runs player 1's cinema handler), and the UI
// system's command controller slot names player 2's. Weak-pointer counts travel with the bytes, so
// nothing is added or released. The scope is refused inside another scope or without the input
// object. Game thread only.

#include <array>
#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::player_two::focus_scope {

bool Install(const platform::GameImage& image) noexcept;

/// Player 2's key guide (a second KeyGuide53 built with the game's constructor), or null: while set, a scope
/// also names it in the UI system's key guide slot (EndUISystem[53]), so player 2's controller drives its own.
void SetKeyGuide(void* keyGuide) noexcept;

/// Player 2's saved context: its path length and layer count (for the log).
struct Summary
{
    std::int32_t pathCount = 0, layerCount = 0;
};
Summary PlayerTwoContext() noexcept;

/// The focus path length in the input object now (inside a scope: player 2's live one).
std::int32_t LivePathCount() noexcept;

/// Player 1's top focus layer is the command menu's own (priority -1000): no pause menu, popup or
/// other modal layer above it. Outside a scope.
bool PlayerOneTopIsCommandMenu() noexcept;

class FocusScope
{
public:
    /// Swaps in player 2's context with `controller` as the UI system's command controller.
    explicit FocusScope(void* controller) noexcept;
    ~FocusScope();
    FocusScope(const FocusScope&)            = delete;
    FocusScope& operator=(const FocusScope&) = delete;

    bool Entered() const noexcept { return entered_; }
    /// Why the scope was refused (null when entered).
    const wchar_t* Refusal() const noexcept { return refusal_; }
    /// Player 2's context is not kept at the end (it is reset to empty): the game faulted inside the
    /// scope, possibly mid-change. What it held is leaked, never replayed.
    void Abandon() noexcept { abandon_ = true; }

private:
    static constexpr std::size_t kContextSize  = 32 + 16 + 1 + 0x50;  // path, layers, cause, capture map
    static constexpr std::size_t kDelegateSize = 16;

    bool                                   entered_ = false;
    bool                                   abandon_ = false;
    const wchar_t*                         refusal_ = nullptr;
    std::uintptr_t                         input_   = 0;
    std::uintptr_t                         savedController_ = 0;
    std::uintptr_t                         savedKeyGuide_   = 0;
    std::array<std::uint8_t, kContextSize>  savedContext_{};   // player 1's
    std::array<std::uint8_t, kDelegateSize> savedDelegate_{};
};

}  // namespace lss::player_two::focus_scope
