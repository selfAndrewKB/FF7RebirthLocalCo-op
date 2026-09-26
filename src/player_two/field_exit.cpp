#include "player_two/field_exit.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <cstdint>
#include <iterator>

namespace lss::player_two::field_exit {

namespace {

// Facts read from the database (RE-p2-retry-reload.md §2.1, §2.3, recipe; this build).
constexpr std::uint8_t kRequestExitBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0x5D, 0x00, 0x01};
constexpr platform::FunctionFact kRequestExit{L"AEndGameMode_RequestFieldExit_Clear71", 0x1410A9168, kRequestExitBytes};
// Its calls: the game state's "leave the field" state entry (every retry, reload and load), and the
// jump actor's repeat.
constexpr std::uintptr_t kRequestExitCalls[] = {0x14154F26B, 0x1410A9E94};

constexpr std::uintptr_t kGameMode   = 0x1490FEDE8;  // the AEndGameMode (a pointer global)
constexpr std::uint32_t  kFieldStep  = 0x70;         // EEndFieldStep; 13 once the entry steps are done
constexpr std::uint32_t  kRunning    = 0x71;         // 1 while the field runs
constexpr std::uint8_t   kStepSettled = 13;

constexpr int kMaxLines = 200;

using RequestExitFn = void (*)();

// Set once at install.
platform::CallRedirect g_calls[2];
std::uintptr_t         g_gameMode     = 0;  // the address holding the game mode
std::uintptr_t         g_partyManager = 0;  // the address holding the party manager
LeaveFn                g_leave        = nullptr;
void*                  g_context      = nullptr;

// Game thread.
std::uint16_t g_logged = 0xFFFF;
int           g_lines  = 0;

template <class T>
T Field(std::uintptr_t address) noexcept
{
    T value{};
    if (address != 0)
    {
        platform::CopyGuarded(&value, reinterpret_cast<const void*>(address), sizeof(T));
    }
    return value;
}

std::uintptr_t GameMode() noexcept
{
    return Field<std::uintptr_t>(g_gameMode);
}

/// Replaces both calls of the game's "leave the field" request: player 2 leaves first, while the
/// party is still intact, then the game's request runs.
void RequestExit() noexcept
{
    const std::uintptr_t gameMode = GameMode();
    if (gameMode != 0 && Field<std::uint8_t>(gameMode + kRunning) != 0 && g_leave != nullptr && generation::OnGameThread())
    {
        platform::Log(L"field exit: the game leaves the field (step {}); player 2 leaves first",
                      Field<std::uint8_t>(gameMode + kFieldStep));
        g_leave(g_context);
    }
    reinterpret_cast<RequestExitFn>(g_calls[0].Original())();
}

}  // namespace

bool Install(const platform::GameImage& image, LeaveFn leave, void* context) noexcept
{
    const auto target  = platform::FindFunction(image, kRequestExit);
    const auto mode    = image.Rebase(kGameMode, sizeof(void*));
    const auto manager = image.Rebase(facts::kPartyManagerGlobal, sizeof(void*));
    bool       ok      = target && mode && manager;
    if (ok)
    {
        g_gameMode     = *mode;
        g_partyManager = *manager;
        g_leave        = leave;
        g_context      = context;
    }
    // Both calls or neither: the leave must come before every teardown the request starts.
    for (std::size_t i = 0; ok && i < std::size(kRequestExitCalls); ++i)
    {
        const auto site = image.Rebase(kRequestExitCalls[i], 5);
        ok = site && g_calls[i].Install(*site, *target, reinterpret_cast<std::uintptr_t>(&RequestExit));
    }
    if (!ok)
    {
        for (auto& call : g_calls)
        {
            if (call.IsInstalled() && !call.Restore())
            {
                platform::Log(L"field exit: a call could not be put back; it stays (it passes through)");
            }
        }
    }
    platform::Log(L"field exit: {}", ok ? L"player 2 leaves before a retry, reload or load leaves the field"
                                        : L"UNAVAILABLE (a check failed); only the running-byte check catches a field exit");
    return ok;
}

void Remove() noexcept
{
    for (auto* call : {&g_calls[1], &g_calls[0]})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"field exit: a call was changed after us; left in place (it passes through)");
        }
    }
}

bool FieldLeft() noexcept
{
    const std::uintptr_t gameMode = GameMode();
    return gameMode != 0 && Field<std::uint8_t>(gameMode + kRunning) == 0;
}

bool FieldReady() noexcept
{
    if (g_gameMode == 0)
    {
        return true;  // not installed: nothing to wait for
    }
    const std::uintptr_t gameMode = GameMode();
    const std::uintptr_t manager  = Field<std::uintptr_t>(g_partyManager);
    return gameMode != 0 && manager != 0 && Field<std::uint8_t>(gameMode + kFieldStep) == kStepSettled &&
           Field<std::uint8_t>(gameMode + kRunning) == 1 && Field<std::uint8_t>(manager) == 0;
}

void Watch() noexcept
{
    const std::uintptr_t gameMode = GameMode();
    if (gameMode == 0 || g_lines >= kMaxLines)
    {
        return;
    }
    const auto now = static_cast<std::uint16_t>(Field<std::uint8_t>(gameMode + kFieldStep) |
                                                (Field<std::uint8_t>(gameMode + kRunning) << 8));
    if (now != g_logged)
    {
        g_logged = now;
        ++g_lines;
        platform::Log(L"field exit: field step {} running {}", now & 0xFF, now >> 8);
    }
}

}  // namespace lss::player_two::field_exit
