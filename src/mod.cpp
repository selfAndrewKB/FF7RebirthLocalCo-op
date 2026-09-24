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

#include "platform/engine_addresses.h"
#include "platform/image.h"
#include "platform/log.h"
#include "players/join_keys.h"
#include "players/local_players.h"
#include "players/split_screen_settings.h"
#include "players/unreal_access.h"
#include "renderer/additional_family_output.h"
#include "renderer/view_family_submission.h"
#include "views/player_view_point.h"

#include <cstdint>
#include <optional>

namespace {

namespace Hook = RC::Unreal::Hook;
using lss::platform::Log;

constexpr const wchar_t* kVersion = L"0.2.0-slice2";

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

    // The GetViewPoint entry is not checked here: it serves only the optional test turn, and its
    // install is itself a compare-and-swap against the expected function (views/player_view_point).

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
        ModVersion     = STR("0.2.0-slice2");
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

        // With player 2 still in, the engine keeps rendering two views. Putting the engine's own
        // submission back would render them in one family, the configuration this mod exists to
        // avoid. Removing player 2 here is not an option: the destructor may run off the game
        // thread. So the submission stays installed; the DLL is pinned, so it stays valid.
        if (players_ && players_->HasPlayerTwo())
        {
            Log(L"unloading with player 2 still in: the per-player rendering stays installed");
            return;
        }
        if (viewPointInstalled_)
        {
            lss::views::RemovePlayerViewPoint();
        }
        if (rendererInstalled_)
        {
            lss::renderer::RemoveViewFamilySubmission();
            lss::renderer::RemoveAdditionalFamilyOutput();
        }
    }

    auto on_unreal_init() -> void override
    {
        Log(L"v{} starting", kVersion);

        const auto image = lss::platform::GameImage::Locate();
        const auto removeLocalPlayer = image ? VerifyEngine(*image) : std::nullopt;
        if (!removeLocalPlayer || !InstallRenderer(*image))
        {
            Log(L"standing aside: nothing installed, the game runs unmodified");
            return;
        }
        players_.emplace(*removeLocalPlayer);

        // The view-point entry only serves the crash-test turn (SPLITSCREEN-ARCHITECTURE.md §9), so
        // the split screen runs without it; the turn key then reports that it is unavailable.
        viewPointInstalled_ = lss::views::InstallPlayerViewPoint(*image);
        if (!viewPointInstalled_)
        {
            Log(L"ULocalPlayer::GetViewPoint's entry does not hold the engine's function; the test turn is unavailable");
        }

        const Hook::FCallbackOptions tickOptions{false, false, STR("LocalSplitScreen"), STR("player intent")};
        tickCallback_ = Hook::RegisterEngineTickPreCallback(
            [this](Hook::TCallbackIterationData<void>&, RC::Unreal::UEngine*, float, bool) { OnEngineTick(); },
            tickOptions);

        // Player 2 leaves before any map load, so the engine never travels with a local player
        // it cannot give a controller of its own (it would run the faulting login path for it).
        const Hook::FCallbackOptions loadOptions{false, false, STR("LocalSplitScreen"), STR("leave before map load")};
        loadMapCallback_ = Hook::RegisterLoadMapPreCallback(
            [this](auto&, auto&&...) {
                if (!players_->HasPlayerTwo())
                {
                    return;
                }
                const bool left = players_->Leave();
                FollowPlayerTwo();  // now, not at the next tick: the view module must not keep the old player 2
                if (left)
                {
                    Log(L"a map is loading: player 2 left (press Ctrl+F7 to rejoin afterwards)");
                }
                else
                {
                    Log(L"WARNING: a map is loading and player 2 could not be removed; the engine will "
                        L"try to log player 2 in, which is known to fault in this game");
                }
            },
            loadOptions);

        Log(L"ready: every engine check passed. Ctrl+F7 adds player 2, Ctrl+F8 removes player 2, Ctrl+F6 turns player 2's view (test)");
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

    /// Runs on the game thread before each engine tick, outside Draw.
    void OnEngineTick() noexcept
    {
        if (!tickSeen_)
        {
            tickSeen_ = true;
            Log(L"engine tick callback is live");
        }
        switch (keys_.Poll())
        {
            case lss::players::PlayerIntent::Join:  players_->Join(); break;
            case lss::players::PlayerIntent::Leave: players_->Leave(); break;  // logs its own outcome
            case lss::players::PlayerIntent::ToggleTestTurn: ToggleTestTurn(); break;
            case lss::players::PlayerIntent::None:  break;
        }
        players_->Tick();
        FollowPlayerTwo();
        LogSettingsChange(settings_.Update(players_->HasPlayerTwo()));
    }

    /// Keeps the view-point module's idea of player 2 current, and describes the local players
    /// when player 2 appears (a subclass with its own vtable would bypass the test turn).
    void FollowPlayerTwo() noexcept
    {
        const void*         playerTwo = players_->PlayerTwo();
        const std::uint32_t joins     = players_->JoinCount();
        if (playerTwo == followedPlayerTwo_ && joins == followedJoins_)
        {
            return;
        }
        followedPlayerTwo_ = playerTwo;
        followedJoins_     = joins;
        // Through null first: a new player 2 at the old one's address must still start fresh
        // (turn off, first view point described).
        lss::views::SetPlayerTwo(nullptr);
        lss::views::SetPlayerTwo(playerTwo);
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
                                                         : L"through ANOTHER vtable (the test turn cannot reach it)");
        }
    }

    void ToggleTestTurn() noexcept
    {
        if (!viewPointInstalled_)
        {
            Log(L"test turn unavailable (the view-point entry is not installed)");
            return;
        }
        if (!players_->HasPlayerTwo())
        {
            Log(L"test turn: no player 2 (Ctrl+F7 adds one)");
            return;
        }
        if (lss::views::ToggleTestTurn())
        {
            Log(L"TEST: player 2's view turned {:.0f} degrees away from player 1's", lss::views::kTestTurnDegrees);
        }
        else
        {
            Log(L"TEST: player 2's view back on player 1's camera (turn off)");
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

    std::optional<lss::players::LocalPlayers> players_;
    lss::players::JoinKeys                    keys_;
    lss::players::SplitScreenSettings         settings_{
        {&lss::players::unreal::ConsoleVariableInt, &lss::players::unreal::ExecuteConsoleCommand}};
    bool                                      tickSeen_        = false;
    bool                                      rendererInstalled_ = false;
    bool                                      viewPointInstalled_ = false;
    const void*                               followedPlayerTwo_  = nullptr;
    std::uint32_t                             followedJoins_      = 0;
    Hook::GlobalCallbackId                    tickCallback_    = Hook::ERROR_ID;
    Hook::GlobalCallbackId                    loadMapCallback_ = Hook::ERROR_ID;
};

}  // namespace

#define LOCALSPLITSCREEN_API __declspec(dllexport)
extern "C"
{
    LOCALSPLITSCREEN_API RC::CppUserModBase* start_mod() { return new LocalSplitScreenMod(); }
    LOCALSPLITSCREEN_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
