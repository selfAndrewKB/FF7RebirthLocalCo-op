#include "player_two/ride_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/move_state.h"
#include "facts/party.h"
#include "facts/vehicles.h"
#include "facts/world.h"
#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/member_movement.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace lss::player_two::ride_watch {

namespace {

using RC::Unreal::UObject;
using Clock = std::chrono::steady_clock;

// AEndPlayerController (SDK offsets, RE-p2-chocobo-ride.md §1.1, §4).
using facts::kControllerActiveVehicle;
constexpr std::uint32_t kVehicleMapCount = 0x5C8;  // VehicleCoordinators (TMap) element count, i32
// A vehicle coordinator: its body and rider; the chocobo's boarding flag; each kind's state machine.
using facts::kCoordinatorBody;
using facts::kCoordinatorRider;
constexpr std::uint32_t kChocoboBoarding    = 0x155;  // u8
constexpr std::uint32_t kChocoboStatePtr    = 0x58;   // -> u32 state id
constexpr std::uint32_t kBuggyStatePtr      = 0x1E8;  // -> u32 state id
// The coordinators' vtables (§4).
using facts::kChocoboCoordinatorVTable;
using facts::kBuggyCoordinatorVTable;
using facts::kBroncoCoordinatorVTable;
using facts::kScooterCoordinatorVTable;

// The chocobo ride manager [[game root]+0x270] (§1.1): state, getoff request, party ride, in battle; member
// entries at +0x40 + 0x2C * player type (state byte +0, weak chocobo +0x20).
using facts::kRootChocoboRideManager;
constexpr std::uint32_t kManagerState    = 0x8;  // u8 (the next byte read the same value live: 0x0707 riding)
using facts::kRideManagerGetOff;
constexpr std::uint32_t kManagerParty    = 0xEB5;
constexpr std::uint32_t kManagerBattle   = 0xEB6;
using facts::kRideManagerEntries;
using facts::kRideManagerEntrySize;
constexpr std::uint32_t kEntryChocobo    = 0x20;
using facts::kRideManagerPlayerTypes;

// AEndChocoboCharacter's native tail (§1.1) and the input component the player-core test reads (§3, lever 1).
// 1.005 moved this tail by +0x20 (RE-1005-offsets-C.md): CreateRideObject 0x143B05BC3/0x143B05C10 (+0x1938, +0x1930),
// 0x140A41434 `cmp byte [rbx+1940h]`, 0x143B06AF9 `cmp byte [rcx+1942h]`, 0x143B13E48 `mov al,[rdi+1944h]`.
constexpr platform::PerBuild<std::uint32_t> kChocoboRideMode   = {0x1910, 0x1930};  // u8
using facts::kChocoboRideObject;
constexpr platform::PerBuild<std::uint32_t> kChocoboFlag1920   = {0x1920, 0x1940};  // u8
constexpr platform::PerBuild<std::uint32_t> kChocoboFlag1922   = {0x1922, 0x1942};  // u8
using facts::kChocoboRidden;
using facts::kCharacterControlHolder;
using facts::kChocoboMoveStateVTable;

// USceneComponent::AttachParent (as player_two/nav_strand.cpp reads it).
using facts::kSceneComponentAttachParent;

using CoreTestFn = std::uint8_t (*)(const void* character);

// The game's read-only lookups (RE-p2-chocobo-ride.md §7.1; entry bytes read in IDA).
using facts::kChocoboForCharacter;
using ChocoboForFn = void* (*)(const void* manager, const void* character);
using facts::kPlayerTypeFromComponent;
using ComponentTypeFn = std::uint8_t (*)(const void* character);
using GlobalTestFn    = std::uint8_t (*)();

// The ride manager entry's grace timer and flags (§7.7: the entry machine skips its own dismount tests in a party
// ride while +0x1C is 0 or the timer runs).
constexpr std::uint32_t kEntryGrace = 0x18;  // float
constexpr std::uint32_t kEntry1C    = 0x1C;  // u8
constexpr std::uint32_t kEntry1D    = 0x1D;  // u8
// A move state's row request bytes: the base ones and the chocobo's (§7.6; which the chocobo's rows use is logged).
constexpr std::uint32_t kRowRequests[] = {0xE8, 0x138, 0x188, 0x298, 0x2E8, 0x338, 0x388};

constexpr std::uint32_t kEvery         = 5;  // ticks between samples
constexpr auto          kSnapshotEvery = std::chrono::seconds(1);
constexpr auto          kSnapshotHeartbeat = std::chrono::seconds(10);

enum class Kind : std::uint8_t
{
    None,
    Chocobo,
    Buggy,
    Bronco,
    Scooter,
    Other,
};

const wchar_t* KindName(Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::None: return L"none";
        case Kind::Chocobo: return L"chocobo";
        case Kind::Buggy: return L"buggy";
        case Kind::Bronco: return L"tiny bronco";
        case Kind::Scooter: return L"scooter";
        case Kind::Other: return L"other";
    }
    return L"?";
}

