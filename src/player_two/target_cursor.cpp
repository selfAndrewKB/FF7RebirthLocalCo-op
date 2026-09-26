#include "player_two/target_cursor.h"

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"

#include <cstdint>
#include <format>

namespace lss::player_two::target_cursor {

namespace {

using platform::CallGuarded;

// Facts read from the database (RE-p2-hud-targets.md §2, R2; this build). Each function's first
// bytes, through its stack probe, are checked before it is called.
constexpr std::uint8_t kCreateBytes[]  = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                          0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8, 0x0B, 0x0E, 0xD2, 0x00};
constexpr std::uint8_t kMoveBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x30,
                                          0x00, 0x00, 0x00, 0xE8, 0x58, 0x1E, 0x42, 0x01};
constexpr std::uint8_t kStyleBytes[]   = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                          0x00, 0x00, 0x00, 0xE8, 0x8C, 0x08, 0x42, 0x01};
constexpr std::uint8_t kFlagBytes[]    = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                          0x00, 0x00, 0x00, 0xE8, 0xA8, 0xA6, 0xEA, 0x00};
constexpr std::uint8_t kReleaseBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xDC, 0x42, 0x07, 0x01};
constexpr std::uint8_t kAllowedBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                          0x00, 0x00, 0x00, 0xE8, 0x28, 0xE6, 0x64, 0x01};
constexpr std::uint8_t kBlockedBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xF6, 0xFE, 0x41, 0x01};
constexpr std::uint8_t kHolderBytes[]  = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x28, 0x05, 0xCA, 0x00};
constexpr std::uint8_t kWorldBytes[]   = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x7E, 0x76, 0x82, 0x01};

constexpr platform::FunctionFact kCreate{L"EndUI22_CreateTargetCursor", 0x14138E110, kCreateBytes};
constexpr platform::FunctionFact kMove{L"EndUI22_MoveTargetCursor", 0x140C8D0C8, kMoveBytes};
constexpr platform::FunctionFact kStyle{L"EndUI22_SetCursorLockedStyle", 0x140C8E694, kStyleBytes};
constexpr platform::FunctionFact kFlag{L"EndUI22_SetCursorEntryFlag", 0x141204878, kFlagBytes};
constexpr platform::FunctionFact kRelease{L"EndUI22_ReleaseTargetCursor", 0x14103AC48, kReleaseBytes};
constexpr platform::FunctionFact kAllowed{L"Battle_CursorAllowed_GameFlags", 0x140A608F8, kAllowedBytes};
constexpr platform::FunctionFact kBlocked{L"BattleTarget_CursorBlockedBySub48", 0x140C8F030, kBlockedBytes};
constexpr platform::FunctionFact kHolderTest{L"the holder class test", 0x14140E9FC, kHolderBytes};
constexpr platform::FunctionFact kWorld{L"GetGameWorld_FromGlobal", 0x1408878A8, kWorldBytes};

// The target's virtual functions the game calls for its cursor (§2.2): the owner actor, the "show a
// cursor" answer for a character, the cursor location. The character's holder (an object embedded at
// +0x4B8) gives the object whose two answers set the entry flag.
constexpr std::uint32_t kOwnerSlot    = 0x28;
constexpr std::uint32_t kShowSlot     = 0x18;
constexpr std::uint32_t kLocationSlot = 0x50;
constexpr std::uint32_t kHolder       = 0x4B8;
constexpr std::uint32_t kHolderObject = 0x1C8;
constexpr std::uint32_t kHolderAnswer = 0x860;
constexpr std::uint32_t kSharedCount  = 0x8;
constexpr std::uint8_t  kUnlocked     = 1;
constexpr std::uint8_t  kLocked       = 3;

constexpr int kMaxLines = 200;

/// The query the game passes a target for its cursor (§2.2 step 4: flags 0x0101, the character, 0).
struct Query
{
    std::uint16_t flags = 0x0101;
    std::uint16_t padding[3]{};
    void*         character = nullptr;
    std::uint64_t zero = 0;
};

