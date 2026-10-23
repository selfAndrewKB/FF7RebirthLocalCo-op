#include "player_two/world/charapop_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "facts/world.h"
#include "platform/angles.h"
#include "platform/build_value.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/object_fields.h"
#include "player_two/scopes/leader_scope.h"
#include "player_two/world/charapop_facts.h"

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

// The manager, entries and camera pose are read as player_two/world/charapop_facts.h names them; these are the
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
constexpr std::uint32_t  kLeaderOn         = 0x11AE;  // u8, M (the score's own test; 0x11BE was wrong, RE-p2-charapop-two-centres.md)
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
constexpr platform::PerBuild<std::uint32_t> kCompLastRender = {0x2A4, 0x294};  // float: LastRenderTime, written by any view that drew it; 1.005: EndSkelMeshComp_UpdateRecentlyRendered 0x1408B101E [rdi+294h] (1.004 0x140A2E386 [rdi+2A4h])

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
using facts::kAnimWorldManager;
constexpr std::uint32_t  kSnapshotLocation  = 0x8C0;
constexpr std::uint32_t  kSnapshotSpeed     = 0x908;  // float
constexpr std::uint32_t  kSnapshotTurn      = 0x90C;  // float
constexpr std::uint32_t  kSnapshotFaster    = 0x910;  // two u8
constexpr std::uint32_t  kSnapshotValid     = 0x913;  // u8

// UEndSkeletalMeshComponent's optimisation fields (§2.2).
constexpr platform::PerBuild<std::uint32_t> kCompLevelOn    = {0x121C, 0x120C};  // u8: reads the owner's CharaPop level; 1.005: AnimOptimizationEval 0x140861DEF cmp byte [rdi+120Ch]
constexpr platform::PerBuild<std::uint32_t> kCompForced     = {0x1220, 0x1210};  // u8; 1.005: AnimOptimizationEval 0x140861E05 mov [rdi+1210h],al
constexpr platform::PerBuild<std::uint32_t> kCompShadow     = {0x1224, 0x1214};  // u8: cast shadow; 1.005: AnimOptimizationEval 0x140861DAD mov byte [rcx+1214h],1
constexpr platform::PerBuild<std::uint32_t> kCompPauseLevel = {0x1226, 0x1217};  // u8; 1.005: AnimOptimizationEval 0x140861D8C movzx eax,byte [rcx+1217h] (not -0x10: a byte was inserted)
constexpr platform::PerBuild<std::uint32_t> kCompLevel      = {0x1228, 0x1219};  // u8: the owner's CharaPop level, copied; 1.005: AnimOptimizationEval 0x140861E3E mov [rdi+1219h],al
constexpr platform::PerBuild<std::uint32_t> kCompDistance   = {0x1234, 0x1224};  // float, to the snapshot camera; 1.005: AnimOptimizationEval 0x1408628BE, CalcAnimSignificance 0x1409D9D8F [rcx+1224h]
constexpr platform::PerBuild<std::uint32_t> kCompAngle      = {0x1238, 0x1228};  // float, radians; 1.005: CalcAnimSignificance 0x1409D9DAF [rcx+1228h]