/// What one sample reads; logged whenever it differs from the last one logged.
struct Sample
{
    // Player 1's controller.
    Kind          kind      = Kind::None;
    std::int32_t  state     = -1;
    const void*   body      = nullptr;
    const void*   rider     = nullptr;
    std::uint8_t  boarding  = 0;
    // Player 2's controller (expected empty).
    const void*   twoVehicle = nullptr;
    std::int32_t  twoMap     = 0;
    // The ride manager and player 2's member's entry.
    std::int32_t  manager   = -1;
    std::uint8_t  getOff    = 0, party = 0, battle = 0;
    std::int32_t  type      = -1;
    std::uint8_t  entry     = 0;
    std::uint8_t  entry1C   = 0, entry1D = 0;
    bool          entryGrace = false;   // the entry's grace timer is running
    std::int32_t  lookup     = -1;      // the game's chocobo lookup for the member: 1 same as the entry's, 0 other, -1 unread
    std::int32_t  inBattle   = -1;
    std::int32_t  chocoboType = -1;     // the chocobo's component player type (16 expected once it has one)
    std::array<std::uint8_t, std::size(kRowRequests)> oneRows{};  // player 1's chocobo state's row requests
    // Player 2's member's chocobo.
    const void*   chocobo    = nullptr;
    std::uint8_t  ridden     = 0, flag1920 = 0, flag1922 = 0, rideMode = 0;
    bool          rideObject = false;
    bool          moveState  = false, moveIsChocobo = false;
    const void*   chocoboController = nullptr, *chocoboRequested = nullptr, *chocoboEffective = nullptr;
    std::int32_t  chocoboAi  = -1;
    const void*   core       = nullptr;
    std::int32_t  coreTest   = -1;  // IsUCPC_Core(chocobo): -1 unread
    std::int32_t  oneCoreTest = -1;  // IsUCPC_Core(player 1's ride body)
    const void*   bodyController = nullptr;  // player 1's ride body's effective controller (PC1 once possessed)
    // Player 2's member.
    const void*   attachedTo = nullptr;
    const void*   memberController = nullptr, *memberEffective = nullptr;

    bool operator==(const Sample&) const = default;
};

/// The sample without the fields that may flicker while riding (they go in the once-a-second snapshot).
Sample Steady(Sample s) noexcept
{
    s.chocoboAi   = -1;
    s.coreTest    = -1;
    s.oneCoreTest = -1;
    s.oneRows     = {};  // the row requests change as prompts come and go: in the snapshot too
    return s;
}

std::uintptr_t g_gameRoot = 0, g_input = 0;
std::uintptr_t g_vt[4]{};  // chocobo, buggy, bronco, scooter coordinators
std::uintptr_t g_chocoboMoveVt = 0;
CoreTestFn     g_isCore = nullptr;
ChocoboForFn   g_chocoboFor = nullptr;       // null: not cross-checked
ComponentTypeFn g_componentType = nullptr;   // null: not read
GlobalTestFn   g_inBattle = nullptr;         // null: not read
bool           g_installed = false;

// Game thread.
Sample            g_last{};
bool              g_logged = false;
std::uint32_t     g_ticks  = 0;
std::int32_t      g_rootOffset = -1;
Clock::time_point g_lastSnapshot{};
float             g_oneAt[3]{};
bool              g_oneAtValid = false;
float             g_twoAt[3]{};
bool              g_twoAtValid = false;
std::array<int, 14> g_lastShown{};
Clock::time_point g_lastShownAt{};
// The start trace: player 2's chocobo for the first seconds after it is ridden (the homing seen at a ride's start).
constexpr auto    kTraceFor   = std::chrono::seconds(8);
constexpr auto    kTraceEvery = std::chrono::milliseconds(250);
Clock::time_point g_traceUntil{};
Clock::time_point g_lastTrace{};
float             g_traceAt[3]{};
float             g_traceGap = -1.0f;  // metres to player 1's ride body at the last trace line

