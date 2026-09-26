#include "player_two/lock_on.h"

#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/battle_camera.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/pad_sticks.h"
#include "player_two/target_cursor.h"

#include <cstdint>
#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

namespace lss::player_two::lock_on {

namespace {

// Facts read from the database (RE-p2-lock-on.md §R.4; this build). Each function's first bytes,
// through its stack probe, are checked before it is called.
constexpr std::uint8_t kLockPressedBytes[]  = {0xB8, 0x48, 0x00, 0x00, 0x00, 0xE8, 0x82, 0xBB, 0x74, 0x01};
constexpr std::uint8_t kSwitchBytes[]       = {0x48, 0x89, 0x74, 0x24, 0x08, 0x57, 0xB8, 0x90,
                                               0x00, 0x00, 0x00, 0xE8, 0x80, 0xC7, 0xB0, 0x00};
constexpr std::uint8_t kAxisBytes[]         = {0x40, 0x53, 0xB8, 0x50, 0x00, 0x00, 0x00, 0xE8, 0x38, 0x20, 0xE5, 0x00};
constexpr std::uint8_t kActivateBytes[]     = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x38, 0xC2, 0xEA, 0x00};
constexpr std::uint8_t kDeactivateBytes[]   = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x38, 0xB3, 0xEA, 0x00};
constexpr std::uint8_t kModuleOnBytes[]     = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30,
                                               0x00, 0x00, 0x00, 0xE8, 0xA0, 0xB6, 0xEA, 0x00};
constexpr std::uint8_t kActionStateBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                               0x00, 0x00, 0x00, 0xE8, 0x14, 0xD7, 0x66, 0x01};
constexpr std::uint8_t kAttackStateBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xB8, 0x84, 0x7F, 0x01};
constexpr std::uint8_t kSetTargetBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0xE5, 0x41, 0x01};
constexpr std::uint8_t kParamBytes[]        = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                               0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57,
                                               0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x82, 0x5A, 0x65, 0x01};
constexpr std::uint8_t kNotInFieldBytes[]   = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0xBB, 0x74, 0x01};
constexpr std::uint8_t kHolderTestBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                               0x00, 0x00, 0x00, 0xE8, 0x94, 0xE6, 0x57, 0xFF};
constexpr std::uint8_t kMenuSelectBytes[]   = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xC6, 0xF9, 0x41, 0x01};
constexpr std::uint8_t kEntryIdBytes[]      = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x6E, 0x9D, 0x8A, 0x01};
constexpr std::uint8_t kReleaseBytes[]      = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x26, 0x74, 0x82, 0x01};
constexpr std::uint8_t kOptionsBytes[]      = {0x48, 0x8B, 0x05, 0xF5, 0xCB, 0xB1, 0x08, 0x48, 0x85, 0xC0,
                                               0x74, 0x10, 0x48, 0x8B, 0x80, 0x00, 0x01, 0x00, 0x00};

constexpr platform::FunctionFact kLockPressed{L"BattleTarget_IsTargetLockPressed", 0x1409633A4, kLockPressedBytes};
constexpr platform::FunctionFact kSwitch{L"BattleTarget_SwitchByRightStick", 0x1415A27A0, kSwitchBytes};
constexpr platform::FunctionFact kAxis{L"InputKey_GetAxisValue_Global", 0x14125CEEC, kAxisBytes};
constexpr platform::FunctionFact kActivate{L"PrimaryPCM_Op2CA8_Command3F8", 0x141202CEC, kActivateBytes};
constexpr platform::FunctionFact kDeactivate{L"PrimaryPCM_Op2CA8_SlotCall18", 0x141203BEC, kDeactivateBytes};
constexpr platform::FunctionFact kModuleOn{L"PrimaryPCM_Op2CA8_SlotFlag44", 0x141203880, kModuleOnBytes};
constexpr platform::FunctionFact kActionState{L"CtrlComp_GetActionStateObjIfCurrent", 0x140A4180C, kActionStateBytes};
constexpr platform::FunctionFact kAttackState{L"ActionStateObj_FilterAttackStateClass", 0x1408B6A6C, kAttackStateBytes};
constexpr platform::FunctionFact kSetTarget{L"CtrlComp_SetTarget258", 0x140C909BC, kSetTargetBytes};
constexpr platform::FunctionFact kParam{L"EndParam__GetFloatByKey", 0x140A59490, kParamBytes};
constexpr platform::FunctionFact kNotInField{L"EndGameMode_IsNotInFieldState", 0x140963368, kNotInFieldBytes};
constexpr platform::FunctionFact kHolderTest{L"Character_HolderCompVt858Predicate", 0x142B3088C, kHolderTestBytes};
constexpr platform::FunctionFact kMenuSelect{L"BattleTarget_IsMenuTargetSelectOpen", 0x140C8F560, kMenuSelectBytes};
constexpr platform::FunctionFact kEntryId{L"EndUISys23_GetCurrentEntryId", 0x1408051B8, kEntryIdBytes};
constexpr platform::FunctionFact kRelease{L"TRefCtrlTS__ReleaseWeakRef", 0x140887B00, kReleaseBytes};
constexpr platform::FunctionFact kOptions{L"GetSystemOptionData", 0x14087F15C, kOptionsBytes};

