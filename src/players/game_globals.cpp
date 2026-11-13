#include "players/game_globals.h"

#include "facts/cameras.h"
#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/field_action.h"
#include "facts/hud.h"
#include "facts/menu.h"
#include "facts/party.h"
#include "facts/world.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lss::players::game_globals {

namespace {

enum class Holds : std::uint8_t
{
    Pointer,  // the global keeps a pointer to the object
    Object,   // the global is the object
};

struct Entry
{
    Global         global;
    const wchar_t* name;
    std::uintptr_t ida;
    std::size_t    bytes;  // checked inside the image at the rebase
    Holds          holds;
};

constexpr std::size_t kUiSlots = facts::kEndUISystemSlots;  // EndUISystem's slot array (UiSlot's reach)
// EndUISystem as far as the modules read its slots: the array, and the field-action rows' pointer past it.
constexpr std::size_t kUiBytes = std::max(kUiSlots * sizeof(void*), std::size_t{facts::kUIActionRows} + sizeof(void*));

constexpr std::array<Entry, static_cast<std::size_t>(Global::Count)> kEntries{{
    {Global::Engine, L"GEngine", facts::kGEngine, sizeof(void*), Holds::Pointer},
    {Global::GameRoot, L"the field root", facts::kEndGameRoot, sizeof(void*), Holds::Pointer},
    {Global::PartyManager, L"the party manager", facts::kPartyManagerGlobal, sizeof(void*), Holds::Pointer},
    {Global::SquareInputOwner, L"Square's input object", facts::kSquareInputGlobal, sizeof(void*), Holds::Pointer},
    {Global::RootOverlay, L"the root overlay", facts::kRootOverlaySingleton, sizeof(void*), Holds::Pointer},
    {Global::FieldAction, L"the field-action object", facts::kFieldActionGlobal, sizeof(void*), Holds::Pointer},
    {Global::UiSystem, L"EndUISystem", facts::kEndUISystem, kUiBytes, Holds::Object},
    {Global::FrameNumber, L"GFrameNumber", facts::kFrameNumber, sizeof(std::uint32_t), Holds::Object},
    {Global::CameraManagerVTable, L"the camera manager's vtable", facts::kCameraManagerVTable, sizeof(void*), Holds::Object},
    {Global::VertexBufferVTable, L"the vertex buffer's vtable", facts::kVertexBufferVtable, sizeof(void*), Holds::Object},
    {Global::IndexBufferVTable, L"the index buffer's vtable", facts::kIndexBufferVtable, sizeof(void*), Holds::Object},
}};

constexpr bool InEnumOrder() noexcept
{
    for (std::size_t i = 0; i < kEntries.size(); ++i)
    {
        if (static_cast<std::size_t>(kEntries[i].global) != i)
        {
            return false;
        }
    }
    return true;
}
static_assert(InEnumOrder(), "one entry per global, in the enum's order");

// Set once at install, then only read.
std::array<std::uintptr_t, kEntries.size()> g_addresses{};

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    std::wstring missing;
    for (std::size_t i = 0; i < kEntries.size(); ++i)
    {
        g_addresses[i] = image.Rebase(kEntries[i].ida, kEntries[i].bytes).value_or(0);
        if (g_addresses[i] == 0)
        {
            try
            {
                missing += missing.empty() ? kEntries[i].name : std::wstring(L", ") + kEntries[i].name;
            }
            catch (...)
            {
            }
        }
    }
    if (!missing.empty())
    {
        platform::Log(L"game globals: outside the image, not as the research read them: {}; the modules that need them stand aside",
                      missing);
        return false;
    }
    return true;
}

std::uintptr_t Address(Global global) noexcept
{
    const auto index = static_cast<std::size_t>(global);
    return index < g_addresses.size() ? g_addresses[index] : 0;
}

void* Object(Global global) noexcept
{
    const std::uintptr_t address = Address(global);
    if (address == 0)
    {
        return nullptr;
    }
    if (kEntries[static_cast<std::size_t>(global)].holds == Holds::Object)
    {
        return reinterpret_cast<void*>(address);
    }
    return platform::ReadPointer(reinterpret_cast<const void*>(address), 0);
}

std::uintptr_t UiSlotAddress(std::uint32_t slot) noexcept
{
    const std::uintptr_t slots = Address(Global::UiSystem);
    return slots != 0 && slot < kUiSlots ? slots + slot * sizeof(void*) : 0;
}

void* UiSlot(std::uint32_t slot) noexcept
{
    const std::uintptr_t address = UiSlotAddress(slot);
    return address != 0 ? platform::ReadPointer(reinterpret_cast<const void*>(address), 0) : nullptr;
}

void* GameViewportClient() noexcept
{
    return platform::ReadPointer(Object(Global::Engine), facts::kEngineGameViewport);
}

}  // namespace lss::players::game_globals
