#include "player_two/wheelie_watch.h"

#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/describe.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/pad_sticks.h"
#include "player_two/party_member.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lss::player_two::wheelie_watch {

namespace {

// AEndPlayerController (SDK; RE-p2-control-mode-object.md): the active coordinator and the TMap<uint8, coordinator>
// of every coordinator it owns (TSetElement<TPair<uint8, T*>>: key +0, value +8, then the hash links: 0x18 bytes).
constexpr std::uint32_t kActiveVehicle   = 0x5B8;
constexpr std::uint32_t kVehicleMap      = 0x5C0;
constexpr std::uint32_t kMapElementSize  = 0x18;
constexpr std::uint32_t kMapElementValue = 0x8;

// UEndPlayerSBScooterCoordinator (RE-p2-wheelie-ride.md §1, §2, §4).
constexpr std::uintptr_t kScooterCoordVt   = 0x145D76DF0;
constexpr std::uint32_t  kCoordScooter     = 0x30;
constexpr std::uint32_t  kCoordRider       = 0x38;
constexpr std::uint32_t  kCoordShared      = 0x48;   // TSharedPtr object
constexpr std::uint32_t  kCoordRiderTable  = 0x58;   // TSharedPtr object
constexpr std::uint32_t  kCoordGetOffFast  = 0x182;  // u8: the getoff is instant
constexpr std::uint32_t  kCoordScripted    = 0x1F8;  // u8
constexpr std::uint32_t  kCoordMiniGame    = 0x1F9;  // u8
constexpr std::uint32_t  kCoordUiShown     = 0x1FA;  // u8
constexpr std::uint32_t  kCoordInstant     = 0x1FC;  // u8
constexpr std::uint32_t  kCoordEnabled     = 0x29;   // u8 (vt+0x300 sets it; its key listener is registered)
constexpr std::uint32_t  kCoordDisabled    = 0x2A;   // u8 (vt+0x308)
constexpr std::uint32_t  kCoordStatePtr    = 0x230;  // -> u32 state id
// The shared block: the nearest free scooter (weak), the scooter list (TArray<TWeakObjectPtr>), the ride-on point
// (dormant), and the ride requested / idle / parking minigame on bytes and the minigame colour mask (§11, §13.1).
constexpr std::uint32_t kSharedListData  = 0x8;
constexpr std::uint32_t kSharedListNum   = 0x10;
constexpr std::uint32_t kSharedRequested = 0x28;
constexpr std::uint32_t kSharedIdle      = 0x29;
constexpr std::uint32_t kSharedParking   = 0x2A;
constexpr std::uint32_t kSharedColours   = 0x2B;
constexpr std::int32_t  kListCap         = 32;
// The per-rider table: 15 entries of 0x14 bytes (FName +0), its count and state.
constexpr std::uint32_t kTableNum        = 0x12C;
constexpr std::uint32_t kTableState      = 0x134;
constexpr std::uint32_t kTableEntrySize  = 0x14;
constexpr std::int32_t  kTableEntries    = 15;

// AEndSelfBalancingScooterCharacter (SDK and §1.1): rider, ridden, parked, excluded; its control holder's accel and
// brake as the move state's slot 11 writes them (§3.3).
constexpr std::uintptr_t kScooterVt        = 0x14583F980;
// 1.005 moved these Square Enix character fields by +0x20 (RE-1005-player-two.md).
constexpr platform::PerBuild<std::uint32_t> kScooterRider    = {0x2210, 0x2230};
constexpr platform::PerBuild<std::uint32_t> kScooterRidden   = {0x2318, 0x2338};
constexpr platform::PerBuild<std::uint32_t> kScooterParked   = {0x2319, 0x2339};
constexpr platform::PerBuild<std::uint32_t> kScooterExcluded = {0x231A, 0x233A};
constexpr std::uint32_t  kControlHolder    = 0x638;
constexpr std::uint32_t  kHolderAccel      = 0x2C0;
constexpr std::uint32_t  kHolderBrake      = 0x2C4;

// Square's input object: the merged trigger values the scooter's AccelAxis and CatchAxis read (§3.3).
constexpr std::uint32_t kInputCatchAxis  = 0x1240;
constexpr std::uint32_t kInputAccelAxis  = 0x1244;
constexpr std::uint32_t kInputCatchOther = 0x11B0;
constexpr std::uint32_t kInputAccelOther = 0x11B4;

// The on/off counters (§4): [[game root]+0x170] + base + 4 * id, two arrays.
constexpr std::uint32_t                kRootOnOff      = 0x170;
constexpr std::uint32_t                kOnOffBase      = 0x1A1238C;
constexpr std::uint32_t                kOnOffAltBase   = 0x1A1263C;
constexpr std::array<std::uint32_t, 3> kOnOffIds       = {152, 136, 4};

// FWeakObjectPtr::Get (hud/hud_updates.cpp).
constexpr std::uint8_t           kWeakGetBytes[] = {0x44, 0x8B, 0x49, 0x04, 0x45, 0x33, 0xC0, 0x45, 0x85, 0xC9, 0x74, 0x4F};
constexpr platform::FunctionFact kWeakGet{L"FWeakObjectPtr::Get", 0x141905998, kWeakGetBytes};
using WeakGetFn = void* (*)(const void* weak);

/// Ticks between reads, and reads between the riding lines (about one a second).
constexpr std::uint32_t kEvery       = 10;
constexpr std::uint32_t kRidingEvery = 6;

// Set once at install.
std::uintptr_t g_coordVt   = 0;
std::uintptr_t g_scooterVt = 0;
std::uintptr_t g_gameRoot  = 0;  // the global that holds the game root
std::uintptr_t g_input     = 0;  // the global that holds Square's input object
WeakGetFn      g_weakGet   = nullptr;

// Game thread.
std::uint32_t g_ticks       = 0;
std::uint32_t g_ridingReads = 0;

struct Snapshot
{
    bool         found       = false;  // player 1 has a key-4 coordinator
    bool         active      = false;  // and it is player 1's active one
    std::int32_t state       = -1;
    const void*  scooter     = nullptr;
    const void*  rider       = nullptr;
    std::uint8_t flags[7]    = {};     // scripted, minigame, UI shown, instant, getoff instant, enabled, disabled
    std::int32_t listNum     = -1;
    std::uint8_t shared[4]   = {};     // requested, idle, parking minigame on, colours
    std::int32_t tableState  = -1;
    std::int32_t tableNum    = -1;
    std::int32_t tableNamed  = 0;
    std::int32_t free        = 0;
    std::int32_t ridden      = 0;
    std::int32_t excluded    = 0;
    const void*  twoCoord    = nullptr;  // player 2's key-4 coordinator
    const void*  member      = nullptr;
    std::int32_t memberType  = -1;
    std::int32_t onOff[6]    = {};