// Player 1's targeting tick (BattleTarget_TickTargeting(sys, dt): soft target, lock toggle and lock
// camera, displayed target and switch) and its two calls in BattleSystem_Tick (§1.1). It reads the
// right stick through the merged stick value, which is whichever pad is pushed furthest.
constexpr std::uint8_t kTickTargetingBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0xF4, 0xAC, 0x74, 0x01};
constexpr platform::FunctionFact kTickTargeting{L"BattleTarget_TickTargeting", 0x140964230, kTickTargetingBytes};
constexpr std::uintptr_t kTickTargetingCalls[] = {0x140C91AA2, 0x140C91CC0};

// The battle system (a pointer global) and its target fields (§R.1, §9).
constexpr std::uintptr_t kBattleSystem = 0x14939BC58;
constexpr std::uint32_t  kCursor       = 0x68;  // the target cursor widget
constexpr std::uint32_t  kDisplayed    = 0x70;  // TWeakPtr {object, reference controller}
constexpr std::uint32_t  kLock         = 0x8C;
constexpr std::uint32_t  kLatch        = 0x8D;
constexpr std::uint32_t  kSkip         = 0x96;  // non-zero: the game skips targeting
// The right stick's FKeys, and the EndParam key "DummyLockCameraFreeTime" (default 1.5 s).
constexpr std::uintptr_t kRightX       = 0x149245BA8;
constexpr std::uintptr_t kRightY       = 0x149245BC0;
constexpr std::uintptr_t kFreeTimeKey  = 0x1491A1D50;
constexpr float          kFreeTime     = 1.5f;
// The option "the right stick switches targets while locked" (0), and the command UI entries in
// which the switch goes to player 1's command menu instead.
constexpr std::uint32_t  kSwitchOption = 0x3E;
constexpr std::int32_t   kCommandEntries[] = {10, 11};
// A character's BattlePlayer component: its target, the owner test (vt+0x80); a target's
// "targetable for" test (vt+0x10); an attack state's target.
constexpr std::uint32_t  kTarget       = 0x258;
constexpr std::uint32_t  kOwnerTest    = 0x80;
constexpr std::uint32_t  kTargetable   = 0x10;
constexpr std::uint32_t  kStateTarget  = 0xC0;
constexpr std::uint8_t   kAttackAction = 2;
// TRefCtrlTS counts; the LockOn module (layer 7) in a Battle operator's module table, its active
// byte and cached target.
constexpr std::uint32_t  kSharedCount  = 0x8;
constexpr std::uint32_t  kWeakCount    = 0xC;
constexpr std::uint8_t   kLockOnLayer  = 7;
constexpr std::uint32_t  kBattleOperator = 0x2CA8;
constexpr std::uint32_t  kModules      = 0x6E0;
constexpr std::uint32_t  kModuleEntry  = 16;
constexpr std::uint32_t  kModuleActive = 0x44;
constexpr std::uint32_t  kModuleTarget = 0x3F8;

constexpr int kMaxLines = 600;

struct Pair  // a TWeakPtr as the game holds it
{
    std::uintptr_t object     = 0;
    std::uintptr_t controller = 0;
    bool operator==(const Pair&) const = default;
};

