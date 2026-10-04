#include "player_two/charapop_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/engine_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::player_two::charapop_watch {

namespace {

using RC::Unreal::UObject;
using RC::Unreal::UStruct;
using Clock = std::chrono::steady_clock;

// The manager, entries and camera pose are read as player_two/charapop_facts.h names them; these are the
// watch's own extra fields (RE-p2-charapop-per-view.md §1.1, §1.3; this build).
using namespace charapop;
constexpr std::uint32_t  kManagerParams  = 0x1148;  // through +0x118C (RE-p2-lod-draw-distance.md §1)
constexpr std::uint32_t  kParamsSize     = 0x1190 - kManagerParams;
constexpr std::uint32_t  kPopForceAll    = 0x619C;
constexpr std::uint32_t  kPopQueueCount  = 0x6228;
constexpr std::uint32_t  kPopCursor      = 0x6230;
constexpr std::uint32_t  kEntryPauseFlag = 0x110;  // u32, cleared on unpause
constexpr std::uint32_t  kEntryPauseWant = 0x114;  // u8, set on a pause request
constexpr std::uint32_t  kEntryRequested = 0x1B0;  // i32
constexpr std::uint32_t  kEntryLock      = 0x1B4;  // u8
// The population pass's fields (RE-p2-charapop-spawn-gate.md §2, §4).
constexpr std::uint32_t  kEntryUseCount    = 0x40;   // i32: 0 -> despawn
constexpr std::uint32_t  kEntryScore       = 0x44;   // i32: below the threshold -> despawn
constexpr std::uint32_t  kEntryFlags       = 0x118;  // u32: 1 managed, 2 placed, 0x20 no despawn fade, 0x100 important
constexpr std::uint32_t  kEntryKeep        = 0x11C;  // u32: keep reasons (no despawn)
constexpr std::uint32_t  kEntrySpawnHidden = 0x1A2;  // u8: over the visible budget
constexpr std::uint32_t  kOverrideOn       = 0x119C;  // u8, M
constexpr std::uint32_t  kOverrideCentre   = 0x11A0;  // FVector, M
constexpr std::uint32_t  kLeaderOn         = 0x11BE;  // u8, M
constexpr std::uint32_t  kLeaderCentre     = 0x11B0;  // FVector, M
constexpr std::uint32_t  kManagerSelect    = 0x18;    // M: pointer; the threshold at +0xE8 (i32)
constexpr std::uint32_t  kThreshold        = 0xE8;

// The SE hidden-reason fade (spawn-gate doc §3.1, §4): CharaPopEntry_GetEndActorInterface(entry) (a pure
// lookup, like the base-interface getter), then the interface's getter at slot +0x1D0 (may return null; its
// creator at +0x1D8 is never called). Entry: as the base-interface getter's, then call __alloca_probe; sub rsp,rax.
constexpr std::uint8_t kActorInterfaceOfBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0xB8,
                                                   0x80, 0x00, 0x00, 0x00, 0xE8, 0x4B, 0xA5, 0x7F, 0x01, 0x48, 0x2B, 0xE0};
constexpr platform::FunctionFact kActorInterfaceOf{L"CharaPopEntry_GetEndActorInterface", 0x1408B49D0, kActorInterfaceOfBytes};
using FadeOfFn = void* (*)(void* iface);
constexpr std::uint32_t kFadeSlot    = 0x1D0;
constexpr std::uint32_t kFadeHidden  = 0x88;  // u8
constexpr std::uint32_t kFadeOpacity = 0x8C;  // float, now
constexpr std::uint32_t kFadeTarget  = 0x90;  // float
constexpr std::uint32_t kFadeReasons = 0xA4;  // u32: bit 0 CharaPop or the wild-animal AI

// Stock visibility (spawn-gate doc §4; offsets of this build).
constexpr std::uint32_t kActorHiddenByte  = 0x58;   // AActor::bHidden, bit 0x20
constexpr std::uint8_t  kActorHiddenBit   = 0x20;
constexpr std::uint32_t kCompVisibleByte  = 0x14C;  // bVisible, bit 0x20
constexpr std::uint8_t  kCompVisibleBit   = 0x20;
constexpr std::uint32_t kCompHiddenByte   = 0x14D;  // bHiddenInGame, bit 0x04
constexpr std::uint8_t  kCompHiddenBit    = 0x04;
constexpr std::uint32_t kCompLastRender   = 0x2A4;  // float: LastRenderTime, written by any view that drew it

