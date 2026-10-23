#include "player_two/session/field_exit.h"

#include "facts/party.h"
#include "facts/world.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"

#include <cstdint>
#include <span>

namespace lss::player_two::field_exit {

namespace {

// Facts read from the database (RE-p2-retry-reload.md §2.1, §2.3, recipe; RE-p2-session-levers.md §4, §5;
// this build). Its three callers: the game state's "leave the field" state entry (every retry, reload and
// load), the jump actor's repeat, and the Blueprint ExitLoation native's tail jump. The entry redirect
// replaces its first instruction, `mov eax, 28h` (5 bytes, no RIP-relative operand, no branch lands
// inside it), so all three reach RequestExit.
constexpr std::uint8_t kRequestExitBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0x5D, 0x00, 0x01};
constexpr platform::FunctionFact kRequestExit{L"AEndGameMode_RequestFieldExit_Clear71", 0x1410A9168, kRequestExitBytes};
constexpr std::size_t kFirstInstruction = 5;

using facts::kEndGameRoot;
constexpr std::uint32_t  kFieldStep  = 0x70;         // EEndFieldStep; 13 once the entry steps are done
constexpr std::uint32_t  kRunning    = 0x71;         // 1 while the field runs
constexpr std::uint8_t   kStepSettled = 13;


using RequestExitFn = void (*)();

// Set once at install.
platform::EntryRedirect g_entry;
std::uintptr_t          g_gameMode     = 0;  // the address holding the game mode
std::uintptr_t          g_partyManager = 0;  // the address holding the party manager
LeaveFn                 g_leave        = nullptr;
void*                   g_context      = nullptr;

// Game thread.
std::uint16_t g_logged = 0xFFFF;

std::uintptr_t GameMode() noexcept
{
    return ReadAt<std::uintptr_t>(g_gameMode);
}

/// Replaces the entry of the game's "leave the field" request: player 2 leaves first, while the
/// party is still intact, then the game's request runs.
void RequestExit() noexcept
{
    const std::uintptr_t gameMode = GameMode();
    if (gameMode != 0 && ReadAt<std::uint8_t>(gameMode + kRunning) != 0 && g_leave != nullptr &&
        generation::OnGameThread())
    {
        platform::Log(L"field exit: the game leaves the field (step {}); player 2 leaves first",
                      ReadAt<std::uint8_t>(gameMode + kFieldStep));
        g_leave(g_context);
    }
    reinterpret_cast<RequestExitFn>(g_entry.Original())();
}

}  // namespace

bool Install(const platform::GameImage& image, LeaveFn leave, void* context) noexcept
{
    const auto target  = platform::FindFunction(image, kRequestExit);
    const auto mode    = image.Rebase(kEndGameRoot, sizeof(void*));
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    bool       ok      = target && mode && manager;
    if (ok)
    {
        g_gameMode     = *mode;
        g_partyManager = *manager;
        g_leave        = leave;
        g_context      = context;
        ok             = g_entry.Install(*target, image.EntryInstruction(kRequestExit.address, std::span<const std::uint8_t>(kRequestExitBytes, kFirstInstruction)),
                                         reinterpret_cast<std::uintptr_t>(&RequestExit));
    }
    platform::Log(L"field exit: {}", ok ? L"player 2 leaves before a retry, reload, load or Blueprint exit leaves the field"
                                        : L"UNAVAILABLE (a check failed); only the running-byte check catches a field exit");
    return ok;
}

void Remove() noexcept
{
    g_leave = nullptr;  // an entry left in place (changed after us) then only runs the game's request
    if (g_entry.IsInstalled() && !g_entry.Restore())
    {
        platform::Log(L"field exit: the entry was changed after us; left in place (it passes through)");
    }
}

bool FieldLeft() noexcept
{
    const std::uintptr_t gameMode = GameMode();
    return gameMode != 0 && ReadAt<std::uint8_t>(gameMode + kRunning) == 0;
}

bool FieldReady() noexcept
{
    if (g_gameMode == 0)
    {
        return true;  // not installed: nothing to wait for
    }
    const std::uintptr_t gameMode = GameMode();
    const std::uintptr_t manager  = ReadAt<std::uintptr_t>(g_partyManager);
    return gameMode != 0 && manager != 0 && ReadAt<std::uint8_t>(gameMode + kFieldStep) == kStepSettled &&
           ReadAt<std::uint8_t>(gameMode + kRunning) == 1 && ReadAt<std::uint8_t>(manager) == 0;
}

void Watch() noexcept
{
    const std::uintptr_t gameMode = GameMode();
    if (gameMode == 0)
    {
        return;
    }
    const auto now = static_cast<std::uint16_t>(ReadAt<std::uint8_t>(gameMode + kFieldStep) |
                                                (ReadAt<std::uint8_t>(gameMode + kRunning) << 8));
    if (now != g_logged)
    {
        g_logged = now;
        platform::Log(L"field exit: field step {} running {}", now & 0xFF, now >> 8);
    }
}

}  // namespace lss::player_two::field_exit