/// The "targetable for" query the game passes a target (§6.3: flags 0x0101, the character, 0).
struct TargetQuery
{
    std::uint16_t flags = 0x0101;
    std::uint16_t padding[3]{};
    void*         character = nullptr;
    std::uint64_t zero = 0;
};

using TestFn        = std::uint8_t (*)();
using SwitchFn      = std::uint8_t (*)(void* system, float rightX, float rightY);
using AxisFn        = float (*)(void* keyCopy);
using LayerFn       = void (*)(std::uint8_t layer);
using LayerTestFn   = std::uint8_t (*)(std::uint8_t layer);
using ActionStateFn = void* (*)(void* component, std::uint8_t action);
using AttackStateFn = void* (*)(void* state);
using SetTargetFn   = void (*)(void* component, Pair* target);
using ParamFn       = float (*)(const void* key, float fallback);
using CharacterFn   = std::uint8_t (*)(void* character);
using EntryFn       = std::int32_t (*)();
using ReleaseFn     = void (*)(std::uintptr_t controller);
using OptionsFn     = std::uintptr_t (*)();
using OwnerTestFn   = std::uint8_t (*)(void* component, void* object, std::uint64_t zero);
using TargetableFn  = std::uint8_t (*)(void* object, const TargetQuery* query);
using TickFn        = void (*)(void* system, float deltaTime);

// Set once at install.
platform::CallRedirect g_tickTargetingCalls[2];
bool           g_installed = false;
TestFn         g_lockPressed = nullptr;
SwitchFn       g_switch = nullptr;
AxisFn         g_axis = nullptr;
LayerFn        g_activate = nullptr;
LayerFn        g_deactivate = nullptr;
LayerTestFn    g_moduleOn = nullptr;
ActionStateFn  g_actionState = nullptr;
AttackStateFn  g_attackState = nullptr;
SetTargetFn    g_setTarget = nullptr;
ParamFn        g_param = nullptr;
TestFn         g_notInField = nullptr;
CharacterFn    g_holderTest = nullptr;
TestFn         g_menuSelect = nullptr;
EntryFn        g_entryId = nullptr;
ReleaseFn      g_release = nullptr;
OptionsFn      g_options = nullptr;
TestFn         g_isInBattle = nullptr;
std::uintptr_t g_battleSystem = 0;  // the address holding the battle system
std::uintptr_t g_rightX = 0;
std::uintptr_t g_rightY = 0;
std::uintptr_t g_freeTimeKey = 0;

// Game thread. Player 2's copies of the battle system's fields (the displayed target owns one weak
// reference), read back from it at the end of each TargetScope.
Pair         g_displayed;
std::uint8_t g_lock = 0;
std::uint8_t g_latch = 0;
float        g_freeTime = 0.0f;  // the lock camera's rest after a stick move (sys+0x90 for player 1)
bool         g_held = false;
bool         g_failed = false;
int          g_lines = 0;

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

template <class T>
bool Put(std::uintptr_t address, const T& value) noexcept
{
    return platform::WriteGuarded(reinterpret_cast<void*>(address), &value, sizeof(T));
}

