#include "player_two/attack_cameras.h"

#include "facts/cameras.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/primary_controller.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace lss::player_two::attack_cameras {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-attack-cameras.md §1.1, §1.3, §9.1 (S-ATKCAM, S-ATKCAMEDGE; both builds).
// The battle system (a pointer global; it creates the list with itself and frees it with itself, §9.1d) and its
// sequence list at +0x50: entry list head +0x08; per-camera state +0x48 (u8 last active priority) and +0x70..+0x8F (the
// previous focus point and camera location, an FVector and 4 bytes of padding each).
using facts::kBattleStateGlobal;
constexpr std::uint32_t  kList           = 0x50;
constexpr std::uint32_t  kEntries        = 0x08;
constexpr std::uint32_t  kLastPriority   = 0x48;
constexpr std::uint32_t  kSmoothing      = 0x70;
constexpr std::size_t    kSmoothingSize  = 0x20;
constexpr std::uint32_t  kPreviousCamera = 0x80;  // FVector: the previous camera location (cm)
// Entry {u8 priority +0, FWeakObjectPtr sequence actor +4, ..., u8 active +0x14, next +0x18}.
using facts::kSequenceEntryPriority;
using facts::kSequenceEntryActor;
constexpr std::uint32_t kEntryActive   = 0x14;
constexpr std::uint32_t kEntryNext     = 0x18;
constexpr std::size_t   kMaxEntries    = 32;  // the list holds a handful (one per playing sequence)
// AEndBattleLevelSequenceActor's camera actor and AEndBattleCameraActor's owner (facts/cameras.h); its explicit target
// (reflection offset, the same in both builds).
using facts::kSequenceActorCamera;
using facts::kBattleCameraOwner;
constexpr std::uint32_t                     kCameraTarget = 0xA68;

// The shot change (§9.3): BattleCamSeqList_AnyActive_ApplyCam's call of FirstPC_PCM_SetCameraCutBit400 (cl = 1; 1.005
// also dl = 1, passed on to its anim world manager), and the game's own PrimaryPCM_SetGameCameraCutThisFrame, which sets
// bGameCameraCutThisFrame (PCM+0x2C10 bit 10) on GetEndPlayerCameraManager()'s and tells the anim world manager.
constexpr std::uintptr_t kCutCall       = 0x140C42F23;
constexpr std::uintptr_t kFirstPcCut    = 0x140F9A40C;
constexpr std::uint8_t   kPrimaryCutBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8};  // mov eax, 28h; call __alloca_probe
constexpr platform::FunctionFact kPrimaryCut{L"the primary camera manager's cut", 0x1417ED43C, kPrimaryCutBytes};

constexpr auto kStatusEvery = std::chrono::seconds(10);

using CutFn = void (*)(std::uint64_t cut, std::uint64_t second);

// Set once at install.
std::uintptr_t         g_battleSystem = 0;  // the address holding the battle system
platform::CallRedirect g_cutCall;
CutFn                  g_primaryCut   = nullptr;

// Game thread.
struct Copy
{
    std::uint8_t                              lastPriority = 0;
    std::array<std::byte, kSmoothingSize>     smoothing{};
};
const void* g_copyList = nullptr;  // the list player 2's copy belongs to (a new list seeds a new copy)
Copy        g_copy;
bool        g_inside   = false;    // inside player 2's update, with the copy in the list
Copy        g_saved;               // player 1's, while g_inside
std::array<float, 3> g_playerOneCamera{};  // player 1's previous camera location, while g_inside (for the log)
struct SavedFlag
{
    void*         entry;
    std::uint64_t actor;     // the entry's sequence-actor weak (+4): with the priority, tells a reused address apart
    std::uint8_t  priority;
    std::uint8_t  active;
};
std::array<SavedFlag, kMaxEntries> g_flags{};
std::size_t                        g_flagCount = 0;
// The entries player 2's camera found active at its last update, and player 1's (from the bytes saved before it).
std::array<const void*, kMaxEntries> g_activeTwo{};
std::size_t                          g_activeTwoCount = 0;
std::array<const void*, kMaxEntries> g_activeOne{};
std::size_t                          g_activeOneCount = 0;
bool                                 g_capSaid        = false;  // the walk met its cap once (logged)