// The actor's paused state as the evaluators read it (§7): CharaPopEntry_GetEndActorBaseInterface(entry)
// resolves the entry's actor and returns its EndActorBaseInterface (null if none), a pure lookup; the
// evaluators then call the interface's slot +0x30 with reason 6. Entry: mov [rsp+8],rbx; mov [rsp+10h],rsi;
// push rdi; mov eax,80h.
constexpr std::uint8_t kInterfaceOfBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x57, 0xB8, 0x80, 0x00, 0x00, 0x00};
constexpr platform::FunctionFact kInterfaceOf{L"CharaPopEntry_GetEndActorBaseInterface", 0x1408B4ABC, kInterfaceOfBytes};
using InterfaceOfFn = void* (*)(const void* entry);
using PausedFn      = std::uint8_t (*)(void* iface, std::uint8_t reason);
constexpr std::uint32_t kPausedSlot  = 0x30;
constexpr std::uint8_t  kPauseReason = 6;
constexpr std::uint8_t  kUnread      = 0xFF;

// The animation world manager W = [0x14939BC48] and its camera snapshot (§2.1).
constexpr std::uintptr_t kAnimWorld         = 0x14939BC48;
constexpr std::uint32_t  kSnapshotLocation  = 0x8C0;
constexpr std::uint32_t  kSnapshotSpeed     = 0x908;  // float
constexpr std::uint32_t  kSnapshotTurn      = 0x90C;  // float
constexpr std::uint32_t  kSnapshotFaster    = 0x910;  // two u8
constexpr std::uint32_t  kSnapshotValid     = 0x913;  // u8

// UEndSkeletalMeshComponent's optimisation fields (§2.2).
constexpr std::uint32_t kCompLevelOn    = 0x121C;  // u8: reads the owner's CharaPop level
constexpr std::uint32_t kCompForced     = 0x1220;  // u8
constexpr std::uint32_t kCompShadow     = 0x1224;  // u8: cast shadow
constexpr std::uint32_t kCompPauseLevel = 0x1226;  // u8
constexpr std::uint32_t kCompLevel      = 0x1228;  // u8: the owner's CharaPop level, copied
constexpr std::uint32_t kCompDistance   = 0x1234;  // float, to the snapshot camera
constexpr std::uint32_t kCompAngle      = 0x1238;  // float, radians

constexpr std::uint32_t kSampleEvery   = 10;      // ticks
constexpr float         kFarFromOneCm  = 5000.0f;  // only entries more than 50 m from player 1's camera
constexpr int           kWatched       = 16;
constexpr int           kRemembered    = 64;      // entries whose last logged state is kept
constexpr int           kMaxLines      = 2000;
constexpr std::uint32_t kUndrawnSamples = 3;     // a mesh counts as not drawn after this many still samples
constexpr std::uint32_t kSettleSamples  = 3;     // an entry's change is logged once it has held this many samples
constexpr auto          kSummaryEvery  = std::chrono::seconds(60);
constexpr auto          kEntryLineGap  = std::chrono::seconds(60);  // the shortest gap between one entry's change lines

/// What a line about an entry is logged on.
struct EntrySteady
{
    std::uint8_t  level = 0, minLevel = 0, lock = 0, pauseWant = 0;
    std::int32_t  state = 0, requested = 0;
    std::uint32_t pauseFlag = 0;
    std::uint8_t  paused = kUnread;  // the actor's own state (reason 6)
    bool          meshRead = false;
    std::uint8_t  shadow = 0, pauseLevel = 0, forced = 0, meshLevel = 0, levelOn = 0;
    std::uint32_t flags = 0, keep = 0;
    std::uint8_t  spawnHidden = 0;
    bool          fadeRead = false, fadeHidden = false;
    std::uint32_t fadeReasons = 0;
    float         fadeTarget = 0.0f;
    bool          actorHidden = false, meshVisible = false, meshHiddenInGame = false;
    bool          drawn = false;  // the mesh's last render time moved within the last kUndrawnSamples samples
    bool operator==(const EntrySteady&) const = default;
};

