#include "player_two/charapop_centre.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>

namespace lss::player_two::charapop_centre {

namespace {

using Clock = std::chrono::steady_clock;

// CharaPop_SetOverrideCenter(bool enable, const FVector* centre): writes M+0x119C and, when enabling,
// M+0x11A0..+0x11A8 (a leaf; game thread only). Entry: mov rax,[rip+...] (g_EndGameRoot); mov r8,[rax+180h];
// mov [r8+119Ch],cl. The first instruction's displacement is within the image.
constexpr std::uint8_t kSetCentreBytes[] = {0x48, 0x8B, 0x05, 0xE1, 0xBE, 0x84, 0x07, 0x4C,
                                            0x8B, 0x80, 0x80, 0x01, 0x00, 0x00, 0x41, 0x88};
constexpr platform::FunctionFact kSetCentre{L"CharaPop_SetOverrideCenter", 0x1418B2F00, kSetCentreBytes};
using SetCentreFn = void (*)(bool enable, const float* centre);

// The override as the score reads it (§2), and the population parameters (floats, cm; int budget), filled
// once by CharaPop_LoadParamsFromEndParam.
constexpr std::uint32_t  kOverrideOn     = 0x119C;  // u8
constexpr std::uint32_t  kOverrideCentre = 0x11A0;  // FVector
constexpr std::uint32_t  kLeaderOn       = 0x11BE;  // u8
constexpr std::uint32_t  kLeaderCentre   = 0x11B0;  // FVector
constexpr std::uint32_t  kManagerSelect  = 0x18;    // pointer; the threshold at +0xE8 (i32)
constexpr std::uint32_t  kThreshold      = 0xE8;
constexpr std::uintptr_t kDistanceMax    = 0x146FCE85C;
constexpr std::uintptr_t kHardLimit      = 0x146FCE860;
constexpr std::uintptr_t kSpawnMaxNum    = 0x146FCE850;

constexpr std::uint32_t kEvery        = 10;  // ticks between passes
constexpr auto          kSummaryEvery = std::chrono::seconds(60);

/// A parameter the mod widens: the game's loaded value and what the mod last wrote.
struct Param
{
    float* address = nullptr;
    float  loaded  = 0.0f;
    float  written = 0.0f;
    bool   held    = false;  // the mod's value is in place

    void Widen(float by) noexcept
    {
        float now = 0.0f;
        if (address == nullptr || !platform::CopyGuarded(&now, address, sizeof(now)))
        {
            return;
        }
        if (!held || now != written)
        {
            loaded = now;  // first time, or the game loaded its parameters again
        }
        written = loaded + by;
        held    = platform::WriteGuarded(address, &written, sizeof(written));
    }