struct Counts
{
    std::uint32_t updates = 0, entriesTwo = 0, entriesOne = 0, cutsTwo = 0, cutsOne = 0, lists = 0, onlyTwo = 0;
};
Counts            g_counts;
platform::Every g_status{kStatusEvery};

void* ListNow() noexcept
{
    void* system = g_battleSystem != 0 ? ReadPointer(reinterpret_cast<const void*>(g_battleSystem), 0) : nullptr;
    return ReadPointer(system, kList);
}

Copy Read(const void* list) noexcept
{
    Copy copy;
    copy.lastPriority = ReadField<std::uint8_t>(list, kLastPriority);
    platform::CopyGuarded(copy.smoothing.data(), static_cast<const std::byte*>(list) + kSmoothing, kSmoothingSize);
    return copy;
}

void Write(void* list, const Copy& copy) noexcept
{
    WriteField<std::uint8_t>(list, kLastPriority, copy.lastPriority);
    std::memcpy(static_cast<std::byte*>(list) + kSmoothing, copy.smoothing.data(), kSmoothingSize);
}

std::array<float, 3> PreviousCamera(const Copy& copy) noexcept
{
    std::array<float, 3> location{};
    std::memcpy(location.data(), copy.smoothing.data() + (kPreviousCamera - kSmoothing), sizeof(location));
    return location;
}

/// Calls `visit(entry)` for each entry of the list, at most kMaxEntries.
template <class Visit>
void ForEachEntry(const void* list, Visit&& visit) noexcept
{
    void* entry = ReadPointer(list, kEntries);
    for (std::size_t i = 0; entry != nullptr && i < kMaxEntries; ++i)
    {
        visit(entry);
        entry = ReadPointer(entry, kEntryNext);
    }
    if (entry != nullptr && !g_capSaid)
    {
        g_capSaid = true;
        platform::LogDebug(L"attack cameras: the sequence list holds more than {} entries; the rest keep the last update's "
                           L"active byte",
                           kMaxEntries);
    }
}

SavedFlag Identity(void* entry, std::uint8_t active) noexcept
{
    return {entry, ReadField<std::uint64_t>(entry, kSequenceEntryActor),
            ReadField<std::uint8_t>(entry, kSequenceEntryPriority), active};
}

bool SameEntry(const SavedFlag& saved, void* entry) noexcept
{
    const SavedFlag now = Identity(entry, 0);
    return saved.entry == entry && saved.actor == now.actor && saved.priority == now.priority;
}


bool Contains(const std::array<const void*, kMaxEntries>& set, std::size_t count, const void* entry) noexcept
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (set[i] == entry)
        {
            return true;
        }
    }
    return false;
}

const wchar_t* PriorityText(std::uint8_t priority) noexcept
{
    switch (priority)
    {
        case 10: return L"SyncAction (a grab or catch)";
        case 20: return L"Player (a party skill)";
        case 30: return L"Enemy (an enemy special)";
        case 40: return L"Burst";
        case 50: return L"Summon";
        case 60: return L"ChangePhase";
        case 70: return L"Menu";
        default: return L"other";
    }
}