template <class R, class... P, class... A>
bool Call(R (*fn)(P...), R& out, A... args) noexcept
{
    __try
    {
        out = fn(args...);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

template <class... P, class... A>
bool Call(void (*fn)(P...), A... args) noexcept
{
    __try
    {
        fn(args...);
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

bool AddWeak(std::uintptr_t controller) noexcept
{
    __try
    {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(controller + kWeakCount));
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

void Fail(const wchar_t* what)
{
    g_failed = true;
    platform::Log(L"lock-on: {} raised an exception; player 2's lock-on is off this session", what);
}

void Note(const std::wstring& text)
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"lock-on: {}", text);
    }
}

bool Alive(const Pair& pair) noexcept
{
    return pair.object != 0 && pair.controller != 0 && Field<std::int32_t>(pair.controller + kSharedCount) > 0;
}

Pair PairAt(std::uintptr_t address) noexcept
{
    return {Field<std::uintptr_t>(address), Field<std::uintptr_t>(address + 8)};
}

bool InBattle() noexcept
{
    std::uint8_t in = 0;
    return g_isInBattle != nullptr && Call(g_isInBattle, in) && in != 0;
}

std::uintptr_t BattleSystem() noexcept
{
    return Field<std::uintptr_t>(g_battleSystem);
}

/// A game test, false (and the feature stopped) if it raises.
bool Test(TestFn test, const wchar_t* what)
{
    std::uint8_t result = 0;
    if (!Call(test, result))
    {
        Fail(what);
        return false;
    }
    return result != 0;
}

/// One right-stick axis as the game reads it (pad 2's inside the member tick): a copy of the FKey,
/// its details' reference taken as the game's copy does; the getter consumes the copy.
float Axis(std::uintptr_t key, bool& ok)
{
    std::uintptr_t copy[3]{};
    ok = platform::CopyGuarded(copy, reinterpret_cast<const void*>(key), sizeof(copy));
    if (ok && copy[2] != 0)
    {
        ok = Put(copy[2] + 8, Field<std::uint32_t>(copy[2] + 8) + 1);
    }
    float value = 0.0f;
    ok = ok && Call(g_axis, value, static_cast<void*>(copy));
    if (!ok)
    {
        Fail(L"a right-stick read");
    }
    return value;
}

// The LockOn module on the primary camera manager: player 2's inside the member tick.
bool ModuleOn()
{
    std::uint8_t on = 0;
    if (!Call(g_moduleOn, on, kLockOnLayer))
    {
        Fail(L"the lock camera test");
        return false;
    }
    return on != 0;
}

void SetModule(bool on, const wchar_t* why)
{
    if (!Call(on ? g_activate : g_deactivate, kLockOnLayer))
    {
        return Fail(L"a lock camera call");
    }
    Note(std::format(L"player 2's lock camera {} ({})", on ? L"on" : L"off", why));
}

bool PlayerTwoBattleCamera() noexcept
{
    return battle_camera::PlayerTwoBattleActive(ReadPointer(generation::PlayerTwoController(), facts::kControllerCameraManager));
}

/// Replaces the battle tick's calls to player 1's targeting tick: while player 2 plays, it reads
/// pad 1's own right stick, so pad 2 neither switches player 1's locked target nor moves player 1's
/// lock camera off it (pad_sticks.h: player 1's gameplay on pad 1).
void PlayerOneTargeting(void* system, float deltaTime) noexcept
{
    const auto engine = reinterpret_cast<TickFn>(g_tickTargetingCalls[0].Original());
    if (!generation::OnGameThread() || !generation::Active() || !pad_sticks::Enabled() ||
        !pad_sticks::PartActs(pad_sticks::kPartPlayerOneMove))
    {
        return engine(system, deltaTime);
    }
    {
        const pad_sticks::StickScope sticks(pad_sticks::ControllerIdOf(generation::PlayerOneController()));
        static bool refusedLogged = false;
        if (!sticks.Held() && !refusedLogged)
        {
            refusedLogged = true;  // another scope was open: this tick reads whatever stick that scope holds
            Note(L"player 1's targeting ran without pad 1's own stick (another stick scope was open)");
        }
        engine(system, deltaTime);
    }
    // Player 1's own target and lock, on change, beside player 2's in the same log.
    static std::uintptr_t loggedTarget = 0;
    static std::uint8_t   loggedLock   = 0;
    const auto            base   = reinterpret_cast<std::uintptr_t>(system);
    const auto            target = Field<std::uintptr_t>(base + kDisplayed);
    const auto            lock   = Field<std::uint8_t>(base + kLock);
    if (target != loggedTarget || lock != loggedLock)
    {
        loggedTarget = target;
        loggedLock   = lock;
        try
        {
            Note(std::format(L"player 1's target {:#x} lock {} (player 2's {:#x} lock {})", target, lock, g_displayed.object,
                             g_lock));
        }
        catch (...)
        {
        }
    }
}

/// The game's toggle for player 2 (BattleTarget_ToggleLock less its Global.LockOn write).
void Toggle(std::uintptr_t system, void* component)
{
    const std::uint8_t lock = Field<std::uint8_t>(system + kLock) == 0 ? 1 : 0;
    Put(system + kLock, lock);
    if (lock != 0)
    {
        // The member's current attack target, if it is attacking, becomes its target.
        void* state = nullptr;
        if (!Call(g_actionState, state, component, kAttackAction) || (state != nullptr && !Call(g_attackState, state, state)))
        {
            return Fail(L"the attack-state read");
        }
        const Pair target = state != nullptr ? PairAt(reinterpret_cast<std::uintptr_t>(state) + kStateTarget) : Pair{};
        if (Alive(target))
        {
            const auto   test = reinterpret_cast<OwnerTestFn>(Field<std::uintptr_t>(Field<std::uintptr_t>(
                                    reinterpret_cast<std::uintptr_t>(component)) + kOwnerTest));
            std::uint8_t own  = 1;
            Pair         given = target;  // the setter consumes one weak reference
            if (!Call(test, own, component, reinterpret_cast<void*>(target.object), std::uint64_t{0}) ||
                (own == 0 && (!AddWeak(target.controller) || !Call(g_setTarget, component, &given))))
            {
                return Fail(L"the lock's attack-target hand-over");
            }
        }
    }
    g_freeTime = 0.0f;
    Note(std::format(L"player 2 {} (displayed {:#x})", lock != 0 ? L"locked on" : L"released the lock",
                     Field<std::uintptr_t>(system + kDisplayed)));
    if (!PlayerTwoBattleCamera())
    {
        return;
    }
    if (lock != 0)
    {
        if (!ModuleOn() && !g_failed && Alive(PairAt(system + kDisplayed)))
        {
            SetModule(true, L"locked");
        }
    }
    else if (!Test(g_notInField, L"the field-state test") && !g_failed)
    {
        SetModule(false, L"released outside battle state");
    }
}

/// The game's lock-camera upkeep with player 2's lock, target, rest timer and right stick.
void Upkeep(std::uintptr_t system, void* member, float deltaTime)
{
    if (g_freeTime > 0.0f)
    {
        g_freeTime -= deltaTime;
        if (!(g_freeTime > 0.0f))
        {
            g_freeTime = 0.0f;
        }
    }
    if (!PlayerTwoBattleCamera())
    {
        return;
    }
    const bool locked    = Field<std::uint8_t>(system + kLock) != 0;
    const bool displayed = Alive(PairAt(system + kDisplayed));
    if (!locked)
    {
        if (!Test(g_notInField, L"the field-state test") || g_failed)
        {
            return;
        }
        std::uint8_t holder = 0;
        if (!Call(g_holderTest, holder, member))
        {
            return Fail(L"the character test");
        }
        if (holder == 0)
        {
            return;
        }
        bool        ok = true;
        const float x  = Axis(g_rightX, ok);
        const float y  = ok ? Axis(g_rightY, ok) : 0.0f;
        if (!ok)
        {
            return;
        }
        if (x == 0.0f && y == 0.0f)
        {
            if (g_freeTime == 0.0f && displayed && !ModuleOn() && !g_failed)
            {
                SetModule(true, L"stick at rest on a target");
            }
            return;
        }
        if (ModuleOn() && !g_failed)
        {
            SetModule(false, L"stick moved");
        }
        if (!g_failed && !Call(g_param, g_freeTime, reinterpret_cast<const void*>(g_freeTimeKey), kFreeTime))
        {
            Fail(L"the rest-time parameter");
        }
        return;
    }
    const bool on = ModuleOn();
    if (!g_failed && displayed != on)
    {
        SetModule(displayed, displayed ? L"locked on a target" : L"locked target lost");
    }
}

/// The game's switch on pad 2's right stick, with the game's gate.
void Switch(std::uintptr_t system, void* member)
{
    const Pair displayed = PairAt(system + kDisplayed);
    if (Field<std::uint8_t>(system + kLock) == 0 || !Alive(displayed))
    {
        return;
    }
    const auto        test = reinterpret_cast<TargetableFn>(Field<std::uintptr_t>(Field<std::uintptr_t>(displayed.object) + kTargetable));
    const TargetQuery query{.character = member};
    std::uint8_t      targetable = 0;
    std::uintptr_t    options    = 0;
    std::int32_t      entry      = 0;
    if (!Call(test, targetable, reinterpret_cast<void*>(displayed.object), &query) || !Call(g_options, options) ||
        !Call(g_entryId, entry))
    {
        return Fail(L"the switch gate");
    }
    if (targetable == 0 || options == 0 || Field<std::uint8_t>(options + kSwitchOption) != 0 || entry == kCommandEntries[0] ||
        entry == kCommandEntries[1] || Test(g_menuSelect, L"the menu-select test") || g_failed)
    {
        return;
    }
    bool        ok = true;
    const float x  = Axis(g_rightX, ok);
    const float y  = ok ? Axis(g_rightY, ok) : 0.0f;
    std::uint8_t switched = 0;
    if (!ok)
    {
        return;
    }
    if (!Call(g_switch, switched, reinterpret_cast<void*>(system), x, y))
    {
        return Fail(L"the target switch");
    }
    if (switched != 0)
    {
        Note(std::format(L"player 2 switched target {:#x} -> {:#x}", displayed.object, Field<std::uintptr_t>(system + kDisplayed)));
    }
}

/// Player 2's displayed target follows the member's own target (the game's choice for the leader,
/// without player 1's cursor and target panel).
void Follow(std::uintptr_t system, void* component)
{
    if (Test(g_menuSelect, L"the menu-select test") || g_failed)
    {
        return;
    }
    const Pair target  = PairAt(reinterpret_cast<std::uintptr_t>(component) + kTarget);
    const Pair current = PairAt(system + kDisplayed);
    if (target == current)
    {
        return;
    }
    if (target.controller != 0 && !AddWeak(target.controller))
    {
        return Fail(L"the displayed-target hand-over");
    }
    if (!Put(system + kDisplayed, target))
    {
        if (target.controller != 0)
        {
            Call(g_release, target.controller);  // the reference just taken, never handed over
        }
        return Fail(L"the displayed-target hand-over");
    }
    if (current.controller != 0 && !Call(g_release, current.controller))
    {
        Fail(L"the displayed-target release");
    }
}

/// Player 2's lock ends with the battle, as the game's teardown ends player 1's.
void EndOfBattle()
{
    const bool lockCamera = ModuleOn();
    if (!g_failed && lockCamera)
    {
        SetModule(false, L"battle over");
    }
    if (g_displayed.controller != 0 && !Call(g_release, g_displayed.controller))
    {
        Fail(L"the displayed-target release");
    }
    target_cursor::Release();
    Note(std::format(L"battle over: player 2's lock {} cleared", g_lock));
    g_displayed = {};
    g_lock      = 0;
    g_latch     = 0;
    g_freeTime  = 0.0f;
}

/// A camera manager's LockOn module as the log shows it: its active bytes and cached target.
struct LockCamera
{
    bool           present = false;
    std::uint8_t   active[2]{};
    std::uintptr_t target = 0;
    bool operator==(const LockCamera&) const = default;
};

LockCamera LockCameraOf(void* controller) noexcept
{
    const auto camera = reinterpret_cast<std::uintptr_t>(ReadPointer(controller, facts::kControllerCameraManager));
    const auto op     = Field<std::uintptr_t>(camera != 0 ? camera + kBattleOperator : 0);
    const auto table  = Field<std::uintptr_t>(op != 0 ? op + kModules : 0);
    const auto module = Field<std::uintptr_t>(table != 0 ? table + kLockOnLayer * kModuleEntry : 0);
    if (module == 0)
    {
        return {};
    }
    return {true, {Field<std::uint8_t>(module + kModuleActive), Field<std::uint8_t>(module + kModuleActive + 1)},
            Field<std::uintptr_t>(module + kModuleTarget)};
}

std::wstring DescribeCamera(const LockCamera& camera)
{
    return camera.present ? std::format(L"{}{} target {:#x}", camera.active[0], camera.active[1], camera.target) : L"-";
}

/// Everything the per-frame log shows, as raw values: compared each frame, formatted on change.
struct Snapshot
{
    std::uint8_t   lock = 0, latch = 0;
    bool           resting = false;
    std::uintptr_t displayed = 0, memberTarget = 0;
    LockCamera     cameras[2];
    bool operator==(const Snapshot&) const = default;
};

Snapshot g_snapshot;  // game thread: as last logged

void Log(std::uintptr_t system, void* component)
{
    if (g_lines >= kMaxLines)
    {
        return;
    }
    const Snapshot now{Field<std::uint8_t>(system + kLock),
                       Field<std::uint8_t>(system + kLatch),
                       g_freeTime > 0.0f,
                       Field<std::uintptr_t>(system + kDisplayed),
                       Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(component) + kTarget),
                       {LockCameraOf(generation::PlayerOneController()), LockCameraOf(generation::PlayerTwoController())}};
    if (now == g_snapshot)
    {
        return;
    }
    g_snapshot = now;
    Note(std::format(L"P2 lock {} latch {} rest {} displayed {:#x} member target {:#x} | lock camera P1 {} P2 {}", now.lock,
                     now.latch, now.resting ? 1 : 0, now.displayed, now.memberTarget, DescribeCamera(now.cameras[0]),
                     DescribeCamera(now.cameras[1])));
}
}  // namespace