// The vehicle kind player 1 rides and its coordinator's state, for the Info line on a change (game thread).
Kind         g_kind      = Kind::None;
std::int32_t g_state     = -1;
bool         g_kindKnown = false;

/// A report of `level` (Debug or Trace, platform/log.h).
template <class... A>
void LineAt(platform::Level level, std::wformat_string<A...> format, A&&... args) noexcept
{
    if (!platform::Enabled(level))
    {
        return;
    }
    try
    {
        platform::WriteLine(std::format(L"[{}] ride watch: ", platform::LevelName(level)) +
                            std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
}

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    LineAt(platform::Level::Debug, format, std::forward<A>(args)...);
}

Kind KindOf(const void* coordinator) noexcept
{
    if (coordinator == nullptr)
    {
        return Kind::None;
    }
    const std::uintptr_t vt = VTableOf(coordinator);
    return vt == g_vt[0] ? Kind::Chocobo : vt == g_vt[1] ? Kind::Buggy : vt == g_vt[2] ? Kind::Bronco
                                         : vt == g_vt[3] ? Kind::Scooter : Kind::Other;
}

std::int32_t StateOf(const void* coordinator, Kind kind) noexcept
{
    const std::uint32_t field = kind == Kind::Chocobo ? kChocoboStatePtr : kind == Kind::Buggy ? kBuggyStatePtr : 0;
    const void*         state = field != 0 ? ReadPointer(coordinator, field) : nullptr;
    return state != nullptr ? ReadField<std::int32_t>(state, 0) : -1;
}

/// IsUCPC_Core(character). Its last call (component vt+0x70) is the slot member_movement replaces; outside that
/// module's leader scope (this tick runs after it closes) the replacement passes straight to the game's own test,
/// so the answer is the game's and nothing is counted.
std::int32_t CoreTest(const void* character) noexcept
{
    std::uint8_t answer = 0;
    return character != nullptr && platform::CallGuarded(g_isCore, answer, character) ? answer : -1;
}

/// The AI state type of a character's retained AI controller (+0x588 -> brain +0x518 -> state +0x20 -> +0x18).
std::int32_t AiStateOf(const void* character) noexcept
{
    const void* ai    = ReadPointer(character, facts::kCharacterRetainedAi);
    const void* brain = ReadPointer(ai, facts::kAiActiveBrain);
    const void* state = ReadPointer(brain, facts::kBrainState);
    return state != nullptr ? ReadField<std::uint8_t>(state, facts::kAiStateType) : -1;
}

const void* RootOf(void* actor) noexcept
{
    if (actor == nullptr)
    {
        return nullptr;
    }
    // The one-time reflection lookup dereferences the actor unguarded: only on an actor the game still names.
    if (g_rootOffset < 0 && leader_scope::StillNames(leader_scope::WeakOf(actor), actor))
    {
        try
        {
            auto* property = static_cast<UObject*>(actor)->GetPropertyByNameInChain(STR("RootComponent"));
            if (property == nullptr || property->GetSize() != sizeof(void*))
            {
                return nullptr;
            }
            g_rootOffset = property->GetOffset_Internal();
        }
        catch (...)
        {
            return nullptr;
        }
    }
    return g_rootOffset < 0 ? nullptr : ReadPointer(actor, static_cast<std::uint32_t>(g_rootOffset));
}

Sample Read(void* playerOne, void* playerTwo, void* member) noexcept
{
    Sample s;
    const void* coord = ReadPointer(playerOne, kControllerActiveVehicle);
    s.kind            = KindOf(coord);
    if (coord != nullptr)
    {
        s.state    = StateOf(coord, s.kind);
        s.body     = ReadPointer(coord, kCoordinatorBody);
        s.rider    = ReadPointer(coord, kCoordinatorRider);
        s.boarding = s.kind == Kind::Chocobo ? ReadField<std::uint8_t>(coord, kChocoboBoarding) : 0;
        s.oneCoreTest = s.kind == Kind::Chocobo ? CoreTest(s.body) : -1;
        s.bodyController = ReadPointer(s.body, facts::kCharacterEffectiveController);
    }
    s.twoVehicle = ReadPointer(playerTwo, kControllerActiveVehicle);
    s.twoMap     = playerTwo != nullptr ? ReadField<std::int32_t>(playerTwo, kVehicleMapCount) : 0;

    const void* root    = ReadPointer(reinterpret_cast<const void*>(g_gameRoot), 0);
    const auto* manager = static_cast<const std::byte*>(ReadPointer(root, kRootChocoboRideManager));
    if (manager != nullptr)
    {
        s.manager = ReadField<std::uint8_t>(manager, kManagerState);
        s.getOff  = ReadField<std::uint8_t>(manager, kRideManagerGetOff);
        s.party   = ReadField<std::uint8_t>(manager, kManagerParty);
        s.battle  = ReadField<std::uint8_t>(manager, kManagerBattle);
        const auto type = member != nullptr && party_member::Installed() ? party_member::PlayerTypeOf(member) : std::nullopt;
        if (type && *type < kRideManagerPlayerTypes)
        {
            const std::byte* entry = manager + kRideManagerEntries + kRideManagerEntrySize * *type;
            s.type       = *type;
            s.entry      = ReadField<std::uint8_t>(entry, 0);
            s.entry1C    = ReadField<std::uint8_t>(entry, kEntry1C);
            s.entry1D    = ReadField<std::uint8_t>(entry, kEntry1D);
            s.entryGrace = ReadField<float>(entry, kEntryGrace) > 0.0f;
            s.chocobo    = leader_scope::ObjectAt(entry + kEntryChocobo);
            void* found  = nullptr;
            if (g_chocoboFor != nullptr && platform::CallGuarded(g_chocoboFor, found, static_cast<const void*>(manager),
                                                                 static_cast<const void*>(member)))
            {
                s.lookup = found == s.chocobo ? 1 : 0;
            }
        }
    }
    std::uint8_t battle = 0;
    s.inBattle = g_inBattle != nullptr && platform::CallGuarded(g_inBattle, battle) ? battle : -1;
    if (s.kind == Kind::Chocobo)
    {
        const void* state = ReadPointer(s.body, facts::kCharacterMoveState);
        for (std::size_t i = 0; state != nullptr && i < std::size(kRowRequests); ++i)
        {
            s.oneRows[i] = ReadField<std::uint8_t>(state, kRowRequests[i]);
        }
    }
    if (s.chocobo != nullptr)
    {
        const void* c       = s.chocobo;
        s.ridden            = ReadField<std::uint8_t>(c, kChocoboRidden);
        s.flag1920          = ReadField<std::uint8_t>(c, kChocoboFlag1920);
        s.flag1922          = ReadField<std::uint8_t>(c, kChocoboFlag1922);
        s.rideMode          = ReadField<std::uint8_t>(c, kChocoboRideMode);
        s.rideObject        = ReadPointer(c, kChocoboRideObject) != nullptr;
        const void* move    = ReadPointer(c, facts::kCharacterMoveState);
        s.moveState         = move != nullptr;
        s.moveIsChocobo     = move != nullptr && VTableOf(move) == g_chocoboMoveVt;
        s.chocoboController = ReadPointer(c, facts::kPawnController);
        s.chocoboRequested  = ReadPointer(c, facts::kCharacterRequestedController);
        s.chocoboEffective  = ReadPointer(c, facts::kCharacterEffectiveController);
        s.chocoboAi         = AiStateOf(c);
        s.core              = ReadPointer(c, kCharacterControlHolder);
        s.coreTest          = CoreTest(c);
        std::uint8_t ctype  = 0;
        s.chocoboType = g_componentType != nullptr && platform::CallGuarded(g_componentType, ctype, c) ? ctype : -1;
    }
    if (member != nullptr)
    {
        s.attachedTo       = ReadPointer(RootOf(member), kSceneComponentAttachParent);
        s.memberController = ReadPointer(member, facts::kPawnController);
        s.memberEffective  = ReadPointer(member, facts::kCharacterEffectiveController);
    }
    return s;
}

/// The row request bytes as "E8:1 138:0 ...", nonzero ones only ("none" if all are zero).
std::wstring RowsText(const std::array<std::uint8_t, std::size(kRowRequests)>& rows)
{
    std::wstring text;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i] != 0)
        {
            text += std::format(L"{}{:X}:{}", text.empty() ? L"" : L" ", kRowRequests[i], rows[i]);
        }
    }
    return text.empty() ? std::wstring(L"none") : text;
}

