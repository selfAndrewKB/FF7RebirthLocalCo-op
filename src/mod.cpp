// LocalSplitScreen: the UE4SS entry point.
//
// This file wires the modules together and holds no logic of its own:
//   1. verify every engine fact the mod relies on (platform/engine_addresses.h);
//   2. if all hold, install the view-family submission (renderer/) and start listening for
//      player intent on the game thread (players/); if any fails, install nothing.
// Design: Rebirth/Documentation/SPLITSCREEN-ARCHITECTURE.md

#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "dev/dev_tools.h"
#include "hud/hud_placement.h"
#include "hud/layout_scale.h"
#include "hud/overlay_layout.h"
#include "hud/player_two_pass.h"
#include "hud/area_icons_two.h"
#include "hud/target_views.h"
#include "hud/prompt_rows.h"
#include "hud/root_overlay_render.h"
#include "hud/second_image.h"
#include "hud/user_widgets.h"
#include "platform/engine_addresses.h"
#include "platform/frame_watch.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "player_two/field_exit.h"
#include "player_two/session.h"
#include "players/join_keys.h"
#include "players/split_layout.h"
#include "players/split_screen_settings.h"
#include "players/unreal_access.h"
#include "renderer/additional_family_output.h"
#include "renderer/null_view_probe.h"
#include "renderer/view_family_submission.h"
#include "views/player_view_point.h"
#include "views/view_shape.h"

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

namespace Hook = RC::Unreal::Hook;
using lss::platform::Log;

constexpr const wchar_t* kVersion = L"0.28.5-game-ready-gate";

/// How often player 2's status line is written while player 2 is in.
constexpr ULONGLONG kStatusIntervalMs = 10'000;

/// How often, in engine ticks, the split halves' shape is read while player 2 is in.
constexpr std::uint32_t kHalfShapeIntervalTicks = 30;

/// Checks every engine fact against the running game and logs each one that fails.
/// Returns the live address of RemoveLocalPlayer when all hold.
std::optional<std::uintptr_t> VerifyEngine(const lss::platform::GameImage& image) noexcept
{
    bool allHold = true;
    for (const auto& fact : lss::engine::kVerifiedCode)
    {
        if (!image.BytesMatch(fact.idaAddress, fact.bytes))
        {
            Log(L"check failed: {} (IDA 0x{:X}) does not have the expected bytes", fact.what, fact.idaAddress);
            allHold = false;
        }
    }

    const auto entryHolds = image.ReadPointer(lss::engine::kBeginRenderingViewFamilyEntry);
    const auto engineFn   = image.Rebase(lss::engine::kBeginRenderingViewFamily);
    if (!entryHolds || !engineFn || *entryHolds != *engineFn)
    {
        Log(L"check failed: the renderer's vtable entry does not hold BeginRenderingViewFamily "
            L"(a game update, or another mod already replaced it)");
        allHold = false;
    }

    // The GetViewPoint entry is not checked here: the split screen works without it (with bars),
    // and its install is itself a compare-and-swap against the expected function
    // (views/player_view_point).

    const auto removeLocalPlayer = image.Rebase(lss::engine::kRemoveLocalPlayer);
    if (!allHold || !removeLocalPlayer)
    {
        return std::nullopt;
    }
    return removeLocalPlayer;
}

class LocalSplitScreenMod final : public RC::CppUserModBase
{
public:
    LocalSplitScreenMod()
    {
        ModName        = STR("LocalSplitScreen");
        ModVersion     = STR("0.28.5-game-ready-gate");
        ModDescription = STR("Two-player split-screen, one view family per player");
        ModAuthors     = STR("Andrew");
    }

