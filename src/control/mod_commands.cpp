#include "control/mod_commands.h"

#include "hud/hud_trace.h"
#include "hud/layout/overlay_layout.h"
#include "hud/minimap/area_icons_two.h"
#include "hud/minimap/minimap_once.h"
#include "hud/passes/player_two_pass.h"
#include "hud/passes/root_overlay_render.h"
#include "hud/passes/second_image.h"
#include "hud/position_probe.h"
#include "hud/widget_dump.h"
#include "platform/command_registry.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/battle/lock_on.h"
#include "player_two/camera/camera_distance.h"
#include "player_two/camera/shoulder_scale.h"
#include "player_two/input/pad_buttons.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/scopes/scope_leaks.h"
#include "player_two/world/charapop_centre.h"
#include "player_two/world/charapop_near.h"
#include "renderer/me_feedback_once.h"
#include "renderer/submission_census.h"
#include "renderer/submit_hints.h"
#include "renderer/view_family_submission.h"
#include "views/player_view_point.h"
#include "views/stream_views.h"

#include <format>
#include <string>

namespace lss::control {

namespace {

constexpr std::int64_t kMaxStreamFov = 170;  // views::kMaxStreamFov

Outcome Failed(std::wstring why)
{
    return {false, std::move(why)};
}

bool On(const Value& value)
{
    return std::get<bool>(value);
}

std::int64_t Number(const Value& value)
{
    return std::get<std::int64_t>(value);
}

const std::wstring& Word(const Value& value)
{
    return std::get<std::wstring>(value);
}

/// A diagnosis mask: `<name> <0..max>`, set by `set` and acknowledged as the old command did.
void RegisterMask(const wchar_t* name, std::int64_t max, const wchar_t* help, const wchar_t* done,
                  void (*set)(std::uint32_t) noexcept)
{
    Register({.name  = name,
              .shape = Shape::Integer,
              .max   = max,
              .help  = help,
              .set   = [set, done](const Value& v) {
                  set(static_cast<std::uint32_t>(Number(v)));
                  return Outcome{true, done};
              }});
}

// ---- the views --------------------------------------------------------------------------------------------------

void RegisterViews()
{
    Register({.name     = L"split-fov",
              .shape    = Shape::Float,
              .minFloat = 0.0,
              .maxFloat = 1.0,
              .words    = {L"native"},
              .help     = L"filled halves' FOV: 0 keeps the single-player framing, 1 its on-screen scale (0.5 at "
                          L"start; views/view_shape.h); native = the stock MajorAxisFOV rule",
              .set      = [](const Value& v) {
                  if (!views::ViewPointInstalled())
                  {
                      return Failed(L"the view-point entry is not installed");
                  }
                  const bool  native = std::holds_alternative<std::wstring>(v);
                  const float blend  = native ? -1.0f : static_cast<float>(std::get<double>(v));
                  views::SetSplitFovWeight(blend);
                  // The resulting FOV is logged by the views module when a filled split view first uses it.
                  return Outcome{true, native ? L"stock MajorAxisFOV rule" : L"weight set; applies to filled halves"};
              }});
    Register({.name  = L"stream-views",
              .shape = Shape::Word,
              .words = {L"off", L"p2", L"both"},
              .help  = L"which split players' cameras are registered with the game's own scenery streaming (both at "
                       L"start)",
              .set   = [](const Value& v) {
                  const std::wstring& word = Word(v);
                  if (!views::StreamViewsInstalled())
                  {
                      return Failed(L"streaming views are not installed (see the install log)");
                  }
                  const auto mode = word == L"off" ? views::StreamViewsMode::Off
                                    : word == L"p2" ? views::StreamViewsMode::PlayerTwo
                                                    : views::StreamViewsMode::Both;
                  views::SetStreamViewsMode(mode);  // applied at the next tick, which logs it
                  return Outcome{true, word == L"off" ? L"no streaming views (the game's own only)"
                                       : word == L"p2" ? L"player 2's camera"
                                                       : L"both players' cameras"};
              }});
    Register({.name  = L"stream-fov-p1",
              .shape = Shape::Integer,
              .min   = 1,
              .max   = kMaxStreamFov,
              .words = {L"auto"},
              .help  = L"the horizontal FOV in degrees player 1's camera is registered with under both, or auto for "
                       L"what its half covers (at start)",
              .set   = [](const Value& v) {
                  if (!views::StreamViewsInstalled())
                  {
                      return Failed(L"streaming views are not installed (see the install log)");
                  }
                  const std::int64_t degrees = std::holds_alternative<std::wstring>(v) ? 0 : Number(v);
                  views::SetPlayerOneStreamFov(static_cast<float>(degrees));  // applied at the next tick, logged
                  return Outcome{true, degrees == 0 ? std::wstring(L"player 1's camera as its half covers")
                                                    : std::format(L"player 1's camera {} degrees wide", degrees)};
              }});
}

// ---- the HUD -------------------------------------------------------------------------------------------------------

void RegisterHud()
{
    Register({.name = L"hud-band", .shape = Shape::OnOff,
              .help = L"lay the HUD out in the part of its image each half shows; off logs the values only",
              .set  = [](const Value& v) {
                  if (!hud::OverlayLayoutInstalled())
                  {
                      return Failed(L"the HUD layout is not installed (see the startup log)");
                  }
                  hud::SetBandEnabled(On(v));
                  // What it sets is logged by the HUD module at the overlay's next render while split.
                  return Outcome{true, On(v) ? L"HUD laid out in each half's visible band (the default)"
                                             : L"HUD full-image layout until `hud-band on` (values logged only)"};
              }});
    Register({.name = L"hud-image", .shape = Shape::OnOff,
              .help = L"player 2's HUD image: the overlay rendered a second time and drawn only in player 2's half",
              .set  = [](const Value& v) {
                  if (!hud::SecondImageInstalled() || !hud::RootOverlayRenderInstalled())
                  {
                      return Failed(L"player 2's HUD image is not installed (see the startup log)");
                  }
                  hud::SetSecondImage(On(v));
                  if (On(v) && !hud::SecondImageOn())
                  {
                      return Failed(L"player 2's HUD image refused a check (see the log)");
                  }
                  return Outcome{true, On(v) ? L"player 2's HUD image on" : L"player 2's HUD image off"};
              }});
    Register({.name = L"hud-levers", .shape = Shape::OnOff,
              .help = L"player 2's image shows player 2's content: the game's player levers switched around player "
                      L"2's pass",
              .set  = [](const Value& v) {
                  if (!hud::PlayerTwoPassInstalled())
                  {
                      return Failed(L"player 2's HUD levers are not installed (see the startup log)");
                  }
                  hud::SetPlayerTwoContent(On(v));
                  if (On(v) && !hud::PlayerTwoContentOn())
                  {
                      return Failed(L"player 2's HUD levers refused a check (see the log)");
                  }
                  return Outcome{true, On(v) ? (hud::SecondImageOn() ? L"player 2's content on"
                                                                     : L"player 2's content on (needs hud-image on)")
                                             : L"player 2's content off"};
              }});
    RegisterMask(L"hud-levers-mask", 511,
                 L"diagnosis: which levers act (1 contexts, 2 first player, 4 primary controller, 8 view rect, 16 "
                 L"updates, 32 compass, 64 the arrows' own timers, 128 the enemy gauges per view, 256 the battle "
                 L"popups per view)",
                 L"levers mask set (see the log)", hud::SetPlayerTwoLeversMask);
    RegisterMask(L"icons-mask", 15,
                 L"diagnosis: which parts of player 2's own map icons act (1 its icon manager ticked, 2 its first "
                 L"controller player 2's, 4 each half shows its own canvases, 8 ranged from player 2's member)",
                 L"icons mask set (see the log)", hud::area_icons_two::SetMask);
    Register({.name  = L"hud-trace",
              .shape = Shape::Integer,
              .min   = 1,
              .max   = hud::kMaxHudTraceFrames,  // the trace's own limit
              .help  = L"log the order of the HUD's tick, marker placements, icon projections and render for a number "
                       L"of frames, and which player each placement and projection used",
              .set   = [](const Value& v) {
                  if (!hud::RootOverlayRenderInstalled())
                  {
                      return Failed(L"the overlay render hook is not installed (see the startup log)");
                  }
                  // Installed by the first trace, not at every dev start (seven call redirects on the HUD's hot path).
                  if (!hud::HudTraceInstalled())
                  {
                      const auto image = platform::GameImage::Locate();
                      if (!image || !hud::InstallHudTrace(*image))
                      {
                          return Failed(L"the HUD trace could not be installed (see the log)");
                      }
                  }
                  hud::StartHudTrace(static_cast<std::int32_t>(Number(v)));
                  return Outcome{true, L"tracing (one log line per frame)"};
              }});
    Register({.name = L"hud-probe", .shape = Shape::OnOff, .help = L"diagnosis: the HUD position probe",
              .set  = [](const Value& v) {
                  // Installed by the first switch-on, not at every dev start (its redirect sits on a Blueprint call
                  // path).
                  if (On(v) && !hud::PositionProbeInstalled())
                  {
                      const auto image = platform::GameImage::Locate();
                      if (!image || !hud::InstallPositionProbe(*image))
                      {
                          return Failed(L"the HUD position probe could not be installed (see the log)");
                      }
                  }
                  if (!hud::PositionProbeInstalled())
                  {
                      return Outcome{true, L"the HUD probe was never installed (it is off)"};
                  }
                  hud::SetPositionProbe(On(v));
                  return Outcome{};
              }});
    Register({.name = L"minimap-once", .shape = Shape::OnOff,
              .help = L"the minimap's map is rendered once per frame, by player 1's HUD pass (on, at start), or again "
                      L"in player 2's",
              .set  = [](const Value& v) {
                  if (!hud::MinimapOnceInstalled())
                  {
                      return Failed(L"the minimap's single render is not installed (see the install log)");
                  }
                  hud::SetMinimapOnce(On(v));
                  return Outcome{true, On(v) ? L"the minimap's map rendered once per frame, by player 1's pass"
                                             : L"the minimap's map rendered in both players' passes"};
              }});
    Register({.name  = L"dump",
              .shape = Shape::Word,
              .words = {L"widgets", L"hud-players"},
              .help  = L"log each visible HUD widget's desired size and geometry (widgets), or which player the HUD "
                       L"roots and its global player reads name (hud-players)",
              .set   = [](const Value& v) {
                  const bool widgets = Word(v) == L"widgets";
                  int        listed  = -1;
                  if (widgets)
                  {
                      listed = hud::DumpWidgets();
                  }
                  else if (const auto image = platform::GameImage::Locate())
                  {
                      listed = hud::DumpHudPlayers(*image);
                  }
                  return listed >= 0 ? Outcome{true, std::to_wstring(listed) + (widgets ? L" widgets" : L" lines") +
                                                         L" listed in the log"}
                                     : Failed(L"nothing dumped (see the log)");
              }});
    Register({.name = L"dump-slots", .shape = Shape::Text, .help = L"log a widget class's viewport layout and slots",
              .set  = [](const Value& v) {
                  const int listed = hud::DumpSlots(Word(v));
                  return listed >= 0 ? Outcome{true, std::to_wstring(listed) + L" slots listed in the log"}
                                     : Failed(L"nothing dumped (see the log)");
              }});
}

// ---- the renderer -------------------------------------------------------------------------------------------

void RegisterRenderer()
{
    Register({.name  = L"stream-feedback",
              .shape = Shape::Word,
              .words = {L"p1", L"each"},
              .help  = L"which view families hand the game's scenery streaming their detail requests: player 1's only, "
                       L"as with one player (p1, at start), or each family",
              .set   = [](const Value& v) {
                  const bool playerOneOnly = Word(v) == L"p1";
                  if (!renderer::MeFeedbackOnceInstalled())
                  {
                      return Failed(L"the one-copy streaming feedback is not installed (see the install log)");
                  }
                  renderer::SetMeFeedbackOnce(playerOneOnly);
                  return Outcome{true, playerOneOnly ? L"player 1's family alone hands in streaming requests"
                                                     : L"each family hands in streaming requests (the game's code as "
                                                       L"it runs per family)"};
              }});
    Register({.name  = L"submit-hints",
              .shape = Shape::Word,
              .words = {L"skip", L"stock"},
              .help  = L"parallel passes without their own GPU submission (skip), or the game's; skip while player 2 "
                       L"is in, set again at the next join or leave",
              .set   = [](const Value& v) {
                  const bool skip = Word(v) == L"skip";
                  if (!renderer::SubmitHintSkipInstalled())
                  {
                      return Failed(L"the submit-hint skip is not installed (see the install log)");
                  }
                  renderer::SetSubmitHintSkip(skip);
                  return Outcome{true, skip ? L"parallel passes skip their submit hint" : L"the game's submit hints"};
              }});
    Register({.name = L"census", .shape = Shape::OnOff,
              .help = L"diagnosis: the GPU submission census in the perf log's window (off at start; census.txt: on)",
              .set  = [](const Value& v) {
                  if (!On(v))
                  {
                      renderer::RemoveSubmissionCensus();
                      return Outcome{true, L"submission census off"};
                  }
                  if (!renderer::InstallSubmissionCensus())
                  {
                      return Failed(L"the submission census could not install (see the log)");
                  }
                  return Outcome{true, L"submission census on (summary in the perf log)"};
              }});
    Register({.name  = L"feedback-mask",
              .shape = Shape::Integer,
              .max   = 31,
              .help  = L"diagnosis: which of player 2's skips under stream-feedback p1 act (1 the readback processing, "
                       L"2 the main pass's copy, 4 the hidden-component pass, 8 the phase 8 mip-request dispatches, 16 "
                       L"the shadow pass's streaming writes)",
              .set   = [](const Value& v) {
                  if (!renderer::MeFeedbackOnceInstalled())
                  {
                      return Failed(L"the one-copy streaming feedback is not installed (see the install log)");
                  }
                  renderer::SetMeFeedbackMask(static_cast<std::uint32_t>(Number(v)));
                  return Outcome{true, L"feedback mask set (see the log)"};
              }});
    Register({.name = L"family-split", .shape = Shape::OnOff,
              .help = L"diagnosis: one view family per player (on, at start), or both views in the engine's one family "
                      L"(off; breaks the multi-view rules, so only with own-camera off)",
              .set  = [](const Value& v) {
                  renderer::SetFamilySplit(On(v));
                  return Outcome{true, On(v) ? L"one view family per player" : L"both views in one family (diagnosis)"};
              }});
}

// ---- player 2's modules --------------------------------------------------------------------------------------------

void RegisterPlayerTwo()
{
    RegisterMask(L"sticks-mask", 15,
                 L"diagnosis: which parts of per-pad sticks act (1 player 2's input as the primary player's, 2 player "
                 L"1's move state on pad 1, 4 player 2's member ticked, 8 player 1's input update and leader-action "
                 L"tick on pad 1); all at start",
                 L"sticks mask set (see the log)", player_two::pad_sticks::SetMask);
    RegisterMask(L"leak-fixes", 7,
                 L"diagnosis: which put-backs of player 2's scope leaks act (1 the input clock, 2 the leader transform, "
                 L"4 field summons skipped); all at start",
                 L"leak fixes set (see the log)", player_two::scope_leaks::SetMask);
    RegisterMask(L"buttons-mask", 3,
                 L"diagnosis: which parts of per-pad buttons act (1 the split views, 2 pad 2 kept from menus and "
                 L"prompts); both at start",
                 L"buttons mask set (see the log)", player_two::pad_buttons::SetMask);
    Register({.name  = L"battle-mask",
              .shape = Shape::Integer,
              .max   = 511,
              .help  = L"diagnosis: bit 8 runs player 2's targeting (the game's targeting tick for player 2); the other "
                       L"bits are retired and ignored; on at start",
              .set   = [](const Value& v) {
                  // Bit 8 is player 2's targeting run (player_two/battle/lock_on.h). The other bits (1, 2, 4, 16: the
                  // direct actions; 32, 64, 128, 256: the mod's own lock, switching, lock camera and cursor) are
                  // retired: the game's own handler and targeting tick do those for player 2.
                  player_two::lock_on::SetEnabled((Number(v) & 8) != 0);
                  return Outcome{true, L"battle mask set (see the log)"};
              }});
    Register({.name = L"shoulder-scale", .shape = Shape::OnOff,
              .help = L"each half's camera shoulder offset scaled to its width (on at start)",
              .set  = [](const Value& v) {
                  if (!player_two::shoulder_scale::Installed())
                  {
                      return Failed(L"the shoulder scale is not installed (see the install log)");
                  }
                  player_two::shoulder_scale::SetOn(On(v));
                  return Outcome{true, On(v) ? L"each half's camera shoulder offset scaled to its width"
                                             : L"the game's shoulder offset as it is"};
              }});
    Register({.name     = L"shoulder-scale-min",
              .shape    = Shape::Float,
              .minFloat = 0.25,
              .maxFloat = 1.0,
              .help     = L"the smallest shoulder factor (0.7 at start; side by side's width alone gives 0.5)",
              .set      = [](const Value& v) {
                  if (!player_two::shoulder_scale::Installed())
                  {
                      return Failed(L"the shoulder scale is not installed (see the install log)");
                  }
                  player_two::shoulder_scale::SetMinimum(static_cast<float>(std::get<double>(v)));
                  return Outcome{true, L"the narrowest half's shoulder offset is softened no further than this"};
              }});
    // The two layouts' framing offsets, one entry each (one argument per command): each sets its own and keeps the
    // other layout's as it is.
    for (const int layout : {0, 1})
    {
        Register({.name  = layout == 0 ? L"framing-offset.top-bottom" : L"framing-offset.side-by-side",
                  .shape = Shape::Integer,
                  .min   = -2,
                  .max   = 5,
                  .help  = layout == 0 ? L"field-camera distance steps added to the Options level while player 2 is in "
                                         L"and the screen splits top and bottom (0 at start)"
                                       : L"field-camera distance steps added to the Options level while player 2 is in "
                                         L"and the screen splits side by side (0 at start)",
                  .set   = [layout](const Value& v) {
                      if (!player_two::camera_distance::FramingAvailable())
                      {
                          return Failed(L"split framing is not available (see the install log)");
                      }
                      const auto steps = static_cast<std::int32_t>(Number(v));
                      player_two::camera_distance::SetLayoutOffsets(
                          layout == 0 ? steps : player_two::camera_distance::LayoutOffset(0),
                          layout == 1 ? steps : player_two::camera_distance::LayoutOffset(1));
                      return Outcome{true, L"each player's field camera follows the Options distance plus the "
                                           L"layout's steps while player 2 is in"};
                  },
                  .get = [layout] { return Value{std::int64_t{player_two::camera_distance::LayoutOffset(layout)}}; }});
    }
    Register({.name = L"npc-centre", .shape = Shape::OnOff,
              .help = L"the NPC population follows both players while split (on at start), or the leader as the game "
                      L"alone does",
              .set  = [](const Value& v) {
                  if (!player_two::charapop_centre::Installed())
                  {
                      return Failed(L"the NPC centre is not installed (see the startup log)");
                  }
                  player_two::charapop_centre::SetEnabled(On(v));  // logs the new state
                  return Outcome{true, On(v) ? L"the NPC population follows both players while split (the default)"
                                             : L"the NPC population follows the leader, as the game alone does"};
              }});
    Register({.name  = L"npc-detail",
              .shape = Shape::Word,
              .words = {L"full", L"reduced", L"off", L"on"},
              .help  = L"characters near player 2 that player 1's camera does not keep get full detail within the "
                       L"game's distances from player 2's camera (full, at start; on means full), within half of them "
                       L"(reduced), or only what player 1's camera gives them (off)",
              .set   = [](const Value& v) {
                  using player_two::charapop_near::Detail;
                  if (!player_two::charapop_near::Installed())
                  {
                      return Failed(L"the NPC detail lever is not installed (see the startup log)");
                  }
                  const std::wstring& word   = Word(v);
                  const Detail        detail = word == L"reduced" ? Detail::Reduced
                                               : word == L"off"   ? Detail::Off
                                                                  : Detail::Full;
                  player_two::charapop_near::SetDetail(detail);  // logs the new state
                  return Outcome{true, detail == Detail::Full
                                           ? L"characters near player 2 keep full detail where player 1's camera does "
                                             L"not (the default)"
                                       : detail == Detail::Reduced
                                           ? L"characters near player 2 keep full detail within half the game's "
                                             L"distances"
                                           : L"characters near player 2 get only what player 1's camera gives them"};
              }});
}

}  // namespace

void RegisterModCommands()
{
    RegisterViews();
    RegisterHud();
    RegisterRenderer();
    RegisterPlayerTwo();
}

}  // namespace lss::control