TargetScope::TargetScope() noexcept
{
    if (!g_installed || g_failed || g_held || !generation::OnGameThread())
    {
        return;
    }
    const std::uintptr_t system = BattleSystem();
    if (system == 0 || !InBattle())
    {
        return;
    }
    const Pair saved = PairAt(system + kDisplayed);
    cursor_          = Field<std::uintptr_t>(system + kCursor);
    object_          = saved.object;
    controller_      = saved.controller;
    lock_            = Field<std::uint8_t>(system + kLock);
    latch_           = Field<std::uint8_t>(system + kLatch);
    if (!Put(system + kCursor, std::uintptr_t{0}) || !Put(system + kDisplayed, g_displayed) || !Put(system + kLock, g_lock) ||
        !Put(system + kLatch, g_latch))
    {
        Put(system + kCursor, cursor_);  // the fields are the game's heap: put back what was written
        Put(system + kDisplayed, saved);
        Put(system + kLock, lock_);
        Put(system + kLatch, latch_);
        return;
    }
    system_ = system;
    g_held  = true;
}

TargetScope::~TargetScope()
{
    if (system_ == 0)
    {
        return;
    }
    g_displayed = PairAt(system_ + kDisplayed);
    g_lock      = Field<std::uint8_t>(system_ + kLock);
    g_latch     = Field<std::uint8_t>(system_ + kLatch);
    if (const auto cursor = Field<std::uintptr_t>(system_ + kCursor); cursor != 0)
    {
        try  // a destructor: nothing may leave it
        {
            Note(std::format(L"a target cursor {:#x} was made inside player 2's scope; player 1's is put back over it",
                             cursor));
        }
        catch (...)
        {
        }
    }
    Put(system_ + kCursor, cursor_);
    Put(system_ + kDisplayed, Pair{object_, controller_});
    Put(system_ + kLock, lock_);
    Put(system_ + kLatch, latch_);
    g_held = false;
}

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    g_lockPressed = reinterpret_cast<TestFn>(find(kLockPressed));
    g_switch      = reinterpret_cast<SwitchFn>(find(kSwitch));
    g_axis        = reinterpret_cast<AxisFn>(find(kAxis));
    g_activate    = reinterpret_cast<LayerFn>(find(kActivate));
    g_deactivate  = reinterpret_cast<LayerFn>(find(kDeactivate));
    g_moduleOn    = reinterpret_cast<LayerTestFn>(find(kModuleOn));
    g_actionState = reinterpret_cast<ActionStateFn>(find(kActionState));
    g_attackState = reinterpret_cast<AttackStateFn>(find(kAttackState));
    g_setTarget   = reinterpret_cast<SetTargetFn>(find(kSetTarget));
    g_param       = reinterpret_cast<ParamFn>(find(kParam));
    g_notInField  = reinterpret_cast<TestFn>(find(kNotInField));
    g_holderTest  = reinterpret_cast<CharacterFn>(find(kHolderTest));
    g_menuSelect  = reinterpret_cast<TestFn>(find(kMenuSelect));
    g_entryId     = reinterpret_cast<EntryFn>(find(kEntryId));
    g_release     = reinterpret_cast<ReleaseFn>(find(kRelease));
    g_options     = reinterpret_cast<OptionsFn>(find(kOptions));
    g_isInBattle  = reinterpret_cast<TestFn>(find(facts::kIsInBattle));
    const auto system   = image.Rebase(kBattleSystem, sizeof(void*));
    const auto rightX   = image.Rebase(kRightX, 24);
    const auto rightY   = image.Rebase(kRightY, 24);
    const auto freeTime = image.Rebase(kFreeTimeKey, sizeof(void*));
    g_installed = g_lockPressed && g_switch && g_axis && g_activate && g_deactivate && g_moduleOn && g_actionState &&
                  g_attackState && g_setTarget && g_param && g_notInField && g_holderTest && g_menuSelect && g_entryId &&
                  g_release && g_options && g_isInBattle && system && rightX && rightY && freeTime;
    g_battleSystem = system ? *system : 0;
    g_rightX       = rightX ? *rightX : 0;
    g_rightY       = rightY ? *rightY : 0;
    g_freeTimeKey  = freeTime ? *freeTime : 0;
    platform::Log(L"lock-on: {}", g_installed ? L"ready (player 2 gets their own lock, switching and lock camera)"
                                              : L"UNAVAILABLE (a check failed); player 2 has no lock-on");
    // Both of player 1's targeting calls on pad 1, or neither.
    const std::uintptr_t tick = find(kTickTargeting);
    bool                 own  = tick != 0;
    for (std::size_t i = 0; own && i < 2; ++i)
    {
        const auto site = image.Rebase(kTickTargetingCalls[i], 5);
        own = site && g_tickTargetingCalls[i].Install(*site, tick, reinterpret_cast<std::uintptr_t>(&PlayerOneTargeting));
    }
    if (!own)
    {
        for (auto& call : g_tickTargetingCalls)
        {
            if (call.IsInstalled())
            {
                call.Restore();
            }
        }
    }
    platform::Log(L"lock-on: player 1's targeting {}", own ? L"reads pad 1's own right stick"
                                                          : L"still reads the merged stick (a check failed)");
    return g_installed;
}

