#pragma once

// The game's globals that more than one module reads, rebased once. Each global is either a pointer the game keeps
// to a heap object (GEngine, the party manager, the field root) or the object itself at a fixed address (the UI
// system, the frame number, a class's vtable): the table says which, and Object() reads each the one right way.
// Rebased and verified once at install, which runs right after weak_object's and before every module that reads
// them; a module checks Address() at its own install as it checked its own rebase before. The globals are the game's
// own fixed addresses, so any thread may read them; what they point at belongs to the thread that owns the object.

#include <cstdint>

namespace lss::platform {
class GameImage;
}

namespace lss::players::game_globals {

/// A global of the game's that more than one module reads.
enum class Global : std::uint8_t
{
    Engine,              // UEngine* GEngine
    GameRoot,            // the field's root: a pointer the game mode fills, its slots the field's managers
    PartyManager,        // the party manager: +0x140 the primary controller, +0x148 the leader
    SquareInputOwner,    // Square's input object (the sticks, the action tables): the UI system's slot 2 is this cell
    RootOverlay,         // the in-game UI's root overlay
    FieldAction,         // the field-action object (the shooting targets' registry, the traversal rumble list)
    UiSystem,            // EndUISystem: the global is the object, its first qwords the managers' slots
    FrameNumber,         // GFrameNumber (u32): the global is the value
    CameraManagerVTable, // the camera manager's vtable: the global is the table
    VertexBufferVTable,  // the menu renderer's vertex buffer's vtable
    IndexBufferVTable,   // its index buffer's vtable
    Count,
};

/// Rebases every global. False (logged, naming each) when one is outside the image; that one's address is then 0 and
/// the modules that need it stand aside at their own install.
bool Install(const platform::GameImage& image) noexcept;

/// The global's live address (the pointer's own cell for a pointer global), or 0 when it is outside the image.
std::uintptr_t Address(Global global) noexcept;

/// The game object the global names: the object a pointer global points at now (null while the game has not set it,
/// or unread), or the object global itself.
void* Object(Global global) noexcept;

/// EndUISystem's manager in `slot`, or null (unset, unread, or a slot past the array).
void* UiSlot(std::uint32_t slot) noexcept;

/// The live address of EndUISystem's `slot` (for the scopes that put a seat's manager there), or 0.
std::uintptr_t UiSlotAddress(std::uint32_t slot) noexcept;

/// GEngine's game viewport client, or null.
void* GameViewportClient() noexcept;

}  // namespace lss::players::game_globals