    ~LocalSplitScreenMod() override
    {
        for (const auto id : {tickCallback_, loadMapCallback_})
        {
            if (id != Hook::ERROR_ID)
            {
                Hook::UnregisterCallback(id);
            }
        }

        // With player 2 still in, the engine keeps rendering two views and calling player 2's
        // wrappers. Putting the engine's own submission back would render them in one family, the
        // configuration this mod exists to avoid, and player 2's camera manager depends on its
        // wrappers. Removing player 2 here is not an option: the destructor may run off the game
        // thread. So everything stays installed; the DLL is pinned, so it stays valid.
        if (session_ && session_->HasPlayerTwo())
        {
            Log(L"unloading with player 2 still in: the per-player rendering and player 2's stages stay installed");
            return;
        }
        if (session_)
        {
            session_->RemoveStages();
        }
        if (viewPointInstalled_)
        {
            lss::views::RemovePlayerViewPoint();
        }
        lss::hud::RemoveRootOverlayRender();
        lss::player_two::field_exit::Remove();
        lss::hud::target_views::Remove();
        lss::hud::area_icons_two::Remove();
        lss::hud::user_widgets::Remove();
        lss::hud::RemoveSecondImage();
        lss::hud::RemovePlayerTwoPass();
        lss::dev::Remove();
        lss::hud::RemoveLayoutScale();
        lss::hud::RemoveOverlayLayout();
        lss::renderer::RemoveNullViewProbe();
        if (rendererInstalled_)
        {
            lss::renderer::RemoveViewFamilySubmission();
            lss::renderer::RemoveAdditionalFamilyOutput();
        }
    }

    auto on_unreal_init() -> void override
    {
        Log(L"v{} starting", kVersion);

        // A measuring control: with `watch_only.txt` in the mod's folder, nothing is installed but the
        // frame-hitch watch, so the game's own frame times can be compared with the mod's.
        std::error_code missing;
        if (std::filesystem::exists(lss::platform::ModFolder() / L"watch_only.txt", missing))
        {
            Log(L"watch only (watch_only.txt): nothing installed except the frame-hitch watch; the game runs unmodified");
            // The control channel alone (no engine redirects), so a script can still quit the game.
            const auto image = lss::platform::GameImage::Locate();
            if (const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt)
            {
                session_.emplace(*removeLocalPlayer);
                lss::dev::InstallControl(*session_, layout_, false);
            }
            const Hook::FCallbackOptions watchOptions{false, false, STR("LocalSplitScreen"), STR("frame watch")};
            tickCallback_ = Hook::RegisterEngineTickPreCallback(
                [](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) {
                    lss::platform::frame_watch::OnFrameStart();
                    lss::dev::TickBeforeSession();
                },
                watchOptions);
            return;
        }

        const auto image = lss::platform::GameImage::Locate();
        const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt;
        if (!removeLocalPlayer || !InstallRenderer(*image))
        {
            Log(L"standing aside: nothing installed, the game runs unmodified");
            return;
        }
        lss::renderer::InstallNullViewProbe(*image);  // diagnosis of the main-menu crash; logs its own outcome
        session_.emplace(*removeLocalPlayer);
        session_->InstallStages(*image);

        // The view-point entry serves the filled halves and the choice of player 2's camera. The
        // split screen runs without it (halves kept 16:9 with bars, player 2 on its own camera);
        // those keys then report that they are unavailable.
        viewPointInstalled_ = lss::views::InstallPlayerViewPoint(*image);
        if (!viewPointInstalled_)
        {
            Log(L"ULocalPlayer::GetViewPoint's entry does not hold the engine's function; filled halves and the "
                L"camera choice are unavailable");
        }

        // The HUD's layout in split views (hud/overlay_layout.h). Optional like the view point: without
        // it the HUD keeps the game's full-screen layout, cropped to each half.
        if (lss::hud::InstallOverlayLayout(*image))
        {
            lss::hud::InstallLayoutScale(*image);  // the contain rule; unchanged for 16:9
        }
        lss::hud::user_widgets::Install();          // HUD widgets as the engine constructs them
        lss::hud::InstallSecondImage(*image);        // player 2's HUD image, on (the hud-image command switches it)
        lss::hud::InstallPlayerTwoPass(*image);      // player 2's HUD content, on (the hud-levers command switches it)
        lss::hud::InstallRootOverlayRender(*image);  // before the overlay renders: placement follow, trace
        lss::hud::prompt_rows::Install(*image);      // each player's field prompt rows in their own half
        lss::hud::area_icons_two::Install(*image);   // player 2's own map icons and markers
        lss::hud::target_views::Install(*image);     // each half's cursor and enemy gauges follow its player's target

        lss::dev::Install(*image, *session_, layout_, viewPointInstalled_);  // built with LSS_DEV_TOOLS only

        const Hook::FCallbackOptions tickOptions{false, false, STR("LocalSplitScreen"), STR("player intent")};
        tickCallback_ = Hook::RegisterEngineTickPreCallback(
            [this](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { OnEngineTick(); },
            tickOptions);

        // Player 2 leaves before any map load: the new world would log player 2 in again, and its
        // camera manager's fixed-name spawns would collide with player 1's outside a join.
        const Hook::FCallbackOptions loadOptions{false, false, STR("LocalSplitScreen"), STR("leave before map load")};
        loadMapCallback_ = Hook::RegisterLoadMapPreCallback(
            [this](auto&, auto&&...) { LeaveBeforeWorldChange(L"a map is loading"); }, loadOptions);
        // The same before a retry, reload or load leaves the field inside this world (no map load).
        if (!lss::player_two::field_exit::Install(
                *image,
                [](void* self) { static_cast<LocalSplitScreenMod*>(self)->LeaveBeforeWorldChange(L"the game leaves the field"); },
                this))
        {
            Log(L"a retry, reload or load is caught only by the engine tick's check of the field running byte");
        }

        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2, Ctrl+F8 removes player 2, Ctrl+F5 switches "
            L"top/bottom and side by side, Ctrl+F4 switches filled halves and 16:9 halves with bars. Player 2 starts on "
            L"their own camera and pad; for testing, Ctrl+F6 switches their half to player 1's camera and Ctrl+F9 "
            L"merges the pads");
    }

private:
    /// The two renderer changes, both or neither. The output rule goes first: with it alone, one
    /// player renders exactly as before (the game's single family is never marked additional), so
    /// there is never a moment when families are split but a later one still clears the screen.
    bool InstallRenderer(const lss::platform::GameImage& image) noexcept
    {
        if (!lss::renderer::InstallAdditionalFamilyOutput(image))
        {
            Log(L"could not install the final output's load-action rule");
            return false;
        }
        if (!lss::renderer::InstallViewFamilySubmission(image))
        {
            lss::renderer::RemoveAdditionalFamilyOutput();
            return false;
        }
        rendererInstalled_ = true;
        Log(L"renderer: one view family per player; later families keep the screen (stock output rule restored)");
        return true;
    }