struct Sample
{
    const void*   entry = nullptr;
    void*         actor = nullptr;
    float         d1 = 0.0f, d2 = 0.0f;  // cm, to each camera
    float         a1 = 0.0f, a2 = 0.0f;  // degrees off each camera's forward
    EntrySteady   steady{};
    float         meshDistance = 0.0f, meshAngle = 0.0f;
    float         lastRender = 0.0f, opacity = 0.0f;
    std::int32_t  score = 0, useCount = 0;
};

struct Remembered
{
    const void*   entry = nullptr;
    EntrySteady   steady{};
    std::uint32_t lastSeen = 0;  // sample number
    float         lastRender = 0.0f;
    std::uint32_t undrawn  = 0;  // samples in a row the last render time stood still
    EntrySteady   pending{};     // a change not logged yet ...
    std::uint32_t pendingSeen = 0;  // ... and how many samples in a row it has held
    Clock::time_point lastLine{};   // this entry's last line
};

/// The summary line's parts logged on change (the counters, count and cameras move: they come with each line).
struct SummarySteady
{
    std::array<std::byte, kParamsSize> params{};
    bool operator==(const SummarySteady&) const = default;
};

std::uintptr_t g_gameRoot  = 0;  // set once at install
std::uintptr_t g_animWorld = 0;
InterfaceOfFn  g_interfaceOf = nullptr;  // null: the paused state is not read
InterfaceOfFn  g_actorInterfaceOf = nullptr;  // null: the fade is not read
bool           g_installed = false;

// Game thread.
std::uint32_t                         g_ticks   = 0;
std::uint32_t                         g_samples = 0;
int                                   g_lines   = 0;
std::array<Remembered, kRemembered>   g_remembered{};
SummarySteady                         g_summary{};
Clock::time_point                     g_lastSummary{};
bool                                  g_wasShown = false;