    bool operator==(const Snapshot&) const = default;
};

bool     g_logged = false;
Snapshot g_last;

void* WeakGet(const void* weak) noexcept
{
    void* out = nullptr;
    return g_weakGet != nullptr && platform::CallGuarded(g_weakGet, out, weak) ? out : nullptr;
}

/// The key-4 (SB Scooter) coordinator among a controller's own coordinators, by vtable.
const void* ScooterCoordinatorOf(const void* controller)
{
    const void* found = nullptr;
    if (controller == nullptr)
    {
        return found;
    }
    platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(controller) + kVehicleMap, kMapElementSize,
                                   [&found](std::uintptr_t element) {
                                       const void* coordinator = ReadPointer(reinterpret_cast<const void*>(element),
                                                                             kMapElementValue);
                                       if (coordinator != nullptr && VTableOf(coordinator) == g_coordVt)
                                       {
                                           found = coordinator;
                                           return false;
                                       }
                                       return true;
                                   });
    return found;
}

float DistanceMetres(void* a, void* b) noexcept
{
    float at[3]{}, to[3]{};
    return a != nullptr && b != nullptr && charapop::ActorLocation(a, at) && charapop::ActorLocation(b, to)
               ? charapop::Distance(at, to) / 100.0f
               : -1.0f;
}