    /// Player 2 leaves before the world changes under it (a map load, or the game leaving the field
    /// for a retry, reload or load); the session joins again once the new field is ready.
    void LeaveBeforeWorldChange(const wchar_t* why) noexcept
    {
        if (!session_->HasPlayerTwo())
        {
            return;
        }
        const bool left = session_->LeaveNow();
        FollowPlayerTwo();  // now, not at the next tick: the view module must not keep the old player 2
        if (left)
        {
            Log(L"{}: player 2 left (and joins again once the field is ready)", why);
        }
        else
        {
            Log(L"WARNING: {} and player 2 could not be removed; a new world would log player 2 in, and its camera "
                L"manager's spawns are expected to collide with player 1's",
                why);
        }
    }

    /// Runs on the game thread before each engine tick, outside Draw.
    void OnEngineTick() noexcept
    {
        lss::platform::frame_watch::OnFrameStart();
        const lss::platform::frame_watch::Charge charge(lss::platform::frame_watch::Part::EngineTick);
        if (!tickSeen_)
        {
            tickSeen_ = true;
            Log(L"engine tick callback is live");
        }
        switch (keys_.Poll())
        {
            case lss::players::PlayerIntent::Join: session_->Join(); break;
            case lss::players::PlayerIntent::Leave: session_->Leave(); break;  // logs its own outcome
            case lss::players::PlayerIntent::ToggleOwnCamera: ToggleOwnCamera(); break;
            case lss::players::PlayerIntent::ToggleLayout: LogLayoutChange(layout_.Toggle()); break;
            case lss::players::PlayerIntent::ToggleFill: ToggleFill(); break;
            case lss::players::PlayerIntent::TogglePadSticks: session_->TogglePadSticks(); break;
            case lss::players::PlayerIntent::None: break;
        }
        lss::renderer::PollNullViewProbe(session_->HasPlayerTwo());
        lss::player_two::field_exit::Watch();
        if (session_->HasPlayerTwo() && lss::player_two::field_exit::FieldLeft())
        {
            LeaveBeforeWorldChange(L"the field stopped running (a field exit the redirected entry did not see)");
        }
        lss::dev::TickBeforeSession();
        session_->Tick();
        lss::dev::TickAfterSession();
        FollowPlayerTwo();
        LogSettingsChange(settings_.Update(session_->HasPlayerTwo()));
        LogLayoutChange(layout_.Update(session_->HasPlayerTwo()));
        UpdateHalfShape();
        lss::hud::SetSplitActive(session_->HasPlayerTwo());
        lss::hud::UpdateHudPlacement(session_->HasPlayerTwo() && lss::hud::BandEnabled());

        const ULONGLONG now = GetTickCount64();
        if (!session_->HasPlayerTwo() || !lss::platform::frame_watch::Detailed())  // perf-log: the periodic status
        {
            nextStatus_ = now + kStatusIntervalMs;
        }
        else if (now >= nextStatus_)
        {
            nextStatus_ = now + kStatusIntervalMs;
            session_->LogStatus();
        }
    }