/// Logs an entry that has just become active for one camera (Debug, once per sequence and camera).
void LogStarted(const void* entry, const wchar_t* camera, float metresFromOne) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        const auto  priority = ReadField<std::uint8_t>(entry, kSequenceEntryPriority);
        const void* actor    = leader_scope::ObjectAt(static_cast<const std::byte*>(entry) + kSequenceEntryActor);
        const void* shot     = ReadPointer(actor, kSequenceActorCamera);
        const std::wstring smoothing =
            metresFromOne < 0.0f ? std::wstring()
                                 : std::format(L"; its smoothing starts from player 2's own camera ({:.1f} m from player 1's)",
                                               metresFromOne);
        platform::LogDebug(L"attack cameras: {}'s camera plays a battle camera sequence, priority {} {}, owner {}, explicit "
                           L"target {}{}",
                           camera, priority, PriorityText(priority), Who(ReadPointer(shot, kBattleCameraOwner)),
                           Who(ReadPointer(shot, kCameraTarget)), smoothing);
    }
    catch (...)
    {
    }
}

/// The game's cut call in BattleCamSeqList_AnyActive_ApplyCam: inside player 2's update, the game's own cut of the primary
/// camera (player 2's there); otherwise the call as the game makes it.
void Cut(std::uint64_t cut, std::uint64_t second) noexcept
{
    if (g_inside && g_primaryCut != nullptr && primary_controller::Is(generation::PlayerTwoController()))
    {
        ++g_counts.cutsTwo;
        g_primaryCut(cut, second);
        return;
    }
    ++g_counts.cutsOne;
    reinterpret_cast<CutFn>(g_cutCall.Original())(cut, second);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto system = image.Rebase(kBattleStateGlobal, sizeof(std::uintptr_t));
    // The global's address is proven by IsInBattle's bytes (its RIP-relative read of it), found at startup.
    g_battleSystem    = system && facts::InBattleKnown() ? *system : 0;

    const auto site    = image.Rebase(kCutCall, 5);
    const auto firstPc = image.Rebase(kFirstPcCut);
    const auto primary = platform::FindFunction(image, kPrimaryCut);
    bool       cut     = false;
    if (site && firstPc && primary)
    {
        g_primaryCut = reinterpret_cast<CutFn>(*primary);
        cut          = g_cutCall.Install(*site, *firstPc, reinterpret_cast<std::uintptr_t>(&Cut));
    }
    platform::Log(L"attack cameras: {}; {}",
                  g_battleSystem != 0 ? L"each camera keeps its own smoothing of the game's battle camera sequences"
                                      : L"the sequence list is NOT found (the cameras share its smoothing, as before)",
                  cut ? L"a shot change inside player 2's update cuts player 2's camera (the game's own primary cut)"
                      : L"shot changes cut player 1's camera as before (the cut call is not what the research read)");
}

PlayerTwoUpdate::PlayerTwoUpdate() noexcept
{
    if (g_battleSystem == 0 || g_inside || !generation::OnGameThread())
    {
        return;
    }
    void* list = ListNow();
    if (list == nullptr)
    {
        g_copyList       = nullptr;  // a list made later at the same address is a new one
        g_activeOneCount = 0;
        return;
    }
    if (list != g_copyList)
    {
        g_copyList       = list;
        g_copy           = Read(list);  // seeded from the live state; player 2's own pose replaces it on its first idle frame
        g_activeOneCount = 0;           // player 1's last active entries belonged to the old list
        ++g_counts.lists;
        platform::LogDebug(L"attack cameras: player 2's copy of the sequence list's camera state seeded (list {})", list);
    }
    g_saved           = Read(list);
    g_playerOneCamera = PreviousCamera(g_saved);
    g_flagCount       = 0;
    std::array<const void*, kMaxEntries> activeOne{};
    std::size_t                          activeOneCount = 0;
    ForEachEntry(list, [&](void* entry) {
        const auto active       = ReadField<std::uint8_t>(entry, kEntryActive);
        g_flags[g_flagCount++] = Identity(entry, active);
        if (active != 0)
        {
            activeOne[activeOneCount++] = entry;
        }
        // Player 2's own byte from its last update (the Skill module re-flags every entry first, RE §1.3; this keeps any
        // reader before that on player 2's own history). A new entry starts inactive, as in the game.
        WriteField<std::uint8_t>(entry, kEntryActive, Contains(g_activeTwo, g_activeTwoCount, entry) ? 1 : 0);
    });
    // Player 1's view of the entries, as its own update left them (the stock order updates player 1's camera first).
    for (std::size_t i = 0; i < activeOneCount; ++i)
    {
        if (!Contains(g_activeOne, g_activeOneCount, activeOne[i]))
        {
            ++g_counts.entriesOne;
            LogStarted(activeOne[i], L"player 1", -1.0f);
        }
    }
    g_activeOne      = activeOne;
    g_activeOneCount = activeOneCount;
    Write(list, g_copy);
    list_    = list;
    g_inside = true;
    ++g_counts.updates;
}