void LogSample(const Sample& s)
{
    Line(L"player 1's vehicle {} (state {}, body {} controlled by {}, rider {}, boarding {}, body's player-core test {}); "
         L"player 2's {} (coordinators {}); ride manager state {} (getoff {}, party {}, battle {}); in battle {}; player 2's "
         L"member type {} entry state {} (+0x1C {}, +0x1D {}, grace {}; the game's lookup {}); player 1's ride rows "
         L"{} (then in the riding line)",
         KindName(s.kind), s.state, s.body, Who(s.bodyController), s.rider, s.boarding,
         s.oneCoreTest, s.twoVehicle, s.twoMap, s.manager, s.getOff, s.party, s.battle, s.inBattle, s.type, s.entry,
         s.entry1C, s.entry1D, s.entryGrace ? 1 : 0,
         s.lookup == 1 ? L"agrees" : s.lookup == 0 ? L"DIFFERS" : L"unread", RowsText(s.oneRows));
    if (s.chocobo != nullptr)
    {
        Line(L"player 2's chocobo {}: ridden {} (0x1920 {}, 0x1922 {}, mode {}, ride object {}); move state {}{}; "
             L"controller {}, requested {}, effective {}; AI state {}; input component {} (player-core test {}, type {})",
             s.chocobo, s.ridden, s.flag1920, s.flag1922, s.rideMode, s.rideObject ? 1 : 0, s.moveState ? L"yes" : L"none",
             s.moveIsChocobo ? L" (the chocobo's)" : L"", Who(s.chocoboController),
             Who(s.chocoboRequested), Who(s.chocoboEffective), s.chocoboAi,
             s.core, s.coreTest, s.chocoboType);
    }
    else
    {
        Line(L"player 2's member has no chocobo in the ride manager");
    }
    Line(L"player 2's member: attached to {}, controller {}, effective {}", s.attachedTo,
         Who(s.memberController), Who(s.memberEffective));
}