    /// Keeps the views module's idea of the halves' shape current while player 2 is in: the
    /// viewport size (it changes with the window) and the layout (it changes by key or script).
    /// Read twice a second; until the first reading the split FOV leaves the stock rule in place.
    void UpdateHalfShape() noexcept
    {
        if (!session_->HasPlayerTwo())
        {
            halfShapeTicks_ = 0;
            lss::views::SetHalfShape({});
            return;
        }
        if (halfShapeTicks_++ % kHalfShapeIntervalTicks != 0)
        {
            return;
        }
        const lss::platform::frame_watch::Job job(L"half shape read");
        auto*      playerOne  = static_cast<RC::Unreal::UObject*>(const_cast<void*>(session_->PlayerOne()));
        auto**     controller = lss::players::unreal::ControllerField(playerOne);
        const auto size   = controller != nullptr ? lss::players::unreal::ViewportSizeOf(*controller) : std::nullopt;
        const auto layout = lss::players::unreal::ReadTwoPlayerSplitscreenLayout();
        const auto half   = size && layout ? lss::views::HalfOf(size->width, size->height,
                                                                *layout == static_cast<std::uint8_t>(
                                                                               lss::players::SplitLayoutKind::SideBySide))
                                           : lss::views::HalfShape{};
        lss::views::SetHalfShape(half);
        if (half.aspect != loggedHalfAspect_)
        {
            loggedHalfAspect_ = half.aspect;
            if (half.aspect > 0.0f)
            {
                Log(L"split halves: viewport {}x{}, layout {}, half aspect {:.3f}, height {:.2f} of the screen",
                    size->width, size->height, *layout, half.aspect, half.heightRatio);
            }
            else
            {
                Log(L"split halves: the viewport size or layout cannot be read; the split FOV waits (stock rule meanwhile)");
            }
        }
    }

    void ToggleFill() noexcept
    {
        if (!viewPointInstalled_)
        {
            Log(L"filled halves unavailable (the view-point entry is not installed)");
            return;
        }
        if (lss::views::ToggleFill())
        {
            Log(L"split halves fill the screen (no bars)");
        }
        else
        {
            Log(L"split halves kept 16:9 with bars (the game's own shape)");
        }
    }

    static void LogLayoutChange(const lss::players::LayoutChange& change) noexcept
    {
        using Kind = lss::players::LayoutChange::Kind;
        const auto name = [](std::uint8_t value) {
            return value == static_cast<std::uint8_t>(lss::players::SplitLayoutKind::SideBySide) ? L"side by side"
                   : value == static_cast<std::uint8_t>(lss::players::SplitLayoutKind::TopBottom) ? L"top/bottom"
                                                                                                   : L"another value";
        };
        switch (change.kind)
        {
            case Kind::None: break;
            case Kind::Chosen: Log(L"split layout: {} from the next join", name(change.value)); break;
            case Kind::Unreadable:
                Log(L"split layout: TwoPlayerSplitscreenLayout cannot be read, so the switch did nothing");
                break;
            case Kind::Applied:
                Log(L"split layout: TwoPlayerSplitscreenLayout = {} ({})", change.value, name(change.value));
                break;
            case Kind::ApplyFailed:
                Log(L"split layout: could not set TwoPlayerSplitscreenLayout to {}; the game's layout stays", change.value);
                break;
            case Kind::Restored:
                Log(L"player 2 is out: TwoPlayerSplitscreenLayout restored to {} ({})", change.value, name(change.value));
                break;
            case Kind::LeftAlone:
                Log(L"player 2 is out: TwoPlayerSplitscreenLayout was changed to {} meanwhile; left as it is", change.value);
                break;
            case Kind::RestoreFailed:
                Log(L"player 2 is out: could not restore TwoPlayerSplitscreenLayout to {}", change.value);
                break;
        }
    }