void Tick(void* member, void* component, float deltaTime, Parts parts) noexcept
{
    if (!g_installed || g_failed || member == nullptr || !generation::OnGameThread())
    {
        return;
    }
    try
    {
        if (!g_held)
        {
            // Out of battle (no scope): player 2's lock ends with it.
            if ((g_lock != 0 || g_displayed.controller != 0 || g_freeTime > 0.0f) && !InBattle())
            {
                EndOfBattle();
            }
            return;
        }
        const std::uintptr_t system = BattleSystem();
        if (component == nullptr || Field<std::uint8_t>(system + kSkip) != 0)
        {
            return;
        }
        // The game's order: the press, the lock camera, then the switch and the displayed target.
        if (parts.lock && Test(g_lockPressed, L"the TargetLock press test") && !g_failed)
        {
            Toggle(system, component);
        }
        if (parts.camera && !g_failed)
        {
            Upkeep(system, member, deltaTime);
        }
        if (parts.switching && !g_failed)
        {
            Switch(system, member);
        }
        if (!g_failed)
        {
            Follow(system, component);
        }
        if (parts.cursor && !g_failed)
        {
            const Pair displayed = PairAt(system + kDisplayed);
            target_cursor::Update(system, member, displayed.object, displayed.controller, Field<std::uint8_t>(system + kLock) != 0);
        }
        else
        {
            target_cursor::Release();
        }
        Log(system, component);
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    for (auto& call : g_tickTargetingCalls)
    {
        if (call.IsInstalled() && !call.Restore())
        {
            platform::Log(L"lock-on: a targeting call was changed after us; left in place (it passes through)");
        }
    }
}

void PlayerTwoOut() noexcept
{
    if (g_held)
    {
        return;
    }
    target_cursor::Release();
    if (g_displayed.controller != 0 && g_release != nullptr)
    {
        Call(g_release, g_displayed.controller);
    }
    g_displayed = {};
    g_lock      = 0;
    g_latch     = 0;
    g_freeTime  = 0.0f;
}

bool Held() noexcept
{
    return g_held;
}

std::uintptr_t DisplayedObject() noexcept
{
    return g_displayed.object;
}

void Displayed(std::uintptr_t& object, std::uintptr_t& controller) noexcept
{
    object     = g_displayed.object;
    controller = g_displayed.controller;
}

}  // namespace lss::player_two::lock_on