Snapshot Read(float& nearestFree)
{
    Snapshot s;
    nearestFree           = -1.0f;
    void*       playerOne = generation::PlayerOneController();
    const void* coord     = ScooterCoordinatorOf(playerOne);
    s.twoCoord            = ScooterCoordinatorOf(generation::PlayerTwoController());
    s.member              = generation::Member();
    if (const auto type = s.member != nullptr ? party_member::PlayerTypeOf(s.member) : std::nullopt)
    {
        s.memberType = *type;
    }
    const void* root  = ReadPointer(reinterpret_cast<const void*>(g_gameRoot), 0);
    const void* onOff = ReadPointer(root, kRootOnOff);
    for (std::size_t i = 0; onOff != nullptr && i < kOnOffIds.size(); ++i)
    {
        s.onOff[i]     = ReadField<std::int32_t>(onOff, kOnOffBase + 4 * kOnOffIds[i]);
        s.onOff[i + 3] = ReadField<std::int32_t>(onOff, kOnOffAltBase + 4 * kOnOffIds[i]);
    }
    if (coord == nullptr)
    {
        return s;
    }
    s.found              = true;
    s.active             = ReadPointer(playerOne, kActiveVehicle) == coord;
    const void* statePtr = ReadPointer(coord, kCoordStatePtr);
    s.state              = statePtr != nullptr ? ReadField<std::int32_t>(statePtr, 0) : -1;
    s.scooter            = ReadPointer(coord, kCoordScooter);
    s.rider              = ReadPointer(coord, kCoordRider);
    s.flags[0]           = ReadField<std::uint8_t>(coord, kCoordScripted);
    s.flags[1]           = ReadField<std::uint8_t>(coord, kCoordMiniGame);
    s.flags[2]           = ReadField<std::uint8_t>(coord, kCoordUiShown);
    s.flags[3]           = ReadField<std::uint8_t>(coord, kCoordInstant);
    s.flags[4]           = ReadField<std::uint8_t>(coord, kCoordGetOffFast);
    s.flags[5]           = ReadField<std::uint8_t>(coord, kCoordEnabled);
    s.flags[6]           = ReadField<std::uint8_t>(coord, kCoordDisabled);
    const void* shared   = ReadPointer(coord, kCoordShared);
    if (shared != nullptr)
    {
        s.listNum   = ReadField<std::int32_t>(shared, kSharedListNum);
        s.shared[0] = ReadField<std::uint8_t>(shared, kSharedRequested);
        s.shared[1] = ReadField<std::uint8_t>(shared, kSharedIdle);
        s.shared[2] = ReadField<std::uint8_t>(shared, kSharedParking);
        s.shared[3] = ReadField<std::uint8_t>(shared, kSharedColours);
        const auto* list = static_cast<const std::byte*>(ReadPointer(shared, kSharedListData));
        for (std::int32_t i = 0; list != nullptr && i < s.listNum && i < kListCap; ++i)
        {
            void* scooter = WeakGet(list + static_cast<std::ptrdiff_t>(i) * sizeof(std::uint64_t));
            if (scooter == nullptr || VTableOf(scooter) != g_scooterVt)
            {
                continue;
            }
            const bool ridden   = ReadField<std::uint8_t>(scooter, kScooterRidden) != 0;
            const bool excluded = ReadField<std::uint8_t>(scooter, kScooterExcluded) != 0;
            s.ridden += ridden ? 1 : 0;
            s.excluded += excluded ? 1 : 0;
            if (ridden || excluded)
            {
                continue;
            }
            ++s.free;
            const float metres = DistanceMetres(const_cast<void*>(s.member), scooter);
            if (metres >= 0.0f && (nearestFree < 0.0f || metres < nearestFree))
            {
                nearestFree = metres;
            }
        }
    }
    const void* table = ReadPointer(coord, kCoordRiderTable);
    if (table != nullptr)
    {
        s.tableState = ReadField<std::uint8_t>(table, kTableState);
        s.tableNum   = ReadField<std::int32_t>(table, kTableNum);
        for (std::int32_t i = 0; i < kTableEntries; ++i)
        {
            s.tableNamed += ReadField<std::uint64_t>(table, static_cast<std::uint32_t>(i) * kTableEntrySize) != 0 ? 1 : 0;
        }
    }
    return s;
}

const wchar_t* StateName(std::int32_t state) noexcept
{
    switch (state)
    {
        case 0: return L"idle";
        case 1: return L"resume";
        case 2: return L"boarding";
        case 3: return L"riding";
        case 4: return L"getting off";
        default: return L"unread";
    }
}

/// Cloud, Tifa, Aerith and Yuffie have `AnimSet_Wheelie00` (research/RE-party-anim-sets.md).
const wchar_t* AnimSetOf(std::int32_t type) noexcept
{
    return type < 0 ? L"no member" : type == 0 || type == 2 || type == 3 || type == 5 ? L"has the Wheelie anim set"
                                                                                      : L"has no Wheelie anim set";
}