/// While a ride is on: the merged sticks and player 1's ride body's speed, once a second (test 3).
void LogSnapshot(const Sample& s, const void* member, const void* playerTwo)
{
    if (!platform::Enabled(platform::Level::Trace))
    {
        // The riding line follows the sticks: a per-input report (Trace). Nothing is read or dry-run below it.
        g_oneAtValid = false;
        g_twoAtValid = false;
        return;
    }
    const auto now = Clock::now();
    if (now - g_lastSnapshot < kSnapshotEvery)
    {
        return;
    }
    const float seconds = std::chrono::duration<float>(now - g_lastSnapshot).count();
    g_lastSnapshot      = now;
    float sticks[4]{};
    const void* input = ReadPointer(reinterpret_cast<const void*>(g_input), 0);
    if (input != nullptr)
    {
        platform::CopyGuarded(sticks, static_cast<const std::byte*>(input) + facts::kInputMergedSticks, sizeof(sticks));
    }
    float at[3]{};
    const bool  moved = s.body != nullptr && charapop::ActorLocation(const_cast<void*>(s.body), at);
    const float speed = moved && g_oneAtValid && seconds > 0.0f ? charapop::Distance(at, g_oneAt) / 100.0f / seconds : -1.0f;
    g_oneAtValid      = moved;
    for (int i = 0; i < 3; ++i)
    {
        g_oneAt[i] = at[i];
    }
    // Player 2's chocobo the same way (the gait reached: walk, jog or sprint), and how far it is from player 1's.
    float twoAt[3]{};
    const bool  twoMoved = s.chocobo != nullptr && charapop::ActorLocation(const_cast<void*>(s.chocobo), twoAt);
    const float twoSpeed =
        twoMoved && g_twoAtValid && seconds > 0.0f ? charapop::Distance(twoAt, g_twoAt) / 100.0f / seconds : -1.0f;
    const float gap = twoMoved && moved ? charapop::Distance(twoAt, at) / 100.0f : -1.0f;
    g_twoAtValid    = twoMoved;
    for (int i = 0; i < 3; ++i)
    {
        g_twoAt[i] = twoAt[i];
    }
    // The dry run of the chocobo swap's gate (RE-p2-chocobo-ride.md §7.2 item 2): the game's player-core test of the
    // member as the swap would see it (1 and PC2 expected), and of her chocobo (0 until the mod answers for it).
    const auto dryMember  = member_movement::PlayerCoreAsPlayerTwo(member, member);
    const auto dryChocobo = member_movement::PlayerCoreAsPlayerTwo(s.chocobo, member);
    const std::int32_t memberAi = AiStateOf(member);
    // Logged when what it shows changes (sticks to a quarter, speed to 1 m/s, the answers), or every 10 s.
    const auto quarter = [](float v) { return static_cast<int>(v * 4.0f + (v < 0.0f ? -0.5f : 0.5f)); };
    int rowBits = 0;
    for (std::size_t i = 0; i < s.oneRows.size(); ++i)
    {
        rowBits |= s.oneRows[i] != 0 ? 1 << i : 0;
    }
    const std::array<int, 14> shown{quarter(sticks[0]), quarter(sticks[1]), quarter(sticks[2]), quarter(sticks[3]),
                                    speed < 0.0f ? -1 : static_cast<int>(speed + 0.5f), s.oneCoreTest * 1000 + s.coreTest,
                                    s.chocoboAi, s.ridden, memberAi, dryMember.answer * 10 + dryChocobo.answer,
                                    dryMember.primary == playerTwo ? 1 : 0, rowBits,
                                    twoSpeed < 0.0f ? -1 : static_cast<int>(twoSpeed + 0.5f),
                                    gap < 0.0f ? -1 : static_cast<int>(gap / 10.0f)};
    if (shown == g_lastShown && now - g_lastShownAt < kSnapshotHeartbeat)
    {
        return;
    }
    g_lastShown   = shown;
    g_lastShownAt = now;
    LineAt(platform::Level::Trace, L"riding ({} state {}): merged sticks L ({:.2f}, {:.2f}) R ({:.2f}, {:.2f}); player 1's ride body {:.1f} m/s "
         L"(player-core test {}); player 2's chocobo {:.1f} m/s, {:.0f} m from player 1's, ridden {}, AI state {}, "
         L"player-core test {}; player 2's member's AI state {}; dry run as player 2 (primary {}): member {}, chocobo {}; "
         L"player 1's ride rows {}",
         KindName(s.kind), s.state, sticks[0], sticks[1], sticks[2], sticks[3], speed, s.oneCoreTest, twoSpeed, gap,
         s.ridden, s.chocoboAi, s.coreTest, memberAi,
         dryMember.primary == playerTwo ? L"PC2" : dryMember.primary ? L"other" : L"none", dryMember.answer,
         dryChocobo.answer, RowsText(s.oneRows));
}

