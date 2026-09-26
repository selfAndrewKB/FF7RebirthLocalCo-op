#pragma once

// The running mod as a control script sees it: each command does what its key does, but asks for a
// state (own camera on, per-pad sticks on) rather than switching it, and says why when it cannot.
// Game thread only.

#include "control/runner.h"

#include <array>
#include <cstdint>

namespace lss::player_two {
class Session;
}

namespace lss::players {
class SplitLayout;
}

namespace lss::control {

class ModTarget final : public Target
{
public:
    ModTarget(player_two::Session& session, players::SplitLayout& layout, bool viewPointInstalled) noexcept
        : session_(session), layout_(layout), viewPointInstalled_(viewPointInstalled)
    {
    }

    bool HasPlayerTwo() const override;
    bool PlayerTwoPlaying() const override;

    Outcome Join() override;
    Outcome Leave() override;
    Outcome SetOwnCamera(bool on) override;
    Outcome SetPerPadSticks(bool on) override;
    Outcome SetLayout(bool sideBySide) override;
    Outcome SetBars(bool on) override;
    Outcome SetSplitFov(float blend) override;
    Outcome SetHudBand(bool on) override;
    Outcome SetHudProbe(bool on) override;
    Outcome TraceHud(std::int32_t frames) override;
    Outcome SetHudImage(bool on) override;
    Outcome SetHudLevers(bool on) override;
    Outcome SetHudLeversMask(std::uint32_t mask) override;
    Outcome SetSticksMask(std::uint32_t mask) override;
    Outcome SetLeakFixes(std::uint32_t mask) override;
    Outcome SetButtonsMask(std::uint32_t mask) override;
    Outcome SetIconsMask(std::uint32_t mask) override;
    Outcome SetBattleMask(std::uint32_t mask) override;
    Outcome SetPerfLog(bool on) override;
    Outcome Dump(const std::wstring& topic) override;
    Outcome HoldSticks(std::int32_t player, const float (&axes)[4]) override;
    void    ReleaseSticks(std::int32_t player) override;
    Outcome Console(const std::wstring& command) override;
    Outcome Quit() override;
    Outcome Status() override;
    Outcome Note(const std::wstring& text) override;

    /// Clears every stick hold made during a join that has since ended (a leave by key, a map load),
    /// so a later player 2 on the same pad never inherits it. Once per tick, after the session's
    /// tick and before the world's input runs.
    void DropEndedHolds() noexcept;

private:
    player_two::Session&         session_;
    players::SplitLayout&        layout_;
    bool                        viewPointInstalled_;
    std::array<std::int32_t, 2>  heldIds_{-1, -1};  // per player: the controller id its stick hold is on
    std::array<std::uint32_t, 2> heldJoins_{};      // per player: the join its stick hold was made in
};

}  // namespace lss::control