constexpr std::uint32_t kSampleEvery   = 10;      // ticks
constexpr float         kFarFromOneCm  = 5000.0f;  // entries more than 50 m from player 1's camera, or
constexpr float         kAnimConeDegrees = 60.0f;  // outside this cone of it (End.Animation.SignificanceCameraDiffDegrees)
constexpr int           kWatched       = 16;
constexpr int           kRemembered    = 64;      // entries whose last logged state is kept
constexpr std::uint32_t kUndrawnSamples = 3;     // a mesh counts as not drawn after this many still samples
constexpr std::uint32_t kSettleSamples  = 3;     // an entry's change is logged once it has held this many samples
constexpr auto          kSummaryEvery  = std::chrono::seconds(60);

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
std::array<Remembered, kRemembered>   g_remembered{};
SummarySteady                         g_summary{};
platform::Every                       g_summaryEvery{kSummaryEvery};
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
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;  // a Debug report (platform/log.h)
    }
    try
    {
        platform::WriteLine(L"[debug] charapop: " + std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
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
    s.level                = ReadField<std::uint8_t>(e, kEntryLevel);
    s.minLevel             = ReadField<std::uint8_t>(e, kEntryMinLevel);
    s.lock                 = ReadField<std::uint8_t>(e, kEntryLock);
    s.pauseWant            = ReadField<std::uint8_t>(e, kEntryPauseWant);
    s.state                = ReadField<std::int32_t>(e, kEntryState);
    s.requested            = ReadField<std::int32_t>(e, kEntryRequested);
    s.pauseFlag            = ReadField<std::uint32_t>(e, kEntryPauseFlag);
    // Asked only in states 5 and 7, where the evaluators ask it themselves (§1.3, §7.4).
    s.paused               = s.state == 5 || s.state == 7 ? PausedOf(e) : kUnread;
    const void* mesh       = EndMeshOf(sample.actor);
    s.meshRead             = mesh != nullptr;
    if (mesh != nullptr)
    {
        s.levelOn           = ReadField<std::uint8_t>(mesh, kCompLevelOn);
        s.forced            = ReadField<std::uint8_t>(mesh, kCompForced);
        s.shadow            = ReadField<std::uint8_t>(mesh, kCompShadow);
        s.pauseLevel        = ReadField<std::uint8_t>(mesh, kCompPauseLevel);
        s.meshLevel         = ReadField<std::uint8_t>(mesh, kCompLevel);
        sample.meshDistance = ReadField<float>(mesh, kCompDistance);
        sample.meshAngle    = ReadField<float>(mesh, kCompAngle) * platform::kDegreesPerRadian;
        s.meshVisible       = (ReadField<std::uint8_t>(mesh, kCompVisibleByte) & kCompVisibleBit) != 0;
        s.meshHiddenInGame  = (ReadField<std::uint8_t>(mesh, kCompHiddenByte) & kCompHiddenBit) != 0;
        sample.lastRender   = ReadField<float>(mesh, kCompLastRender);
    }
    s.flags           = ReadField<std::uint32_t>(e, kEntryFlags);
    s.keep            = ReadField<std::uint32_t>(e, kEntryKeep);
    s.spawnHidden     = ReadField<std::uint8_t>(e, kEntrySpawnHidden);
    sample.score      = ReadField<std::int32_t>(e, kEntryScore);
    sample.useCount   = ReadField<std::int32_t>(e, kEntryUseCount);
    s.actorHidden     = (ReadField<std::uint8_t>(sample.actor, kActorHiddenByte) & kActorHiddenBit) != 0;
    const void* fade  = FadeOf(e);
    s.fadeRead        = fade != nullptr;
    if (fade != nullptr)
    {
        s.fadeHidden   = ReadField<std::uint8_t>(fade, kFadeHidden) != 0;
        s.fadeReasons  = ReadField<std::uint32_t>(fade, kFadeReasons);
        s.fadeTarget   = ReadField<float>(fade, kFadeTarget);
        sample.opacity = ReadField<float>(fade, kFadeOpacity);
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
         first ? std::format(L" ({}, id {}, category {}, key {:#x})", NameOf(sample.actor),
                             ReadField<std::int32_t>(sample.entry, kEntryId),
                             ReadField<std::uint8_t>(sample.entry, kEntryCategory),
                             ReadField<std::uint64_t>(sample.entry, kEntryKey))
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
    if (first)
    {
        slot           = oldest;
        *slot          = {sample.entry, sample.steady, g_samples, sample.lastRender};
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
    if (slot->pendingSeen >= kSettleSamples)
    {
        slot->steady      = sample.steady;
        slot->pendingSeen = 0;
        LogSample(sample, false);
    }
}

/// Which centre the population follows, and its distance to each camera.
std::wstring CentreText(const std::byte* manager, const Camera& one, const Camera& two)
{
    const bool overridden = ReadField<std::uint8_t>(manager, kOverrideOn) != 0;
    if (!overridden && ReadField<std::uint8_t>(manager, kLeaderOn) == 0)
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
    if (steady == g_summary && !g_summaryEvery.Due(now))
    {
        return;
    }
    g_summary = steady;
    g_summaryEvery.Restart(now);  // a change logs at once, and the period counts from it
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
    const bool  shot  = world != nullptr && ReadField<std::uint8_t>(world, kSnapshotValid) != 0 &&
                      platform::CopyGuarded(snapshot, world + kSnapshotLocation, sizeof(snapshot));
    Line(L"{} entries (cursor {}, force-all {}, queued {}); cameras {:.0f} m apart; anim snapshot {} ({:.0f} m from "
         L"player 1's camera, {:.0f} from player 2's; speed {:.0f} turn {:.2f} faster {}/{}) | params: FOV {:.0f}, Light draw "
         L"{:.0f} in {:.0f} out {:.0f} tick {:.0f}, Middle draw {:.0f} in {:.0f} out {:.0f} tick {:.0f}, split {}, enemy +{:.0f} "
         L"audience +{:.0f}; overrides draw {:.0f} ({}) in {:.0f} ({}) out {:.0f} ({}) | population centre: {}; threshold {}",
         count, ReadField<std::int32_t>(pop, kPopCursor), ReadField<std::int32_t>(pop, kPopForceAll),
         ReadField<std::int32_t>(pop, kPopQueueCount), Distance(one.location, two.location) / 100.0f,
         shot ? L"read" : L"not read", Distance(snapshot, one.location) / 100.0f,
         Distance(snapshot, two.location) / 100.0f, ReadField<float>(world, kSnapshotSpeed),
         ReadField<float>(world, kSnapshotTurn), ReadField<std::uint8_t>(world, kSnapshotFaster),
         ReadField<std::uint8_t>(world, kSnapshotFaster + 1), f(0x1148), f(0x114C), f(0x1150), f(0x1154), f(0x1158),
         f(0x115C), f(0x1160), f(0x1164), f(0x1168), split, f(0x1170), f(0x1174), f(0x1178), b(0x117C), f(0x1180),
         b(0x1184), f(0x1188), b(0x118C), CentreText(manager, one, two),
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
        if (kept == kWatched && d2 >= nearest[kWatched - 1].d2)
        {
            continue;
        }
        // Far from player 1, or near but outside player 1's anim cone (the anim optimisation's out-of-camera case:
        // live 2026-10-07, NPCs in player 2's view froze when player 1 turned their back).
        const float a1 = AngleOff(one, at);
        if (d1 <= kFarFromOneCm && a1 <= kAnimConeDegrees)
        {
            continue;
        }
        Sample sample;
        sample.entry = entry;
        sample.actor = actor;
        sample.d1    = d1;
        sample.d2    = d2;
        sample.a1    = a1;
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
    const auto root  = image.Rebase(facts::kEndGameRoot, sizeof(void*));
    const auto world = image.Rebase(kAnimWorldManager, sizeof(void*));
    g_gameRoot       = root ? *root : 0;
    g_animWorld      = world ? *world : 0;
    g_installed      = leader_scope::Installed() && g_gameRoot != 0;
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
    // The watch only logs, and its lines are Debug: nothing is sampled below that level.
    if (!g_installed || !splitShown || !platform::Enabled(platform::Level::Debug) || !generation::OnGameThread() ||
        !generation::Active())
    {
        g_wasShown = false;
        return;
    }
    if (!g_wasShown)
    {
        g_wasShown = true;
        g_summaryEvery.Reset();
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