/// For the first seconds after player 2's chocobo is ridden, four lines a second: its speed, its distance to player 1's
/// ride body and how fast that distance closes (the homing at a ride's start), who controls it and its AI state.
void LogStartTrace(const Sample& s)
{
    const auto now = Clock::now();
    if (s.ridden != 0 && g_last.ridden == 0)
    {
        g_traceUntil = now + kTraceFor;
        g_lastTrace  = {};
        g_traceGap   = -1.0f;
        Line(L"start trace: player 2's chocobo ridden; tracing it for {} s", kTraceFor.count());
    }
    if (now >= g_traceUntil || now - g_lastTrace < kTraceEvery || s.chocobo == nullptr)
    {
        return;
    }
    const float seconds = g_lastTrace == Clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - g_lastTrace).count();
    g_lastTrace         = now;
    float two[3]{}, one[3]{};
    const bool twoRead = charapop::ActorLocation(const_cast<void*>(s.chocobo), two);
    const bool oneRead = s.body != nullptr && charapop::ActorLocation(const_cast<void*>(s.body), one);
    const float gap    = twoRead && oneRead ? charapop::Distance(two, one) / 100.0f : -1.0f;
    const float speed  = twoRead && seconds > 0.0f ? charapop::Distance(two, g_traceAt) / 100.0f / seconds : -1.0f;
    const float closing = gap >= 0.0f && g_traceGap >= 0.0f && seconds > 0.0f ? (g_traceGap - gap) / seconds : 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        g_traceAt[i] = two[i];
    }
    g_traceGap = gap;
    Line(L"start trace: player 2's chocobo {:.1f} m/s, {:.1f} m from player 1's ride body (closing {:+.1f} m/s); "
         L"controller {}, AI state {}, ridden {}; player 2's coordinator {}",
         speed, gap, closing, Who(s.chocoboEffective), s.chocoboAi, s.ridden, s.twoVehicle);
}

