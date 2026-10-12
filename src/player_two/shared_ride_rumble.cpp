#include "player_two/shared_ride_rumble.h"

#include "platform/log.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "players/unreal_access.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::shared_ride_rumble {

namespace {

// Read from both exes (VibrationChannel_ApplyToPC758, 1.004 0x1408A177C, 1.005 0x141896890; the apply's `PC+0x758` in
// both PrimaryPC_ApplyVibrationChannels758), 2026-10-12.
constexpr std::uint32_t kFfbComponent = 0x758;  // the controller's force-feedback component (embedded)
constexpr std::uint32_t kManager      = 0x8;    // ... its manager (the slot table's owner)
constexpr std::uint32_t kSlots        = 0x90;   // the manager's slot array
constexpr std::uint32_t kSlotCount    = 0x98;
constexpr std::uint32_t kSlotSize     = 0x40;
constexpr std::uint32_t kSlotName     = 0x20;   // the effect the slot plays now (an FName)
constexpr int           kMaxSlots     = 16;     // channels 5..10 (1.004) or 5..11 (1.005) live within these
constexpr std::size_t   kMaxSeen      = 12;

struct Side
{
    const void* component = nullptr;
    const void* manager   = nullptr;
    bool operator==(const Side&) const = default;
};

Side SideOf(const void* controller) noexcept
{
    Side side;
    // The component is part of the controller (the apply passes `PC+0x758` itself), so only its manager is a pointer.
    side.component = controller != nullptr ? static_cast<const std::byte*>(controller) + kFfbComponent : nullptr;
    side.manager   = controller != nullptr ? ReadPointer(controller, kFfbComponent + kManager) : nullptr;
    return side;
}

std::int32_t RawSlotCount(const void* manager) noexcept
{
    return manager != nullptr ? ReadField<std::int32_t>(manager, kSlotCount) : 0;
}

int SlotCountOf(const void* manager) noexcept
{
    return std::clamp(RawSlotCount(manager), 0, kMaxSlots);  // the slots followed
}

std::array<std::uint64_t, kMaxSlots> NamesOf(const void* manager) noexcept
{
    std::array<std::uint64_t, kMaxSlots> names{};
    const void* const slots = manager != nullptr ? ReadPointer(manager, kSlots) : nullptr;
    const int         count = SlotCountOf(manager);
    for (int i = 0; slots != nullptr && i < count; ++i)
    {
        names[i] = ReadField<std::uint64_t>(slots, static_cast<std::uint32_t>(i * kSlotSize + kSlotName));
    }
    return names;
}

// Game thread.
Side g_one;
Side g_two;
bool g_noted  = false;
bool g_riding = false;
std::array<std::uint64_t, kMaxSlots> g_namesOne{};
std::array<std::uint64_t, kMaxSlots> g_namesTwo{};
std::array<std::uint64_t, kMaxSeen>  g_seen{};
std::size_t                          g_seenCount = 0;
unsigned                             g_changes   = 0;

std::wstring Pointer(const void* p)
{
    return p != nullptr ? std::format(L"0x{:X}", reinterpret_cast<std::uintptr_t>(p)) : std::wstring(L"none");
}

void NoteSides(const Side& one, const Side& two)
{
    if (g_noted && one == g_one && two == g_two)
    {
        return;
    }
    g_noted = true;
    g_one   = one;
    g_two   = two;
    const bool shared = one.manager != nullptr && one.manager == two.manager;
    platform::Log(L"shared ride rumble: player 1's force-feedback component {} (manager {}, {} slots), player 2's {} "
                  L"(manager {}, {} slots): {}",
                  Pointer(one.component), Pointer(one.manager), RawSlotCount(one.manager), Pointer(two.component),
                  Pointer(two.manager), RawSlotCount(two.manager),
                  shared ? L"ONE manager for both: a copy for player 2 of an effect player 1 plays on the same channel "
                           L"would be skipped by the game's apply"
                  : two.manager == nullptr ? L"player 2 has no manager"
                                           : L"separate managers: a copy for player 2 would play on its own");
}

void Seen(std::uint64_t name)
{
    if (name == 0 || g_seenCount == kMaxSeen ||
        std::find(g_seen.begin(), g_seen.begin() + g_seenCount, name) != g_seen.begin() + g_seenCount)
    {
        return;
    }
    g_seen[g_seenCount++] = name;
}

void Compare(const wchar_t* who, const void* manager, std::array<std::uint64_t, kMaxSlots>& last)
{
    const auto names = NamesOf(manager);
    for (int i = 0; i < kMaxSlots; ++i)
    {
        if (names[i] == last[i])
        {
            continue;
        }
        ++g_changes;
        Seen(names[i]);
        platform::LogDebug(L"shared ride rumble: {}'s slot {}: {} -> {}", who, i,
                           last[i] != 0 ? players::unreal::NameText(last[i]) : std::wstring(L"none"),
                           names[i] != 0 ? players::unreal::NameText(names[i]) : std::wstring(L"none"));
        last[i] = names[i];
    }
}

void Begin()
{
    g_riding    = true;
    g_seenCount = 0;
    g_changes   = 0;
    g_namesOne  = NamesOf(g_one.manager);
    g_namesTwo  = NamesOf(g_two.manager);
    platform::Log(L"shared ride rumble: a shared ride starts; slot changes follow at Debug");
}

void End()
{
    g_riding = false;
    std::wstring seen;
    for (std::size_t i = 0; i < g_seenCount; ++i)
    {
        seen += (i == 0 ? L"" : L", ") + players::unreal::NameText(g_seen[i]);
    }
    platform::Log(L"shared ride rumble: the shared ride ended; {} slot change(s), effects seen: {}", g_changes,
                  seen.empty() ? std::wstring(L"none") : seen);
}

}  // namespace

void Tick(bool sharedRide) noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    try
    {
        void* const one = generation::PlayerOneController();
        void* const two = generation::PlayerTwoController();
        if (one != nullptr && two != nullptr)
        {
            NoteSides(SideOf(one), SideOf(two));
        }
        else
        {
            g_one = SideOf(one);  // player 1's slots are still followed during a ride
            g_two = {};
        }
        if (sharedRide && !g_riding)
        {
            Begin();
        }
        if (!g_riding)
        {
            return;
        }
        if (!sharedRide)
        {
            End();
            return;
        }
        if (platform::Enabled(platform::Level::Debug))
        {
            Compare(L"player 1", g_one.manager, g_namesOne);
            if (g_two.manager != nullptr && g_two.manager != g_one.manager)
            {
                Compare(L"player 2", g_two.manager, g_namesTwo);
            }
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::shared_ride_rumble