/// Per class: the reflected Mesh offset (-1: none) and whether that mesh is an EndSkeletalMeshComponent.
struct ClassFacts
{
    const void*  cls = nullptr;
    std::int32_t meshOffset = -1;
};
std::array<ClassFacts, 32> g_classes{};
std::uint32_t              g_nextClass = 0;
struct MeshClass
{
    const void* cls = nullptr;
    bool        end = false;
};
std::array<MeshClass, 16> g_meshClasses{};
std::uint32_t             g_nextMeshClass = 0;

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    if (g_lines >= kMaxLines)
    {
        return;
    }
    ++g_lines;
    try
    {
        platform::WriteLine(L"charapop: " + std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
    if (g_lines == kMaxLines)
    {
        platform::WriteLine(L"charapop: line limit reached; the watch is quiet from here on");
    }
}

template <class T>
T At(const void* object, std::uint32_t offset) noexcept
{
    return object == nullptr ? T{} : ReadField<T>(object, offset);
}

/// The actor's own paused state for the evaluators' reason 6: the game's interface getter, then the call the
/// evaluators make (`iface vt+0x30(iface, 6)`), both guarded (RE-p2-charapop-per-view.md §7). 0xFF if unread.
std::uint8_t PausedOf(const void* entry) noexcept
{
    void* iface = nullptr;
    if (g_interfaceOf == nullptr || !platform::CallGuarded(g_interfaceOf, iface, entry) || iface == nullptr)
    {
        return kUnread;
    }
    const auto query = reinterpret_cast<PausedFn>(ReadPointer(ReadPointer(iface, 0), kPausedSlot));
    std::uint8_t paused = kUnread;
    return query != nullptr && platform::CallGuarded(query, paused, iface, kPauseReason) ? static_cast<std::uint8_t>(paused != 0)
                                                                                       : kUnread;
}

/// The actor's SE fade object (the game's interface getter, then the interface's fade getter; both guarded),
/// or null when it has none.
const void* FadeOf(const void* entry) noexcept
{
    void* iface = nullptr;
    if (g_actorInterfaceOf == nullptr || !platform::CallGuarded(g_actorInterfaceOf, iface, entry) || iface == nullptr)
    {
        return nullptr;
    }
    const auto getter = reinterpret_cast<FadeOfFn>(ReadPointer(ReadPointer(iface, 0), kFadeSlot));
    void*      fade   = nullptr;
    return getter != nullptr && platform::CallGuarded(getter, fade, iface) ? fade : nullptr;
}

/// Whether `cls` is UEndSkeletalMeshComponent or derives from it (cached per class).
bool IsEndMeshClass(const void* cls)
{
    for (const MeshClass& known : g_meshClasses)
    {
        if (known.cls == cls)
        {
            return known.end;
        }
    }
    bool end = false;
    for (auto* s = static_cast<UStruct*>(const_cast<void*>(cls)); s != nullptr; s = s->GetSuperStruct())
    {
        if (s->GetName() == STR("EndSkeletalMeshComponent"))
        {
            end = true;
            break;
        }
    }
    g_meshClasses[g_nextMeshClass++ % g_meshClasses.size()] = {cls, end};
    return end;
}

/// The actor's reflected Mesh, if it is an EndSkeletalMeshComponent (the component the optimisation reads).
const void* EndMeshOf(void* actor)
{
    auto* object = static_cast<UObject*>(actor);
    const void* cls = object->GetClassPrivate();
    ClassFacts* facts = nullptr;
    for (ClassFacts& known : g_classes)
    {
        if (known.cls == cls)
        {
            facts = &known;
            break;
        }
    }
    if (facts == nullptr)
    {
        auto*        property = object->GetPropertyByNameInChain(STR("Mesh"));
        const auto   offset   = property != nullptr && property->GetSize() == sizeof(void*) ? property->GetOffset_Internal() : -1;
        facts                 = &g_classes[g_nextClass++ % g_classes.size()];
        *facts                = {cls, offset};
    }
    if (facts->meshOffset < 0)
    {
        return nullptr;
    }
    // The field's object is confirmed live by the game's own weak pointer (both calls guarded) before
    // reflection reads it.
    void* mesh = ReadPointer(actor, static_cast<std::uint32_t>(facts->meshOffset));
    if (mesh == nullptr || !leader_scope::StillNames(leader_scope::WeakOf(mesh), mesh))
    {
        return nullptr;
    }
    return IsEndMeshClass(static_cast<UObject*>(mesh)->GetClassPrivate()) ? mesh : nullptr;
}

void ReadEntry(Sample& sample)
{
    const void* e          = sample.entry;
    EntrySteady& s         = sample.steady;
    s.level                = At<std::uint8_t>(e, kEntryLevel);
    s.minLevel             = At<std::uint8_t>(e, kEntryMinLevel);
    s.lock                 = At<std::uint8_t>(e, kEntryLock);
    s.pauseWant            = At<std::uint8_t>(e, kEntryPauseWant);
    s.state                = At<std::int32_t>(e, kEntryState);
    s.requested            = At<std::int32_t>(e, kEntryRequested);
    s.pauseFlag            = At<std::uint32_t>(e, kEntryPauseFlag);
    // Asked only in states 5 and 7, where the evaluators ask it themselves (§1.3, §7.4).
    s.paused               = s.state == 5 || s.state == 7 ? PausedOf(e) : kUnread;
    const void* mesh       = EndMeshOf(sample.actor);
    s.meshRead             = mesh != nullptr;
    if (mesh != nullptr)
    {
        s.levelOn           = At<std::uint8_t>(mesh, kCompLevelOn);
        s.forced            = At<std::uint8_t>(mesh, kCompForced);
        s.shadow            = At<std::uint8_t>(mesh, kCompShadow);
        s.pauseLevel        = At<std::uint8_t>(mesh, kCompPauseLevel);
        s.meshLevel         = At<std::uint8_t>(mesh, kCompLevel);
        sample.meshDistance = At<float>(mesh, kCompDistance);
        sample.meshAngle    = At<float>(mesh, kCompAngle) * 180.0f / 3.14159265f;
        s.meshVisible       = (At<std::uint8_t>(mesh, kCompVisibleByte) & kCompVisibleBit) != 0;
        s.meshHiddenInGame  = (At<std::uint8_t>(mesh, kCompHiddenByte) & kCompHiddenBit) != 0;
        sample.lastRender   = At<float>(mesh, kCompLastRender);
    }
    s.flags           = At<std::uint32_t>(e, kEntryFlags);
    s.keep            = At<std::uint32_t>(e, kEntryKeep);
    s.spawnHidden     = At<std::uint8_t>(e, kEntrySpawnHidden);
    sample.score      = At<std::int32_t>(e, kEntryScore);
    sample.useCount   = At<std::int32_t>(e, kEntryUseCount);
    s.actorHidden     = (At<std::uint8_t>(sample.actor, kActorHiddenByte) & kActorHiddenBit) != 0;
    const void* fade  = FadeOf(e);
    s.fadeRead        = fade != nullptr;
    if (fade != nullptr)
    {
        s.fadeHidden   = At<std::uint8_t>(fade, kFadeHidden) != 0;
        s.fadeReasons  = At<std::uint32_t>(fade, kFadeReasons);
        s.fadeTarget   = At<float>(fade, kFadeTarget);
        sample.opacity = At<float>(fade, kFadeOpacity);
    }
}

void LogSample(const Sample& sample, bool first)
{
    const EntrySteady& s = sample.steady;
    const std::wstring mesh =
        s.meshRead ? std::format(L"mesh: shadow {} pause level {} forced {} level {} (reads it {}) distance {:.0f} angle {:.0f}",
                                 s.shadow, s.pauseLevel, s.forced, s.meshLevel, s.levelOn, sample.meshDistance, sample.meshAngle)
                   : std::wstring(L"mesh: none read");
    const std::wstring shown =
        std::format(L"actor hidden {} mesh visible {} hidden in game {} drawn {} | fade {} | spawn: score {} use {} keep {:#x} flags {:#x} "
                    L"spawn hidden {}",
                    s.actorHidden ? 1 : 0, s.meshVisible ? 1 : 0, s.meshHiddenInGame ? 1 : 0, s.drawn ? 1 : 0,
                    s.fadeRead ? std::format(L"reasons {:#x} hidden {} opacity {:.2f} -> {:.2f}", s.fadeReasons, s.fadeHidden ? 1 : 0,
                                             sample.opacity, s.fadeTarget)
                               : std::wstring(L"none"),
                    sample.score, sample.useCount, s.keep, s.flags, s.spawnHidden);
    Line(L"{} {:#x}{}: player 1 {:.0f} m at {:.0f} deg, player 2 {:.0f} m at {:.0f} deg | level {} (min {}) state {} "
         L"(requested {}, lock {}) pause flag {} want {} paused {} | {} | {}",
         first ? L"watching" : L"changed", reinterpret_cast<std::uintptr_t>(sample.entry),
         first ? std::format(L" ({}, id {}, category {}, key {:#x})", NameOf(sample.actor), At<std::int32_t>(sample.entry, kEntryId),
                             At<std::uint8_t>(sample.entry, kEntryCategory), At<std::uint64_t>(sample.entry, kEntryKey))
               : std::wstring(),
         sample.d1 / 100.0f, sample.a1, sample.d2 / 100.0f, sample.a2, s.level, s.minLevel, s.state, s.requested, s.lock,
         s.pauseFlag, s.pauseWant, s.paused == kUnread ? std::wstring(L"?") : std::to_wstring(s.paused), mesh, shown);
}

/// Logs the sample on its first sighting and on each change of its steady parts.
void Remember(Sample& sample)
{
    Remembered* slot   = nullptr;
    Remembered* oldest = &g_remembered[0];
    for (Remembered& known : g_remembered)
    {
        if (known.entry == sample.entry)
        {
            slot = &known;
            break;
        }
        if (known.lastSeen < oldest->lastSeen)
        {
            oldest = &known;
        }
    }
    const bool first = slot == nullptr;
    const auto now   = Clock::now();
    if (first)
    {
        slot           = oldest;
        *slot          = {sample.entry, sample.steady, g_samples, sample.lastRender};
        slot->lastLine = now;
        LogSample(sample, true);
        return;
    }
    slot->lastSeen       = g_samples;
    if (sample.steady.meshRead)
    {
        slot->undrawn    = sample.lastRender != slot->lastRender ? 0 : slot->undrawn + 1;
        slot->lastRender = sample.lastRender;
    }
    sample.steady.drawn  = sample.steady.meshRead && slot->undrawn < kUndrawnSamples;
    // A change is logged once it has held for kSettleSamples samples: the mesh's pause level and the pause
    // request flip every sample on some entries, and logging each flip spent the line budget (live 2026-10-02).
    if (slot->steady == sample.steady)
    {
        slot->pendingSeen = 0;
        return;
    }
    slot->pendingSeen = slot->pendingSeen > 0 && slot->pending == sample.steady ? slot->pendingSeen + 1 : 1;
    slot->pending     = sample.steady;
    // And at most once a minute per entry, with the state it holds then: entries near the cameras' reach edges
    // change for minutes on end (hundreds of lines per session, live 2026-10-03). A change undone in between is
    // not logged.
    if (slot->pendingSeen >= kSettleSamples && now - slot->lastLine >= kEntryLineGap)
    {
        slot->steady      = sample.steady;
        slot->pendingSeen = 0;
        slot->lastLine    = now;
        LogSample(sample, false);
    }
}

/// Which centre the population follows, and its distance to each camera.
std::wstring CentreText(const std::byte* manager, const Camera& one, const Camera& two)
{
    const bool overridden = At<std::uint8_t>(manager, kOverrideOn) != 0;
    if (!overridden && At<std::uint8_t>(manager, kLeaderOn) == 0)
    {
        return L"none";
    }
    float centre[3]{};
    platform::CopyGuarded(centre, manager + (overridden ? kOverrideCentre : kLeaderCentre), sizeof(centre));
    return std::format(L"{} ({:.0f} m from player 1's camera, {:.0f} from player 2's)", overridden ? L"override" : L"leader",
                       Distance(centre, one.location) / 100.0f, Distance(centre, two.location) / 100.0f);
}

void LogSummary(const std::byte* manager, const std::byte* pop, const Camera& one, const Camera& two, std::int32_t count)
{
    SummarySteady steady;
    platform::CopyGuarded(steady.params.data(), manager + kManagerParams, steady.params.size());
    const auto now   = Clock::now();
    if (steady == g_summary && now - g_lastSummary < kSummaryEvery)
    {
        return;
    }
    g_summary     = steady;
    g_lastSummary = now;
    const auto f  = [&steady](std::uint32_t offset) {
        float value = 0.0f;
        std::memcpy(&value, steady.params.data() + (offset - kManagerParams), sizeof(value));
        return value;
    };
    const auto b = [&steady](std::uint32_t offset) { return static_cast<int>(steady.params[offset - kManagerParams]); };
    std::int32_t split = 0;
    std::memcpy(&split, steady.params.data() + (0x116C - kManagerParams), sizeof(split));
    float snapshot[3]{};
    const auto* world = static_cast<const std::byte*>(ReadPointer(reinterpret_cast<const void*>(g_animWorld), 0));
    const bool  shot  = world != nullptr && At<std::uint8_t>(world, kSnapshotValid) != 0 &&
                      platform::CopyGuarded(snapshot, world + kSnapshotLocation, sizeof(snapshot));
    Line(L"{} entries (cursor {}, force-all {}, queued {}); cameras {:.0f} m apart; anim snapshot {} ({:.0f} m from "
         L"player 1's camera, {:.0f} from player 2's; speed {:.0f} turn {:.2f} faster {}/{}) | params: FOV {:.0f}, Light draw "
         L"{:.0f} in {:.0f} out {:.0f} tick {:.0f}, Middle draw {:.0f} in {:.0f} out {:.0f} tick {:.0f}, split {}, enemy +{:.0f} "
         L"audience +{:.0f}; overrides draw {:.0f} ({}) in {:.0f} ({}) out {:.0f} ({}) | population centre: {}; threshold {}",
         count, At<std::int32_t>(pop, kPopCursor), At<std::int32_t>(pop, kPopForceAll), At<std::int32_t>(pop, kPopQueueCount),
         Distance(one.location, two.location) / 100.0f, shot ? L"read" : L"not read", Distance(snapshot, one.location) / 100.0f,
         Distance(snapshot, two.location) / 100.0f, At<float>(world, kSnapshotSpeed), At<float>(world, kSnapshotTurn),
         At<std::uint8_t>(world, kSnapshotFaster), At<std::uint8_t>(world, kSnapshotFaster + 1), f(0x1148), f(0x114C),
         f(0x1150), f(0x1154), f(0x1158), f(0x115C), f(0x1160), f(0x1164), f(0x1168), split, f(0x1170), f(0x1174), f(0x1178),
         b(0x117C), f(0x1180), b(0x1184), f(0x1188), b(0x118C), CentreText(manager, one, two),
         ReadField<std::int32_t>(ReadPointer(manager, kManagerSelect), kThreshold));
}

void SampleNow()
{
    const Objects objects = ObjectsOf(g_gameRoot);
    if (objects.pop == nullptr)
    {
        return;  // no CharaPop in this map (or not the shape the research read)
    }
    const Camera one = CameraOf(generation::PlayerOneController());
    const Camera two = CameraOf(generation::PlayerTwoController());
    if (!one.read || !two.read)
    {
        return;
    }
    std::int32_t     count = 0;
    const std::byte* list  = EntryList(objects.pop, count);
    LogSummary(objects.manager, objects.pop, one, two, count);

    std::array<Sample, kWatched> nearest{};
    int                          kept = 0;
    for (std::int32_t i = 0; i < count; ++i)
    {
        const void* entry = ReadPointer(list, static_cast<std::uint32_t>(i) * sizeof(void*));
        void*       actor = entry != nullptr ? leader_scope::ObjectAt(static_cast<const std::byte*>(entry) + kEntryActor) : nullptr;
        float       at[3]{};
        if (actor == nullptr || !ActorLocation(actor, at))
        {
            continue;
        }
        const float d1 = Distance(at, one.location);
        const float d2 = Distance(at, two.location);
        if (d1 <= kFarFromOneCm || (kept == kWatched && d2 >= nearest[kWatched - 1].d2))
        {
            continue;
        }
        Sample sample;
        sample.entry = entry;
        sample.actor = actor;
        sample.d1    = d1;
        sample.d2    = d2;
        sample.a1    = AngleOff(one, at);
        sample.a2    = AngleOff(two, at);
        int place    = kept < kWatched ? kept++ : kWatched - 1;
        while (place > 0 && nearest[static_cast<std::size_t>(place - 1)].d2 > d2)
        {
            nearest[static_cast<std::size_t>(place)] = nearest[static_cast<std::size_t>(place - 1)];
            --place;
        }
        nearest[static_cast<std::size_t>(place)] = sample;
    }
    for (int i = 0; i < kept; ++i)
    {
        Sample& sample = nearest[static_cast<std::size_t>(i)];
        ReadEntry(sample);
        Remember(sample);
    }
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto root  = image.Rebase(kGameRoot, sizeof(void*));
    const auto world = image.Rebase(kAnimWorld, sizeof(void*));
    g_gameRoot       = root ? *root : 0;
    g_animWorld      = world ? *world : 0;
    g_installed      = leader_scope::Install(image) && g_gameRoot != 0;
    const auto getter = platform::FindFunction(image, kInterfaceOf);
    g_interfaceOf    = getter ? reinterpret_cast<InterfaceOfFn>(*getter) : nullptr;
    const auto actorGetter = platform::FindFunction(image, kActorInterfaceOf);
    g_actorInterfaceOf     = actorGetter ? reinterpret_cast<InterfaceOfFn>(*actorGetter) : nullptr;
    platform::Log(L"charapop: {}", g_installed ? L"watch installed (read-only; logs the characters near player 2 while split)"
                                               : L"UNAVAILABLE (a check failed)");
    return g_installed;
}

void Tick(bool splitShown) noexcept
{
    if (!g_installed || !splitShown || !generation::OnGameThread() || !generation::Active() || g_lines >= kMaxLines)
    {
        g_wasShown = false;
        return;
    }
    if (!g_wasShown)
    {
        g_wasShown    = true;
        g_lastSummary = {};
    }
    if (++g_ticks % kSampleEvery != 0)
    {
        return;
    }
    ++g_samples;
    try
    {
        SampleNow();
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::charapop_watch