void Run()
{
    void* playerOne = generation::PlayerOneController();
    // Mounting and dismounting are rare, real changes: Info, read cheaply whatever the level.
    // The state too (the buggy's 3 boarding, 4 riding, 5 getting off; the single-view signal, plan "Buggy: single view").
    const void*        coordinator = ReadPointer(playerOne, kControllerActiveVehicle);
    const Kind         kind        = KindOf(coordinator);
    const std::int32_t state       = kind == Kind::None ? -1 : StateOf(coordinator, kind);
    if (g_kindKnown && (kind != g_kind || state != g_state))
    {
        platform::Log(L"ride watch: player 1's vehicle: {} (state {}) -> {} (state {})", KindName(g_kind), g_state,
                      KindName(kind), state);
    }
    g_kind      = kind;
    g_state     = state;
    g_kindKnown = true;
    if (!platform::Enabled(platform::Level::Debug))
    {
        // The rest is Debug (and Trace): nothing is sampled below that level.
        g_logged     = false;
        g_oneAtValid = false;
        g_twoAtValid = false;
        return;
    }
    void* playerTwo = generation::PlayerTwoController();
    void* member    = generation::Member();
    const Sample s  = Read(playerOne, playerTwo, member);
    LogStartTrace(s);  // before g_last moves on: it starts on the ridden edge
    if (!g_logged || !(Steady(s) == Steady(g_last)))
    {
        LogSample(s);
        g_last   = s;
        g_logged = true;
    }
    if (s.kind != Kind::None || s.ridden != 0)
    {
        LogSnapshot(s, member, playerTwo);
    }
    else
    {
        g_oneAtValid = false;
        g_twoAtValid = false;
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root  = image.Rebase(facts::kEndGameRoot, sizeof(void*));
    const auto input = image.Rebase(facts::kSquareInputGlobal, sizeof(void*));
    const auto test  = platform::FindFunction(image, facts::kIsPlayerCore);
    const std::uintptr_t vts[] = {kChocoboCoordinatorVTable, kBuggyCoordinatorVTable, kBroncoCoordinatorVTable,
                                  kScooterCoordinatorVTable};
    bool vtsFound = true;
    for (int i = 0; i < 4; ++i)
    {
        const auto vt = image.Rebase(vts[i]);
        g_vt[i]       = vt ? *vt : 0;
        vtsFound      = vtsFound && vt.has_value();
    }
    const auto move  = image.Rebase(kChocoboMoveStateVTable);
    g_gameRoot       = root ? *root : 0;
    g_input          = input ? *input : 0;
    g_chocoboMoveVt  = move ? *move : 0;
    g_isCore         = test ? reinterpret_cast<CoreTestFn>(*test) : nullptr;
    // Optional reads: each logged as unread when its bytes differ.
    const auto chocoboFor = platform::FindFunction(image, kChocoboForCharacter);
    const auto ctype      = platform::FindFunction(image, kPlayerTypeFromComponent);
    const auto battle     = platform::FindFunction(image, facts::kIsInBattle);
    g_chocoboFor          = chocoboFor ? reinterpret_cast<ChocoboForFn>(*chocoboFor) : nullptr;
    g_componentType       = ctype ? reinterpret_cast<ComponentTypeFn>(*ctype) : nullptr;
    g_inBattle            = battle ? reinterpret_cast<GlobalTestFn>(*battle) : nullptr;
    g_installed      = g_gameRoot != 0 && g_input != 0 && g_isCore != nullptr && vtsFound && g_chocoboMoveVt != 0 &&
                  leader_scope::Installed();
    platform::Log(L"ride watch: {}", g_installed ? L"installed (chocobo and buggy rides logged while player 2 is in; read-only)"
                                                 : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Tick() noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    if (!generation::Active())
    {
        g_kindKnown  = false;
        g_logged     = false;
        g_oneAtValid = false;
        g_twoAtValid = false;
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

}  // namespace lss::player_two::ride_watch