PlayerTwoUpdate::~PlayerTwoUpdate()
{
    if (list_ == nullptr)
    {
        return;
    }
    g_inside = false;
    if (ListNow() != list_)
    {
        // The battle system went away during the update (it frees the list with itself): nothing to put back.
        g_copyList = nullptr;
        return;
    }
    g_copy   = Read(list_);
    Write(list_, g_saved);
    std::array<const void*, kMaxEntries> activeTwo{};
    std::size_t                          activeTwoCount = 0;
    ForEachEntry(list_, [&](void* entry) {
        if (ReadField<std::uint8_t>(entry, kEntryActive) != 0 && activeTwoCount < kMaxEntries)
        {
            activeTwo[activeTwoCount++] = entry;
        }
        // Entries still in the list after player 2's update hold "active on either camera": the readers outside the
        // updates (the stop by id, which defers the end of a sequence some camera still plays; the battle system's
        // post-tick) then see a sequence player 2 plays as playing, as solo sees the one camera's (S-ATKCAMVERIFY 2e).
        // One the update removed is not touched (the walk finds only live entries).
        const auto two = ReadField<std::uint8_t>(entry, kEntryActive);
        for (std::size_t i = 0; i < g_flagCount; ++i)
        {
            if (SameEntry(g_flags[i], entry))
            {
                WriteField<std::uint8_t>(entry, kEntryActive, (g_flags[i].active != 0 || two != 0) ? 1 : 0);
                g_counts.onlyTwo += g_flags[i].active == 0 && two != 0 ? 1 : 0;
                break;
            }
        }
    });
    for (std::size_t i = 0; i < activeTwoCount; ++i)
    {
        if (!Contains(g_activeTwo, g_activeTwoCount, activeTwo[i]))
        {
            ++g_counts.entriesTwo;
            const auto two = PreviousCamera(g_copy);
            const float dx = two[0] - g_playerOneCamera[0], dy = two[1] - g_playerOneCamera[1],
                        dz = two[2] - g_playerOneCamera[2];
            LogStarted(activeTwo[i], L"player 2", std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f);
        }
    }
    g_activeTwo      = activeTwo;
    g_activeTwoCount = activeTwoCount;
}

bool InsidePlayerTwoUpdate() noexcept
{
    return g_inside;
}

void Tick() noexcept
{
    const auto now = Clock::now();
    if (!g_status.Due(now))
    {
        return;
    }
    const Counts c = std::exchange(g_counts, {});
    if (c.entriesOne + c.entriesTwo + c.cutsTwo + c.cutsOne + c.lists == 0)
    {
        return;
    }
    platform::LogDebug(L"attack cameras (10 s): player 2's updates with its own sequence state {}; sequences started on "
                       L"player 1's camera {}, on player 2's {}; entry-frames active on player 2's camera only {}; shot cuts on "
                       L"player 2's camera {}, player 1's {}; lists seeded {}",
                       c.updates, c.entriesOne, c.entriesTwo, c.onlyTwo, c.cutsTwo, c.cutsOne, c.lists);
}

void Remove() noexcept
{
    if (!g_cutCall.Restore())
    {
        platform::Log(L"attack cameras: the cut call was changed after us; left in place (it passes through)");
    }
}

}  // namespace lss::player_two::attack_cameras