using CreateFn   = void* (*)(void* world, const float* location, std::uint8_t style);
using MoveFn     = std::uint8_t (*)(void* widget, const float* location);
using ByteFn     = void (*)(void* widget, std::uint8_t value);
using ReleaseFn  = void (*)(void* widget);
using TestFn     = std::uint8_t (*)();
using BlockedFn  = std::uint8_t (*)(void* system, void* owner, void* character);
using ObjectTest = std::uint8_t (*)(void* object);
using WorldFn    = void* (*)();
using OwnerFn    = void* (*)(void* target);
using ShowFn     = void (*)(void* target, std::uint8_t* show, const Query* query);
using LocationFn = void* (*)(void* target, float* location);
using HolderFn   = void* (*)(void* holder);
using AnswerFn   = std::uint8_t (*)(void* object, std::int32_t which);
using WeakGetFn  = void* (*)(const std::uint64_t* weak);
using WeakSetFn  = void (*)(std::uint64_t* weak, void* object);

// Set once at install.
bool       g_installed = false;
CreateFn   g_create    = nullptr;
MoveFn     g_move      = nullptr;
ByteFn     g_style     = nullptr;
ByteFn     g_flag      = nullptr;
ReleaseFn  g_release   = nullptr;
TestFn     g_allowed   = nullptr;
BlockedFn  g_blocked   = nullptr;
ObjectTest g_holder    = nullptr;
WorldFn    g_world     = nullptr;
WeakGetFn  g_weakGet   = nullptr;
WeakSetFn  g_weakSet   = nullptr;

// Game thread.
void*         g_widget = nullptr;  // player 2's cursor, from the last create
std::uint64_t g_weak   = 0;        // its FWeakObjectPtr, to see it go
bool          g_failed = false;
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

template <class Fn>
Fn Slot(const void* object, std::uint32_t slot) noexcept
{
    return reinterpret_cast<Fn>(Field<std::uintptr_t>(Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(object)) + slot));
}

void Fail(const wchar_t* what)
{
    g_failed = true;
    g_widget = nullptr;
    platform::Log(L"target cursor: {} raised an exception; player 2 has no cursor of their own this session", what);
}

void Note(const std::wstring& text)
{
    if (g_lines < kMaxLines)
    {
        ++g_lines;
        platform::Log(L"target cursor: {}", text);
    }
}

bool Alive() noexcept
{
    void* live = nullptr;
    return g_widget != nullptr && CallGuarded(g_weakGet, live, static_cast<const std::uint64_t*>(&g_weak)) && live == g_widget;
}

/// The entry flag the game sets on a new cursor from the character's holder (§2.2 step 6).
std::uint8_t EntryFlag(void* member)
{
    void* holderBase = static_cast<std::byte*>(member) + kHolder;
    void* object     = nullptr;
    if (!CallGuarded(Slot<HolderFn>(holderBase, kHolderObject), object, holderBase) || object == nullptr)
    {
        return 0;
    }
    std::uint8_t kind = 0, first = 0, second = 0;
    const auto   answer = Slot<AnswerFn>(object, kHolderAnswer);
    return CallGuarded(g_holder, kind, object) && kind != 0 && CallGuarded(answer, first, object, std::int32_t{0}) &&
                   first != 0 && CallGuarded(answer, second, object, std::int32_t{5}) && second != 0
               ? 1
               : 0;
}