    /// Keeps the view-point module's idea of the players current, and describes the local players
    /// when player 2 appears (a subclass with its own vtable would bypass the camera choice).
    void FollowPlayerTwo() noexcept
    {
        const void*         playerTwo = session_->PlayerTwo();
        const std::uint32_t joins     = session_->JoinCount();
        if (playerTwo == followedPlayerTwo_ && joins == followedJoins_)
        {
            return;
        }
        followedPlayerTwo_ = playerTwo;
        followedJoins_     = joins;
        // The FOV rule's offset, confirmed by reflection before the first split frame uses it.
        if (playerTwo != nullptr)
        {
            const auto axisOffset = lss::players::unreal::AspectRatioAxisConstraintOffset();
            lss::views::SetAxisConstraintOffset(axisOffset.value_or(0));
            if (!axisOffset)
            {
                Log(L"ULocalPlayer::AspectRatioAxisConstraint not found where the engine reads it; split views keep "
                    L"the game's FOV rule");
            }
        }
        // Through null first: a new player 2 at the old one's address must still start fresh
        // (on player 1's camera, first view point described).
        lss::views::SetPlayers(nullptr, nullptr);
        lss::views::SetPlayers(session_->PlayerOne(), playerTwo);
        if (playerTwo == nullptr || !viewPointInstalled_)
        {
            return;
        }
        auto* players = lss::players::unreal::LocalPlayers(lss::players::unreal::GameInstance());
        for (std::int32_t i = 0; players != nullptr && i < players->Num(); ++i)
        {
            auto* player = (*players)[i];
            Log(L"local player {}: class {}, view point {}", i + 1, player->GetClassPrivate()->GetName(),
                lss::views::UsesEngineViewPoint(player) ? L"through the engine's ULocalPlayer entry"
                                                         : L"through ANOTHER vtable (the camera choice cannot reach it)");
        }
    }

    void ToggleOwnCamera() noexcept
    {
        if (!viewPointInstalled_)
        {
            Log(L"camera choice unavailable (the view-point entry is not installed)");
            return;
        }
        if (!session_->HasPlayerTwo())
        {
            Log(L"camera choice: no player 2 (Ctrl+F7 adds one)");
            return;
        }
        if (lss::views::ToggleOwnCamera())
        {
            Log(L"player 2's half shows player 2's own camera");
        }
        else
        {
            Log(L"player 2's half shows player 1's camera");
        }
    }

    static void LogSettingsChange(const lss::players::SettingsChange& change) noexcept
    {
        using Kind = lss::players::SettingsChange::Kind;
        constexpr auto name = lss::players::SplitScreenSettings::kResizeMethod;
        switch (change.kind)
        {
            case Kind::None: break;
            case Kind::Applied: Log(L"split screen: {} {} -> 2 while player 2 is in", name, change.value); break;
            case Kind::AlreadySet: Log(L"split screen: {} is already 2; left as it is", name); break;
            case Kind::ApplyFailed:
                Log(L"split screen: could not set {} to 2; rendering stays correct but slow", name);
                break;
            case Kind::Restored: Log(L"player 2 is out: {} restored to {}", name, change.value); break;
            case Kind::LeftAlone:
                Log(L"player 2 is out: {} was changed to {} meanwhile; left as it is", name, change.value);
                break;
            case Kind::RestoreFailed: Log(L"player 2 is out: could not restore {} to {}", name, change.value); break;
        }
    }

    std::optional<lss::player_two::Session>   session_;
    lss::players::JoinKeys                    keys_;
    ULONGLONG                                 nextStatus_ = 0;
    lss::players::SplitScreenSettings         settings_{
        {&lss::players::unreal::ConsoleVariableInt, &lss::players::unreal::ExecuteConsoleCommand}};
    lss::players::SplitLayout                 layout_{{&lss::players::unreal::ReadTwoPlayerSplitscreenLayout,
                                                       &lss::players::unreal::WriteTwoPlayerSplitscreenLayout}};
    std::uint32_t                             halfShapeTicks_     = 0;
    float                                     loggedHalfAspect_   = -1.0f;
    bool                                      tickSeen_           = false;
    bool                                      rendererInstalled_  = false;
    bool                                      viewPointInstalled_ = false;
    const void*                               followedPlayerTwo_  = nullptr;
    std::uint32_t                             followedJoins_      = 0;
    Hook::GlobalCallbackId                    tickCallback_       = Hook::ERROR_ID;
    Hook::GlobalCallbackId                    loadMapCallback_    = Hook::ERROR_ID;
};

}  // namespace

#define LOCALSPLITSCREEN_API __declspec(dllexport)
extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
