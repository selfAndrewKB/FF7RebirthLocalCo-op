#include "control/mod_commands.h"

#include "platform/command_registry.h"
#include "platform/log.h"
#include "player_two/battle/lock_on.h"
#include "player_two/camera/camera_distance.h"
#include "player_two/camera/shoulder_scale.h"
#include "player_two/input/pad_buttons.h"
#include "player_two/input/pad_sticks.h"
#include "player_two/scopes/scope_leaks.h"
#include "player_two/world/charapop_centre.h"
#include "player_two/world/charapop_near.h"

#include <format>
#include <string>

namespace lss::control {

namespace {

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
    RegisterPlayerTwo();
}

}  // namespace lss::control