/// The game's tests for showing a cursor on `target` for `member`, and its location if so.
bool Shows(std::uintptr_t system, void* member, void* target, float (&location)[3])
{
    void*        owner   = nullptr;
    std::uint8_t blocked = 0, allowed = 0, show = 0;
    const Query  query{.character = member};
    void*        ignored = nullptr;
    if (!CallGuarded(Slot<OwnerFn>(target, kOwnerSlot), owner, target) ||
        !CallGuarded(g_blocked, blocked, reinterpret_cast<void*>(system), owner, member))
    {
        Fail(L"the cursor block test");
        return false;
    }
    if (blocked != 0)
    {
        return false;
    }
    if (!CallGuarded(Slot<ShowFn>(target, kShowSlot), target, &show, &query) || !CallGuarded(g_allowed, allowed))
    {
        Fail(L"the cursor show test");
        return false;
    }
    if (show == 0 || allowed == 0)
    {
        return false;
    }
    if (!CallGuarded(Slot<LocationFn>(target, kLocationSlot), ignored, target, static_cast<float*>(location)))
    {
        Fail(L"the cursor location");
        return false;
    }
    return true;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    g_create  = reinterpret_cast<CreateFn>(find(kCreate));
    g_move    = reinterpret_cast<MoveFn>(find(kMove));
    g_style   = reinterpret_cast<ByteFn>(find(kStyle));
    g_flag    = reinterpret_cast<ByteFn>(find(kFlag));
    g_release = reinterpret_cast<ReleaseFn>(find(kRelease));
    g_allowed = reinterpret_cast<TestFn>(find(kAllowed));
    g_blocked = reinterpret_cast<BlockedFn>(find(kBlocked));
    g_holder  = reinterpret_cast<ObjectTest>(find(kHolderTest));
    g_world   = reinterpret_cast<WorldFn>(find(kWorld));
    g_weakGet = reinterpret_cast<WeakGetFn>(find(facts::kWeakObjectGet));
    g_weakSet = reinterpret_cast<WeakSetFn>(find(facts::kWeakObjectAssign));
    g_installed = g_create && g_move && g_style && g_flag && g_release && g_allowed && g_blocked && g_holder && g_world &&
                  g_weakGet && g_weakSet;
    platform::Log(L"target cursor: {}", g_installed ? L"ready (player 2 gets a cursor of their own)"
                                                    : L"UNAVAILABLE (a check failed); player 2 has no cursor of their own");
    return g_installed;
}

void Update(std::uintptr_t system, void* member, std::uintptr_t targetObject, std::uintptr_t targetController,
            bool locked) noexcept
{
    if (!g_installed || g_failed || !generation::OnGameThread())
    {
        return;
    }
    try
    {
        const bool alive = targetObject != 0 && targetController != 0 && Field<std::int32_t>(targetController + kSharedCount) > 0;
        float      location[3]{};
        if (!alive || member == nullptr || !Shows(system, member, reinterpret_cast<void*>(targetObject), location))
        {
            Release();
            return;
        }
        if (Alive())
        {
            std::uint8_t onScreen = 0;
            if (!CallGuarded(g_move, onScreen, g_widget, static_cast<const float*>(location)))
            {
                return Fail(L"the cursor move");
            }
        }
        else
        {
            void* world = nullptr;
            if (!CallGuarded(g_world, world))
            {
                return Fail(L"the world read");
            }
            g_widget = nullptr;
            if (world != nullptr &&
                !CallGuarded(g_create, g_widget, world, static_cast<const float*>(location), locked ? kLocked : kUnlocked))
            {
                return Fail(L"the cursor create");
            }
            if (g_widget == nullptr)
            {
                return;
            }
            g_weak = 0;
            if (!CallGuarded(g_weakSet, &g_weak, g_widget) || !CallGuarded(g_flag, g_widget, EntryFlag(member)))
            {
                return Fail(L"the new cursor's set-up");
            }
            Note(std::format(L"player 2's cursor {:#x} made on {:#x}", reinterpret_cast<std::uintptr_t>(g_widget), targetObject));
        }
        if (!CallGuarded(g_style, g_widget, locked ? std::uint8_t{1} : std::uint8_t{0}))
        {
            Fail(L"the cursor style");
        }
    }
    catch (...)
    {
    }
}

void Release() noexcept
{
    if (g_installed && Alive())
    {
        void* released = g_widget;
        g_widget       = nullptr;  // released widgets go to the game's shared reuse slot: never ours again
        if (!CallGuarded(g_release, released))
        {
            Fail(L"the cursor release");
            return;
        }
        try
        {
            Note(std::format(L"player 2's cursor {:#x} released", reinterpret_cast<std::uintptr_t>(released)));
        }
        catch (...)
        {
        }
    }
    g_widget = nullptr;
}

void* Widget() noexcept
{
    return Alive() ? g_widget : nullptr;
}

}  // namespace lss::player_two::target_cursor