void LogSnapshot(const Snapshot& s, float nearestFree)
{
    if (!s.found)
    {
        platform::Log(L"wheelie watch: player 1 has no scooter coordinator (no parked Wheelie registered here); player 2's {}; "
                      L"on/off 152/136/4 {}/{}/{} (alt {}/{}/{})",
                      s.twoCoord != nullptr ? L"exists" : L"none", s.onOff[0], s.onOff[1], s.onOff[2], s.onOff[3],
                      s.onOff[4], s.onOff[5]);
        return;
    }
    platform::Log(L"wheelie watch: player 1's coordinator {} state {} ({}), scooter {} rider {}; scripted {} minigame {} UI {} "
                  L"instant {} getoff-instant {} enabled {} disabled {}; shared: {} scooters, requested {} idle {} "
                  L"parking {} colours {:#04x}; "
                  L"scooters free {}, ridden {}, excluded {}; nearest free to player 2's member {}; rider table state {} "
                  L"num {} named {}; player 2's coordinator {}; player 2's member {} (type {}, {}); on/off 152/136/4 "
                  L"{}/{}/{} (alt {}/{}/{})",
                  s.active ? L"ACTIVE" : L"idle", s.state, StateName(s.state), NameOf(s.scooter), NameOf(s.rider),
                  s.flags[0], s.flags[1], s.flags[2], s.flags[3], s.flags[4], s.flags[5], s.flags[6], s.listNum, s.shared[0], s.shared[1],
                  s.shared[2], s.shared[3], s.free, s.ridden, s.excluded,
                  nearestFree >= 0.0f ? std::format(L"{:.1f} m", nearestFree) : std::wstring{L"(none or unread)"},
                  s.tableState, s.tableNum, s.tableNamed, s.twoCoord != nullptr ? NameOf(s.twoCoord) : std::wstring{L"none"},
                  NameOf(s.member), s.memberType, AnimSetOf(s.memberType), s.onOff[0], s.onOff[1], s.onOff[2], s.onOff[3],
                  s.onOff[4], s.onOff[5]);
}

/// While player 1 rides: the triggers the scooter reads, each pad's own, and what player 1's scooter made of them.
void LogRiding(const Snapshot& s)
{
    if (!platform::Enabled(platform::Level::Debug) || ++g_ridingReads % kRidingEvery != 0)
    {
        return;
    }
    const void* input = ReadPointer(reinterpret_cast<const void*>(g_input), 0);
    float       merged[4]{};
    if (input != nullptr)
    {
        merged[0] = ReadField<float>(input, kInputCatchAxis);
        merged[1] = ReadField<float>(input, kInputAccelAxis);
        merged[2] = ReadField<float>(input, kInputCatchOther);
        merged[3] = ReadField<float>(input, kInputAccelOther);
    }
    float      padOne[2]{}, padTwo[2]{};
    const bool one    = pad_sticks::PadTriggers(0, padOne[0], padOne[1]);
    const bool two    = pad_sticks::PadTriggers(1, padTwo[0], padTwo[1]);
    const auto pair   = [](bool read, const float (&value)[2]) {
        return read ? std::format(L"LT {:.2f} RT {:.2f}", value[0], value[1]) : std::wstring{L"unread"};
    };
    const void* holder = ReadPointer(s.scooter, kControlHolder);
    platform::LogDebug(L"wheelie watch: riding: merged triggers catch {:.2f} accel {:.2f} (other {:.2f}/{:.2f}); pad 1 {}, "
                       L"pad 2 {}; player 1's scooter accel {:.2f} brake {:.2f}",
                       merged[0], merged[1], merged[2], merged[3], pair(one, padOne), pair(two, padTwo),
                       holder != nullptr ? ReadField<float>(holder, kHolderAccel) : -1.0f,
                       holder != nullptr ? ReadField<float>(holder, kHolderBrake) : -1.0f);
}

void Run()
{
    float          nearestFree = -1.0f;
    const Snapshot s           = Read(nearestFree);
    if (!g_logged || !(s == g_last))
    {
        LogSnapshot(s, nearestFree);
        g_last   = s;
        g_logged = true;
    }
    if (s.active && s.state == 3)
    {
        LogRiding(s);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto coord   = image.Rebase(kScooterCoordVt);
    const auto scooter = image.Rebase(kScooterVt);
    const auto root    = image.Rebase(charapop::kGameRoot, sizeof(void*));
    const auto input   = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    const auto weakGet = platform::FindFunction(image, kWeakGet);
    g_coordVt          = coord ? *coord : 0;
    g_scooterVt        = scooter ? *scooter : 0;
    g_gameRoot         = root ? *root : 0;
    g_input            = input ? *input : 0;
    g_weakGet          = weakGet ? reinterpret_cast<WeakGetFn>(*weakGet) : nullptr;
    const bool ok      = g_coordVt != 0 && g_scooterVt != 0 && g_gameRoot != 0 && g_input != 0 && g_weakGet != nullptr;
    platform::Log(L"wheelie watch: {}", ok ? L"installed (the Wheelie system logged while player 2 is in; read-only)"
                                           : L"UNAVAILABLE (a check failed)");
    if (!ok)
    {
        g_coordVt = 0;
    }
    return ok;
}

void Tick() noexcept
{
    if (g_coordVt == 0 || !generation::OnGameThread())
    {
        return;
    }
    if (!generation::Active())
    {
        g_logged      = false;
        g_ridingReads = 0;
        return;
    }
    if (++g_ticks % kEvery != 0)
    {
        return;
    }
    try
    {
        Run();
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::wheelie_watch
