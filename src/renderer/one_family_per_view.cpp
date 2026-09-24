#include "renderer/one_family_per_view.h"

#include "renderer/scene_view_family.h"

#include <cstdint>
#include <utility>

namespace lss::renderer {

namespace {

/// Typed access to one field of the family, by the offsets in scene_view_family.h.
template <class T>
T& Field(std::byte* viewFamily, std::size_t offset) noexcept
{
    return *reinterpret_cast<T*>(viewFamily + offset);
}

}  // namespace

void SubmitOneFamilyPerView(void* rendererModule, void* canvas, std::byte* viewFamily,
                            SubmitViewFamilyFn engineSubmit) noexcept
{
    auto& views      = Field<const void**>(viewFamily, family::kViewsData);
    auto& viewCount  = Field<std::int32_t>(viewFamily, family::kViewsCount);
    auto& skipFrame  = Field<std::int32_t>(viewFamily, family::kSkipFrameNumberIncrement);
    auto& additional = Field<std::uint8_t>(viewFamily, family::kAdditionalViewFamily);

    const std::int32_t totalViews = viewCount;
    if (totalViews <= 1 || views == nullptr)
    {
        engineSubmit(rendererModule, canvas, viewFamily);
        return;
    }

    const std::int32_t savedSkipFrame  = skipFrame;
    const std::uint8_t savedAdditional = additional;

    for (std::int32_t k = 0; k < totalViews; ++k)
    {
        std::swap(views[0], views[k]);  // view k into slot 0; a no-op for k == 0
        viewCount = 1;
        if (k > 0)
        {
            skipFrame  = 1;
            additional = 1;
        }

        engineSubmit(rendererModule, canvas, viewFamily);

        // Put this view back before the next one is brought forward, so that at every step the
        // array holds the engine's views in the engine's order apart from one swap.
        std::swap(views[0], views[k]);
        viewCount  = totalViews;
        skipFrame  = savedSkipFrame;
        additional = savedAdditional;
    }
}

}  // namespace lss::renderer
