#pragma once

// DLSS Frame Generation with several views (research/RE-p2-framegen-4.md). The game drives NVIDIA Streamline's
// DLSS-G per view family: each view is its own Streamline viewport with its own camera data, motion vectors, HUD-less
// colour, UI and back-buffer part. Two inputs were only right for one view:
//   - the scene depth is one texture every family draws into, tagged "valid until present": at present every
//     viewport would read the last family's depth. While views are split, the depth tag goes to Streamline as "only
//     valid now", Streamline's own option, so it copies each viewport's depth at tag time on the game's command list;
//   - the reset on a camera cut (the generated-frame constants' and the velocity history's) is gated on the view's
//     family having `+0x70 == 0`, which the mod's later families never have. The gates are opened, so each view resets
//     on its own camera cut.
// Single player is unchanged: the depth rule acts only while views are split, and one family has `+0x70 == 0` anyway.
// The tonemap's frame-generation targets are renderer/additional_family_output's.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::renderer::frame_generation {

/// The reset gates (both or neither), and the depth rule's tag wrapper if Streamline is loaded. Logs each outcome.
void Install(const platform::GameImage& image) noexcept;

/// Game thread, each engine tick: places the tag wrapper once the game has loaded Streamline (nothing after that).
void Poll() noexcept;

/// The game's own pointer and instructions back.
void Remove() noexcept;

/// Which tags go to Streamline as "only valid now" while views are split: none (the game's own lifecycles), the depth
/// (the rule), or every frame input (depth, motion vectors, HUD-less colour, UI; for comparison).
enum class TagsNow : std::uint8_t
{
    Off,
    Depth,
    All,
};
void    SetTagsNow(TagsNow tags) noexcept;
TagsNow GetTagsNow() noexcept;

/// The tag wrapper is in place.
bool TagWrapperPlaced() noexcept;

/// One slSetTag call as forwarded: the tags as sent (the game's, or the wrapper's copy), each tag's lifecycle as the
/// game asked, the call's result. For the development watch (renderer/framegen_watch).
struct TagCall
{
    const void*          viewport = nullptr;
    const std::uint8_t*  sent     = nullptr;  // count tags of facts::kSlTagSize bytes
    const std::uint32_t* lifeIn   = nullptr;  // count entries
    std::uint32_t        count    = 0;
    const void*          command  = nullptr;
    std::int32_t         result   = 0;
};
using TagObserver = void (*)(const TagCall& call);

/// Any thread: the observer called after each forwarded tag call (on the render and RHI threads), or null.
void SetTagObserver(TagObserver observer) noexcept;

}  // namespace lss::renderer::frame_generation
