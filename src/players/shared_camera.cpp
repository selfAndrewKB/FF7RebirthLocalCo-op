#include "players/shared_camera.h"

#include "platform/log.h"

namespace lss::players {

bool SharedCamera::Attach(unreal::UObject* playerOne, unreal::UObject* playerTwo) noexcept
{
    unreal::UObject** oneField = unreal::ControllerField(playerOne);
    unreal::UObject** twoField = unreal::ControllerField(playerTwo);
    if (IsAttached() || oneField == nullptr || twoField == nullptr || *oneField == nullptr)
    {
        return false;
    }
    if (*twoField != nullptr)
    {
        // Something already gave player 2 a controller; it is not ours to replace.
        return false;
    }

    // Local-player objects live for the whole session, so their field addresses are fixed and
    // looked up once here rather than by name on every tick.
    *twoField = *oneField;
    oneField_ = oneField;
    twoField_ = twoField;
    lent_     = *oneField;
    return true;
}

void SharedCamera::Refresh() noexcept
{
    if (!IsAttached() || *twoField_ != lent_)
    {
        return;  // not attached, or the field no longer holds our loan: leave it to whoever changed it
    }
    if (*oneField_ != lent_)
    {
        // Player 1's controller was replaced (or cleared). Follow it, so player 2 never holds a
        // pointer to a controller the engine has destroyed.
        *twoField_ = *oneField_;
        lent_      = *oneField_;
        platform::Log(L"shared camera now follows player 1's new controller");
    }
}

bool SharedCamera::Detach() noexcept
{
    if (!IsAttached())
    {
        return true;
    }
    if (*twoField_ == lent_)
    {
        *twoField_ = nullptr;
    }
    const bool empty = *twoField_ == nullptr;
    oneField_ = twoField_ = nullptr;
    lent_     = nullptr;
    return empty;
}

}  // namespace lss::players