    void Restore() noexcept
    {
        float now = 0.0f;
        if (held && address != nullptr && platform::CopyGuarded(&now, address, sizeof(now)) && now == written)
        {
            platform::WriteGuarded(address, &loaded, sizeof(loaded));
        }
        held = false;
    }
};

std::uintptr_t   g_gameRoot  = 0;  // set once at install
SetCentreFn      g_setCentre = nullptr;
const std::int32_t* g_spawnMaxNum = nullptr;
bool             g_installed = false;
std::atomic<bool> g_enabled{true};

// Game thread.
Param             g_distanceMax, g_hardLimit;
bool              g_ownsCentre   = false;
float             g_written[3]   = {};
bool              g_deferring    = false;  // an event's centre is in place
std::uint32_t     g_ticks        = 0;
Clock::time_point g_lastSummary{};

bool SameCentre(const std::byte* manager) noexcept
{
    float now[3]{};
    return platform::CopyGuarded(now, manager + kOverrideCentre, sizeof(now)) && now[0] == g_written[0] &&
           now[1] == g_written[1] && now[2] == g_written[2];
}

/// Clears the override if it is still the mod's, and puts the parameters back.
void Stop(const wchar_t* why) noexcept
{
    const bool had = g_ownsCentre || g_distanceMax.held || g_hardLimit.held;
    if (g_ownsCentre)
    {
        const charapop::Objects objects = charapop::ObjectsOf(g_gameRoot);
        if (objects.manager != nullptr && ReadField<std::uint8_t>(objects.manager, kOverrideOn) != 0 && SameCentre(objects.manager))
        {
            const float zero[3] = {};
            platform::CallGuarded(g_setCentre, false, static_cast<const float*>(zero));
        }
        g_ownsCentre = false;
    }
    g_distanceMax.Restore();
    g_hardLimit.Restore();
    if (had)
    {
        platform::Log(L"npc centre: back to the game's own (the leader, distances {:.0f} / {:.0f}): {}", g_distanceMax.loaded,
                      g_hardLimit.loaded, why);
    }
}

bool CharacterAt(const void* character, float (&out)[3]) noexcept
{
    return character != nullptr && charapop::ActorLocation(const_cast<void*>(character), out);
}

void LogSummary(const std::byte* manager, const float (&centre)[3], float apart) noexcept
{
    const auto now = Clock::now();
    if (now - g_lastSummary < kSummaryEvery)
    {
        return;
    }
    g_lastSummary = now;
    float leader[3]{};
    platform::CopyGuarded(leader, manager + kLeaderCentre, sizeof(leader));
    const void* select = ReadPointer(manager, kManagerSelect);
    platform::Log(L"npc centre: midpoint ({:.0f}, {:.0f}, {:.0f}), players {:.0f} m apart, {:.0f} m from the leader; distances "
                  L"{:.0f} -> {:.0f}, hard limit {:.0f} -> {:.0f}; threshold {}, spawn max {}",
                  centre[0], centre[1], centre[2], apart / 100.0f, charapop::Distance(centre, leader) / 100.0f,
                  g_distanceMax.loaded, g_distanceMax.written, g_hardLimit.loaded, g_hardLimit.written,
                  select != nullptr ? ReadField<std::int32_t>(select, kThreshold) : -1,
                  g_spawnMaxNum != nullptr ? ReadField<std::int32_t>(g_spawnMaxNum, 0) : -1);
}

void Pass() noexcept
{
    const charapop::Objects objects = charapop::ObjectsOf(g_gameRoot);
    if (objects.manager == nullptr)
    {
        return;
    }
    const bool overrideOn = ReadField<std::uint8_t>(objects.manager, kOverrideOn) != 0;
    if (overrideOn && (!g_ownsCentre || !SameCentre(objects.manager)))
    {
        // An event's centre: it owns the population until it clears it.
        if (!g_deferring)
        {
            g_deferring  = true;
            g_ownsCentre = false;
            g_distanceMax.Restore();
            g_hardLimit.Restore();
            platform::Log(L"npc centre: an event set its own centre; left to it until it clears");
        }
        return;
    }
    if (g_deferring)
    {
        g_deferring = false;
        platform::Log(L"npc centre: the event's centre cleared; following both players again");
    }
    float one[3]{}, two[3]{};
    const void* playerOne = ReadPointer(generation::PlayerOneController(), facts::kControllerPawn);
    if (!CharacterAt(playerOne, one) || !CharacterAt(generation::Member(), two))
    {
        return;
    }
    const float centre[3] = {(one[0] + two[0]) * 0.5f, (one[1] + two[1]) * 0.5f, (one[2] + two[2]) * 0.5f};
    const float apart     = charapop::Distance(one, two);
    if (!platform::CallGuarded(g_setCentre, true, static_cast<const float*>(centre)))
    {
        return;
    }
    if (!g_ownsCentre)
    {
        platform::Log(L"npc centre: the population follows the midpoint of both players (the game's override centre)");
    }
    g_ownsCentre = true;
    for (int i = 0; i < 3; ++i)
    {
        g_written[i] = centre[i];
    }
    g_distanceMax.Widen(apart * 0.5f);
    g_hardLimit.Widen(apart * 0.5f);
    LogSummary(objects.manager, centre, apart);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root     = image.Rebase(charapop::kGameRoot, sizeof(void*));
    const auto setter   = platform::FindFunction(image, kSetCentre);
    const auto distance = image.Rebase(kDistanceMax, sizeof(float));
    const auto limit    = image.Rebase(kHardLimit, sizeof(float));
    const auto spawnMax = image.Rebase(kSpawnMaxNum, sizeof(std::int32_t));
    g_gameRoot            = root ? *root : 0;
    g_setCentre           = setter ? reinterpret_cast<SetCentreFn>(*setter) : nullptr;
    g_distanceMax.address = distance ? reinterpret_cast<float*>(*distance) : nullptr;
    g_hardLimit.address   = limit ? reinterpret_cast<float*>(*limit) : nullptr;
    g_spawnMaxNum         = spawnMax ? reinterpret_cast<const std::int32_t*>(*spawnMax) : nullptr;
    g_installed = g_gameRoot != 0 && g_setCentre != nullptr && g_distanceMax.address != nullptr && g_hardLimit.address != nullptr;
    platform::Log(L"npc centre: {}", g_installed ? L"installed (the population follows both players while split; Ctrl+F10 "
                                                   L"switches it for comparison)"
                                                 : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Tick(bool splitShown, bool playerTwoPlays) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    if (!splitShown || !playerTwoPlays || !generation::Active() || !g_enabled.load(std::memory_order_relaxed))
    {
        g_deferring = false;
        Stop(!g_enabled.load(std::memory_order_relaxed) ? L"switched off" : L"the split closed or player 2 stopped playing");
        return;
    }
    if (++g_ticks % kEvery == 0)
    {
        Pass();
    }
}

void SetEnabled(bool on) noexcept
{
    g_enabled.store(on, std::memory_order_relaxed);
    g_lastSummary = {};
    platform::Log(L"npc centre: switched {} (the population {})", on ? L"on" : L"off",
                  on ? L"follows both players while split" : L"follows the leader, as the game alone does");
}

bool Enabled() noexcept
{
    return g_enabled.load(std::memory_order_relaxed);
}

void Release() noexcept
{
    if (!g_installed)
    {
        return;
    }
    // The setter is game-thread only (no marshalling) and the parameters are read by the game's tick.
    if (generation::OnGameThread())
    {
        Stop(L"the mod is unloading");
    }
    else if (g_ownsCentre || g_distanceMax.held || g_hardLimit.held)
    {
        platform::Log(L"npc centre: unloading off the game thread; the centre and distances stay until the game resets them");
    }
}

bool Installed() noexcept
{
    return g_installed;
}

}  // namespace lss::player_two::charapop_centre
