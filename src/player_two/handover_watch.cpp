#include "player_two/handover_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "platform/build_value.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"
#include "player_two/party_member.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>

namespace lss::player_two::handover_watch {

namespace {

using RC::Unreal::UObject;
using Clock = std::chrono::steady_clock;

// The AI controller's brain stack and state (RE-p2-member-handover.md §3; this build).
constexpr std::uint32_t kAiDefaultState = 0x340;  // u8, EEndAIState
constexpr std::uint32_t kAiPauseMask    = 0x460;  // u32
constexpr std::uint32_t kAiBrains       = 0x478;  // 20 pointers; index 0 is the base brain
constexpr int           kBrainSlots     = 20;
constexpr std::uint32_t kAiActiveBrain  = 0x518;
constexpr std::uint32_t kAiActiveIndex  = 0x520;  // u8 (21: none)
constexpr std::uint32_t kBrainMode      = 0x18;   // u8
constexpr std::uint32_t kBrainIndex     = 0x19;   // u8; 0 is the base brain
constexpr std::uint32_t kBrainState     = 0x20;
constexpr std::uint32_t kStateType      = 0x18;   // u8, EEndAIState

// The states' catch-up fields (§1.2, §1.3).
constexpr std::uint32_t kFollowRate      = 0x6D0;  // float
constexpr std::uint32_t kFollowMode      = 0x39E;  // u8
constexpr std::uint32_t kLeadTarget      = 0x3B0;  // FWeakObjectPtr
constexpr std::uint32_t kLeadArrived     = 0x3C1;  // u8: within 8 m of the destination
constexpr std::uint32_t kLeadDestination = 0x3E8;  // three floats

// The character's movement object (embedded at +0x1400; its flags at +0x1408) and its anim instance, a
// weak pointer at +0x160 of it (§1.1).
constexpr platform::PerBuild<std::uint32_t> kCharacterMove = {0x1400, 0x1420};  // 1.005: AIState_ApplyStateParams 0x14123E573 / RevertStateParams, FollowState reset 0x140EB0350 mov rcx,[rsi+1420h]
constexpr platform::PerBuild<std::uint32_t> kCharacterFlags = {0x1408, 0x1428};  // u8; bit 2 set by LEAD's and FOLLOW's Enter; 1.005: the +0x4B8 interface's vt+0xB8 flag setter 0x140ED3830 reads [rcx+0F70h] (1.004 0x140AEA668 [rcx+0F50h])
constexpr std::uint32_t kMoveAnimInstance = 0x160;

// UEndCharacterAnimInstance: the root-motion and rotation-rate products and their TMap<u8, float>
// modifier maps (a TSparseArray: data +0, count +8, its allocation bits inline at +0x10 or at the
// pointer at +0x20; 16-byte elements, key u8 at +0, value float at +4).
constexpr std::uint32_t kAnimRotationScale   = 0x550;
constexpr std::uint32_t kAnimRotationMap     = 0x558;
constexpr std::uint32_t kAnimRootMotionScale = 0x5A8;
constexpr std::uint32_t kAnimRootMotionMap   = 0x5B0;
constexpr std::uint32_t kAnimSpeedScale      = 0x10CC;  // float, for comparison
constexpr std::uint32_t kAnimFlags           = 0x11DC;  // u32; bit 0 set by LEAD's Enter
constexpr std::uint32_t kMapCount            = 0x8;
constexpr std::uint32_t kMapInlineBits       = 0x10;
constexpr std::uint32_t kMapHeapBits         = 0x20;
constexpr std::uint32_t kMapElement          = 16;
constexpr std::int32_t  kMapShown            = 32;

constexpr std::uint8_t kStateDoNothing = 1;
constexpr std::uint8_t kStateBattle    = 3;
constexpr std::uint8_t kStateStay      = 4;
constexpr std::uint8_t kStateLead      = 6;
constexpr std::uint8_t kStateFollow    = 7;
constexpr std::uint8_t kUnread         = 0xFF;

// USceneComponent's world location (as nav_strand reads it).
constexpr std::uint32_t kComponentWorldLocation = 0x1D0;

constexpr int   kMaxMembers        = 8;
constexpr int   kPending           = 4;
constexpr auto  kWatchEvery        = std::chrono::milliseconds(100);
constexpr auto  kHeldEvery         = std::chrono::seconds(1);
constexpr auto  kSpeedEvery        = std::chrono::milliseconds(500);
constexpr auto  kSpeedLineEvery    = std::chrono::seconds(5);
constexpr auto  kStillThereEvery   = std::chrono::seconds(5);
constexpr auto  kAgain             = std::chrono::seconds(1);
constexpr float kMovingCmPerSecond = 150.0f;
constexpr float kMovedCm           = 100.0f;

using CountFn          = std::int32_t (*)();
using TypeAtFn         = std::uint8_t (*)(std::int32_t ordinal);  // al only
using TypeFn           = std::uint8_t (*)();                      // al only
using CharacterByTypeFn = void* (*)(std::uint8_t lane, std::uint8_t playerType);

struct Functions
{
    CountFn           count           = nullptr;
    TypeAtFn          typeAt          = nullptr;
    TypeFn            fieldLeader     = nullptr;
    TypeFn            defaultLeader   = nullptr;
    CharacterByTypeFn characterByType = nullptr;
};
Functions g_fn;  // set once at install
bool      g_installed = false;

struct Order
{
    std::int32_t count = -1;  // -1: unread
    std::array<std::uint8_t, kMaxMembers> types{};
    std::uint8_t fieldLeader   = kUnread;
    std::uint8_t defaultLeader = kUnread;
    bool operator==(const Order&) const = default;
};

struct Ai
{
    const void*  controller  = nullptr;
    std::uint8_t state       = kUnread;
    std::uint8_t brain       = kUnread;  // the active brain's index
    std::uint8_t brainMode   = kUnread;
    std::uint8_t activeIndex = kUnread;  // ai+0x520
    std::uint8_t defaultState = kUnread;
    std::uint32_t pause      = 0;
    const void*  stateObject = nullptr;
    bool ReadyToTake() const noexcept { return brain == 0 && state == kStateFollow; }
};

struct Watched
{
    const void*       character = nullptr;
    std::uint8_t      state     = kUnread;
    std::uint8_t      brain     = kUnread;
    Clock::time_point since{};
    Clock::time_point lastStill{};
    float             start[3]{};  // where the state began
    float             lastStillAt[3]{};
    bool              placed = false;
};

struct Pending
{
    void*             character = nullptr;
    std::uint64_t     weak      = 0;
    const wchar_t*    when      = nullptr;
    Clock::time_point due{};
};

struct Held
{
    const void*       character = nullptr;
    Ai                ai{};
    int               rootPercent = -1;
    int               rotationPercent = -1;
    Clock::time_point lastCheck{};
    Clock::time_point lastSpeed{};
    Clock::time_point lastSpeedLine{};
    int               speedLinePercent = -1;
    float             two[3]{};
    float             one[3]{};
    bool              placed = false;
};

Order                                      g_lastOrder{};
std::array<Watched, facts::kNoPlayerType>  g_members{};
std::array<Pending, kPending>              g_pending{};
Held                                       g_held{};
Clock::time_point                          g_lastWatch{};
std::int32_t                               g_rootOffset = -1;

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // a Debug report (platform/log.h)
    }
    try
    {
        platform::WriteLine(L"[debug] handover: " + std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
}

template <class T>
T At(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? T{} : ReadField<T>(object, offset);
}

const wchar_t* TypeName(std::uint8_t type) noexcept
{
    static constexpr const wchar_t* kNames[] = {L"Cloud",   L"Barret", L"Tifa",      L"Aerith",
                                                L"Red XIII", L"Yuffie", L"Cait Sith", L"Zack",
                                                L"Sephiroth", L"Vincent", L"Cid",   L"young Aerith"};
    return type < std::size(kNames) ? kNames[type] : L"?";
}

std::wstring StateName(std::uint8_t state)
{
    switch (state)
    {
        case kStateDoNothing: return L"DO_NOTHING";
        case 2: return L"CONNECT_ENCOUNT";
        case 3: return L"BATTLE";
        case kStateStay: return L"STAY";
        case kStateLead: return L"LEAD";
        case kStateFollow: return L"FOLLOW";
        case kUnread: return L"?";
        default: return std::format(L"state {}", state);
    }
}

// ---- the party order -------------------------------------------------------------------------

Order ReadOrder() noexcept
{
    Order order;
    std::int32_t count = 0;
    if (!platform::CallGuarded(g_fn.count, count))
    {
        return order;
    }
    order.count = std::clamp(count, 0, kMaxMembers);
    for (std::int32_t ordinal = 0; ordinal < order.count; ++ordinal)
    {
        std::uint8_t type = kUnread;
        platform::CallGuarded(g_fn.typeAt, type, ordinal);
        order.types[static_cast<std::size_t>(ordinal)] = type;
    }
    platform::CallGuarded(g_fn.fieldLeader, order.fieldLeader);
    platform::CallGuarded(g_fn.defaultLeader, order.defaultLeader);
    return order;
}

/// The 2nd slot from the field leader, stepping past player 1's character like the game's cycle
/// (RE-p2-party-order.md §5.1, §5.2). -1 if none.
std::int32_t SlotRule(const Order& order, std::optional<std::uint8_t> playerOnes, std::int32_t& anchor) noexcept
{
    anchor = 0;
    for (std::int32_t ordinal = 0; ordinal < order.count; ++ordinal)
    {
        if (order.types[static_cast<std::size_t>(ordinal)] == order.fieldLeader)
        {
            anchor = ordinal;
            break;
        }
    }
    if (order.count < 2)
    {
        return -1;
    }
    for (std::int32_t step = 1; step <= order.count; ++step)
    {
        const std::int32_t slot = (anchor + step) % order.count;
        if (!playerOnes || order.types[static_cast<std::size_t>(slot)] != *playerOnes)
        {
            return slot;
        }
    }
    return -1;
}

std::wstring OrderText(const Order& order)
{
    std::wstring text = L"[";
    for (std::int32_t ordinal = 0; ordinal < order.count; ++ordinal)
    {
        const std::uint8_t type = order.types[static_cast<std::size_t>(ordinal)];
        text += std::format(L"{}{} {}", ordinal == 0 ? L"" : L", ", type, TypeName(type));
    }
    return text + L"]";
}

std::wstring TypeText(std::optional<std::uint8_t> type)
{
    return type ? std::format(L"{} {}", *type, TypeName(*type)) : std::wstring(L"none");
}

void LogOrder(const wchar_t* why, const Order& order, const void* playerTwoMember)
{
    const auto playerOnes = party_member::PlayerTypeOf(ReadPointer(generation::PlayerOneController(), facts::kControllerPawn));
    const auto playerTwos = party_member::PlayerTypeOf(playerTwoMember);
    std::int32_t anchor = 0;
    const std::int32_t slot = SlotRule(order, playerOnes, anchor);
    const std::optional<std::uint8_t> ruled =
        slot < 0 ? std::nullopt : std::optional<std::uint8_t>(order.types[static_cast<std::size_t>(slot)]);
    // A party order change is rare and real (a reorder, a join, a hand-over): Info, not the watch's Debug lines.
    platform::Log(L"handover: party order ({}): {}, field leader {}, default battle leader {}, player 1 plays {}; the "
                  L"2nd-slot rule (anchor {}) gives {}{}; player 2 holds {}{}",
                  why, OrderText(order), order.fieldLeader, order.defaultLeader, TypeText(playerOnes), anchor,
                  slot < 0 ? std::wstring(L"nobody") : std::format(L"ordinal {} = ", slot),
                  slot < 0 ? std::wstring() : TypeText(ruled), TypeText(playerTwos),
                  playerTwoMember == nullptr ? L""
                  : ruled == playerTwos      ? L" (same)"
                                             : L" (DIFFERENT)");
}

// ---- one member --------------------------------------------------------------------------------

Ai ReadAi(const void* character) noexcept
{
    Ai ai;
    ai.controller = ReadPointer(character, facts::kCharacterRetainedAi);
    if (ai.controller == nullptr)
    {
        return ai;
    }
    const void* brain = ReadPointer(ai.controller, kAiActiveBrain);
    ai.stateObject    = ReadPointer(brain, kBrainState);
    ai.state          = ai.stateObject == nullptr ? kUnread : At<std::uint8_t>(ai.stateObject, kStateType);
    ai.brain          = brain == nullptr ? kUnread : At<std::uint8_t>(brain, kBrainIndex);
    ai.brainMode      = brain == nullptr ? kUnread : At<std::uint8_t>(brain, kBrainMode);
    ai.activeIndex    = At<std::uint8_t>(ai.controller, kAiActiveIndex);
    ai.defaultState   = At<std::uint8_t>(ai.controller, kAiDefaultState);
    ai.pause          = At<std::uint32_t>(ai.controller, kAiPauseMask);
    return ai;
}

std::wstring BrainsText(const void* controller)
{
    std::wstring text;
    for (int slot = 0; slot < kBrainSlots; ++slot)
    {
        const void* brain = ReadPointer(controller, kAiBrains + static_cast<std::uint32_t>(slot) * 8);
        if (brain == nullptr)
        {
            continue;
        }
        const void* state = ReadPointer(brain, kBrainState);
        if (state == nullptr)
        {
            continue;  // an unused brain (every slot holds one)
        }
        text += std::format(L"{}{}: mode {} {}", text.empty() ? L"" : L", ", slot, At<std::uint8_t>(brain, kBrainMode),
                            StateName(At<std::uint8_t>(state, kStateType)));
    }
    return text.empty() ? std::wstring(L"none") : text;
}

std::wstring AiText(const Ai& ai)
{
    if (ai.controller == nullptr)
    {
        return L"no retained AI";
    }
    return std::format(L"AI {} {} (active brain {} mode {}, +0x520 {}; default {}; pause mask {:#x}; ready by the take rule: "
                       L"{}); brains [{}]",
                       ai.controller, StateName(ai.state), ai.brain, ai.brainMode, ai.activeIndex,
                       StateName(ai.defaultState), ai.pause, ai.ReadyToTake() ? L"yes" : L"no", BrainsText(ai.controller));
}

/// A reflected property's offset on `object`'s class chain, found once into `cache` (-1 if not found).
std::int32_t PropertyOffset(const void* object, const wchar_t* name, std::int32_t& cache) noexcept
{
    if (cache >= 0 || object == nullptr)
    {
        return cache;
    }
    try
    {
        auto* property = static_cast<UObject*>(const_cast<void*>(object))->GetPropertyByNameInChain(name);
        if (property != nullptr && property->GetSize() == sizeof(void*))
        {
            cache = property->GetOffset_Internal();
        }
    }
    catch (...)
    {
    }
    return cache;
}

std::int32_t g_meshOffset = -1;
std::int32_t g_animOffset = -1;
bool         g_animChecked = false;

/// The character's anim instance, through the reflected ACharacter::Mesh and
/// USkeletalMeshComponent::AnimScriptInstance. Once, the anim instance's own reflected offsets are
/// compared with this build's, and the weak pointer the IDA pass named (+0x1400 +0x160) with it.
const void* AnimOf(const void* character) noexcept
{
    const std::int32_t meshAt = PropertyOffset(character, STR("Mesh"), g_meshOffset);
    const void*        mesh   = meshAt < 0 ? nullptr : ReadPointer(character, static_cast<std::uint32_t>(meshAt));
    const std::int32_t animAt = PropertyOffset(mesh, STR("AnimScriptInstance"), g_animOffset);
    const void*        anim   = animAt < 0 ? nullptr : ReadPointer(mesh, static_cast<std::uint32_t>(animAt));
    if (anim == nullptr || g_animChecked)
    {
        return anim;
    }
    g_animChecked = true;
    std::int32_t root = -1, rotation = -1;
    try
    {
        auto* object = static_cast<UObject*>(const_cast<void*>(anim));
        if (auto* p = object->GetPropertyByNameInChain(STR("RootMotionTranslationScale")))
        {
            root = p->GetOffset_Internal();
        }
        if (auto* p = object->GetPropertyByNameInChain(STR("RotationRateScale")))
        {
            rotation = p->GetOffset_Internal();
        }
    }
    catch (...)
    {
    }
    Line(L"anim instance {} through the mesh; reflected RootMotionTranslationScale at {:#x} (this build: {:#x}), "
         L"RotationRateScale at {:#x} (this build: {:#x}); the weak pointer at +0x1400 +0x160 names {}",
         anim, root, kAnimRootMotionScale, rotation, kAnimRotationScale,
         leader_scope::ObjectAt(static_cast<const std::byte*>(character) + static_cast<std::uint32_t>(kCharacterMove) + kMoveAnimInstance));
    return anim;
}

std::wstring MapText(const void* anim, std::uint32_t mapOffset)
{
    const auto*        map   = static_cast<const std::byte*>(anim) + mapOffset;
    const void*        data  = ReadPointer(map, 0);
    const std::int32_t count = At<std::int32_t>(map, kMapCount);
    if (data == nullptr || count <= 0)
    {
        return L"{}";
    }
    if (count > kMapShown)
    {
        return std::format(L"{{{} slots}}", count);
    }
    const void*         heapBits = ReadPointer(map, kMapHeapBits);
    const std::uint32_t bits     = heapBits != nullptr ? At<std::uint32_t>(heapBits, 0) : At<std::uint32_t>(map, kMapInlineBits);
    std::wstring        text     = L"{";
    for (std::int32_t index = 0; index < count; ++index)
    {
        if ((bits & (1u << index)) == 0)
        {
            continue;  // a free slot
        }
        const auto* element = static_cast<const std::byte*>(data) + static_cast<std::size_t>(index) * kMapElement;
        text += std::format(L"{}{}: {:.2f}", text.size() == 1 ? L"" : L", ", At<std::uint8_t>(element, 0),
                            At<float>(element, 4));
    }
    return text + L"}";
}

std::wstring ModifiersText(const void* character)
{
    const void* anim = AnimOf(character);
    if (anim == nullptr)
    {
        return L"no anim instance";
    }
    return std::format(L"root-motion scale {:.2f} {} (6 = FieldAI), rotation-rate scale {:.2f} {} (0 = Field), +0x10CC "
                       L"{:.2f}, anim flags +0x11DC {:#x}, character flags +0x1408 {:#04x}",
                       At<float>(anim, kAnimRootMotionScale), MapText(anim, kAnimRootMotionMap),
                       At<float>(anim, kAnimRotationScale), MapText(anim, kAnimRotationMap),
                       At<float>(anim, kAnimSpeedScale), At<std::uint32_t>(anim, kAnimFlags),
                       At<std::uint8_t>(character, kCharacterFlags));
}

std::wstring StateFieldsText(const Ai& ai)
{
    if (ai.state == kStateFollow)
    {
        return std::format(L"FOLLOW rate {:.2f}, follow mode {}", At<float>(ai.stateObject, kFollowRate),
                           At<std::uint8_t>(ai.stateObject, kFollowMode));
    }
    if (ai.state == kStateLead)
    {
        const auto* state = static_cast<const std::byte*>(ai.stateObject);
        return std::format(L"LEAD arrived {}, destination ({:.0f}, {:.0f}, {:.0f}), target {}",
                           At<std::uint8_t>(state, kLeadArrived), At<float>(state, kLeadDestination),
                           At<float>(state, kLeadDestination + 4), At<float>(state, kLeadDestination + 8),
                           leader_scope::ObjectAt(state == nullptr ? nullptr : state + kLeadTarget));
    }
    return L"no catch-up fields";
}

int Percent(float value) noexcept
{
    return static_cast<int>(std::lround(value * 100.0f));
}

const std::byte* RootOf(const void* character) noexcept
{
    if (character == nullptr)
    {
        return nullptr;
    }
    if (g_rootOffset < 0)
    {
        try
        {
            auto* property =
                static_cast<UObject*>(const_cast<void*>(character))->GetPropertyByNameInChain(STR("RootComponent"));
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
    return static_cast<const std::byte*>(ReadPointer(character, static_cast<std::uint32_t>(g_rootOffset)));
}

bool LocationOf(const void* character, float (&out)[3]) noexcept
{
    const std::byte* root = RootOf(character);
    return root != nullptr && platform::CopyGuarded(out, root + kComponentWorldLocation, sizeof(out));
}

float Apart(const float (&a)[3], const float (&b)[3]) noexcept
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

std::wstring NameOf(const void* character)
{
    const auto type = party_member::PlayerTypeOf(character);
    return type ? std::wstring(TypeName(*type)) : std::format(L"{}", character);
}

void LogSnapshot(const wchar_t* when, const void* character)
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // snapshots are Debug: nothing is read below that level
    }
    const Ai ai = ReadAi(character);
    Line(L"{}: {}'s {}", when, NameOf(character), AiText(ai));
    Line(L"  {}; {}", ModifiersText(character), StateFieldsText(ai));
}

// ---- the watch ---------------------------------------------------------------------------------

std::wstring Whose(const void* character)
{
    if (character != nullptr && character == generation::Member())
    {
        return L" (player 2's)";
    }
    if (character != nullptr && character == ReadPointer(generation::PlayerOneController(), facts::kControllerPawn))
    {
        return L" (player 1's)";
    }
    return {};
}

void WatchMember(std::uint8_t type, const Clock::time_point now)
{
    void* character = nullptr;
    if (!platform::CallGuarded(g_fn.characterByType, character, facts::kFieldLane, type) || character == nullptr)
    {
        return;
    }
    Watched& seen = g_members[type];
    const Ai ai   = ReadAi(character);
    float    at[3]{};
    const bool placed = LocationOf(character, at);
    if (character != seen.character || ai.state != seen.state || ai.brain != seen.brain)
    {
        if (seen.character == character && seen.state != kUnread)
        {
            const float moved = seen.placed && placed ? Apart(at, seen.start) : -1.0f;
            Line(L"{}{}: AI {} (brain {}) -> {} (brain {}) after {:.1f} s, moved {:.0f} cm during it; now at ({:.0f}, "
                 L"{:.0f}, {:.0f}), ready by the take rule: {}",
                 TypeName(type), Whose(character), StateName(seen.state), seen.brain, StateName(ai.state), ai.brain,
                 std::chrono::duration<float>(now - seen.since).count(), moved, at[0], at[1], at[2],
                 ai.ReadyToTake() ? L"yes" : L"no");
        }
        else
        {
            Line(L"{}{}: AI {} (brain {}), at ({:.0f}, {:.0f}, {:.0f}), ready by the take rule: {}", TypeName(type),
                 Whose(character), StateName(ai.state), ai.brain, at[0], at[1], at[2], ai.ReadyToTake() ? L"yes" : L"no");
        }
        seen.character = character;
        seen.state     = ai.state;
        seen.brain     = ai.brain;
        seen.since     = now;
        seen.lastStill = now;
        seen.placed    = placed;
        std::copy(std::begin(at), std::end(at), seen.start);
        std::copy(std::begin(at), std::end(at), seen.lastStillAt);
        return;
    }
    // A member out of FOLLOW: does anything move it (a script) while the AI does not? (BATTLE moves on its own.)
    if (ai.state == kStateFollow || ai.state == kStateBattle || !placed || !seen.placed || now - seen.lastStill < kStillThereEvery)
    {
        return;
    }
    const float moved = Apart(at, seen.lastStillAt);
    if (moved >= kMovedCm)
    {
        Line(L"{}{}: still {} (brain {}) after {:.1f} s and moving: {:.0f} cm in the last {:.1f} s",
             TypeName(type), Whose(character), StateName(ai.state), ai.brain,
             std::chrono::duration<float>(now - seen.since).count(), moved,
             std::chrono::duration<float>(now - seen.lastStill).count());
    }
    seen.lastStill = now;
    std::copy(std::begin(at), std::end(at), seen.lastStillAt);
}

/// Player 2's member while held: its AI and modifiers on change, and its speed beside player 1's.
void WatchHeld(const Clock::time_point now)
{
    const void* character = generation::Member();
    if (character != g_held.character)
    {
        g_held           = {};
        g_held.character = character;
    }
    if (character == nullptr)
    {
        return;
    }
    const void* anim = AnimOf(character);
    if (now - g_held.lastCheck >= kHeldEvery)
    {
        g_held.lastCheck  = now;
        const Ai ai       = ReadAi(character);
        const int root    = anim == nullptr ? -1 : Percent(At<float>(anim, kAnimRootMotionScale));
        const int rotation = anim == nullptr ? -1 : Percent(At<float>(anim, kAnimRotationScale));
        if (ai.state != g_held.ai.state || ai.brain != g_held.ai.brain || ai.pause != g_held.ai.pause ||
            root != g_held.rootPercent || rotation != g_held.rotationPercent)
        {
            LogSnapshot(L"player 2's member changed while held", character);
            g_held.ai              = ai;
            g_held.rootPercent     = root;
            g_held.rotationPercent = rotation;
        }
    }
    if (now - g_held.lastSpeed < kSpeedEvery)
    {
        return;
    }
    float two[3]{}, one[3]{};
    const bool placed = LocationOf(character, two) &&
                        LocationOf(ReadPointer(generation::PlayerOneController(), facts::kControllerPawn), one);
    if (placed && g_held.placed)
    {
        const float seconds  = std::chrono::duration<float>(now - g_held.lastSpeed).count();
        const float twoSpeed = Apart(two, g_held.two) / seconds;
        const float oneSpeed = Apart(one, g_held.one) / seconds;
        const int   root     = anim == nullptr ? -1 : Percent(At<float>(anim, kAnimRootMotionScale));
        if (twoSpeed >= kMovingCmPerSecond &&
            (now - g_held.lastSpeedLine >= kSpeedLineEvery || root != g_held.speedLinePercent))
        {
            g_held.lastSpeedLine    = now;
            g_held.speedLinePercent = root;
            Line(L"speed: player 2's {} {:.0f} cm/s, player 1's {:.0f} cm/s; player 2's root-motion scale {}",
                 NameOf(character), twoSpeed, oneSpeed,
                 root < 0 ? std::wstring(L"unread") : std::format(L"{:.2f}", root / 100.0f));
        }
    }
    g_held.lastSpeed = now;
    g_held.placed    = placed;
    std::copy(std::begin(two), std::end(two), g_held.two);
    std::copy(std::begin(one), std::end(one), g_held.one);
}

void RunPending(const Clock::time_point now)
{
    for (Pending& pending : g_pending)
    {
        if (pending.character == nullptr || now < pending.due)
        {
            continue;
        }
        if (leader_scope::StillNames(pending.weak, pending.character))
        {
            try
            {
                LogSnapshot(std::format(L"1 s after {}", pending.when).c_str(), pending.character);
            }
            catch (...)
            {
            }
        }
        pending = {};
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact, auto& out) {
        const auto live = platform::FindFunction(image, fact);
        out             = live ? reinterpret_cast<std::remove_reference_t<decltype(out)>>(*live) : nullptr;
        return live.has_value();
    };
    bool found = find(facts::kRosterCount, g_fn.count);
    found      = find(facts::kRosterPlayerType, g_fn.typeAt) && found;
    found      = find(facts::kFieldLeaderType, g_fn.fieldLeader) && found;
    found      = find(facts::kDefaultBattleLeaderType, g_fn.defaultLeader) && found;
    found      = find(facts::kCharacterByPlayerType, g_fn.characterByType) && found;
    g_installed = found;
    platform::Log(L"handover watch: {}", found ? L"logging the party order, the members' AI states and player 2's hand-overs (read-only)"
                                               : L"UNAVAILABLE (a check failed); the party order and hand-overs are not logged");
    return found;
}

void PartyOrder(const wchar_t* why, const void* playerTwoMember) noexcept
{
    if (!g_installed || !party_member::GameReady())
    {
        return;
    }
    try
    {
        g_lastOrder = ReadOrder();
        LogOrder(why, g_lastOrder, playerTwoMember);
    }
    catch (...)
    {
    }
}

void Snapshot(const wchar_t* when, void* member) noexcept
{
    if (!g_installed || member == nullptr || !platform::Enabled(platform::Level::Debug))
    {
        return;  // the snapshot and the one 1 s later are Debug
    }
    try
    {
        LogSnapshot(when, member);
    }
    catch (...)
    {
    }
    for (Pending& pending : g_pending)
    {
        if (pending.character == nullptr)
        {
            pending = {member, leader_scope::WeakOf(member), when, Clock::now() + kAgain};
            return;
        }
    }
}

void Tick() noexcept
{
    if (!g_installed)
    {
        return;
    }
    if (!generation::Active() || !party_member::GameReady())
    {
        g_members = {};
        g_held    = {};
        return;
    }
    const auto now = Clock::now();
    // The members' AI, the held member and the pending snapshots are Debug; the party order's line is Info.
    const bool debug = platform::Enabled(platform::Level::Debug);
    if (!debug)
    {
        g_members = {};
        g_held    = {};
        g_pending = {};
    }
    try
    {
        if (debug)
        {
            RunPending(now);
            WatchHeld(now);
        }
        if (now - g_lastWatch < kWatchEvery)
        {
            return;
        }
        g_lastWatch       = now;
        const Order order = ReadOrder();
        if (order != g_lastOrder)
        {
            g_lastOrder = order;
            LogOrder(L"the watch saw it change", order, generation::Member());
        }
        for (std::int32_t ordinal = 0; debug && ordinal < order.count; ++ordinal)
        {
            const std::uint8_t type = order.types[static_cast<std::size_t>(ordinal)];
            if (type < facts::kNoPlayerType)
            {
                WatchMember(type, now);
            }
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::handover_watch
