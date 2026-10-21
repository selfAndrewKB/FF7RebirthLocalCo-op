#include "player_two/anim_two_views.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "platform/angles.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/describe.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace lss::player_two::anim_two_views {

namespace {

// 1.004 addresses; 1.005 through replaced-anim-two-views.json (every call site and function read from both exes).
constexpr std::uintptr_t kAnimWorld   = 0x14939BC48;  // the animation world manager W
constexpr std::uintptr_t kClearCall   = 0x140A32B57;  // PropagateLinkedActorCamDist's last call: its arrays are refilled
constexpr std::uintptr_t kEvalCall    = 0x141001E92;  // AnimBudgetAllocator_Tick_SE: the parallel evaluation
constexpr std::uintptr_t kEvalTarget  = 0x1410025A0;  // ParallelFor_Wrapper
constexpr std::uintptr_t kGrowCall    = 0x140A32B0A;  // the game's own append: `mov rcx, array; call TArray8_ResizeGrow`
constexpr std::uintptr_t kInterfaceClassGlobal = 0x1492B5E30;  // UClass* of EndActorInterface

constexpr std::uint8_t kInterfaceOfBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                              0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
const platform::FunctionFact kInterfaceOf{L"UObject_GetInterfaceAddress", 0x140885660, kInterfaceOfBytes};
constexpr std::uint8_t kMeasureBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                          0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41};
const platform::FunctionFact kMeasure{L"EndSkelMeshComp_ComputeCamDistAngle", 0x140886E6C, kMeasureBytes};

// W's registrations: a stock TSparseArray (platform/sparse_array.h) at +0x7F8 of 56-byte elements. An element's
// actor is a weak pointer at +0x18.
constexpr std::uint32_t kSetData   = 0x7F8;
constexpr std::uint32_t kElemSize  = 56;
constexpr std::uint32_t kElemActor = 0x18;
constexpr std::int32_t  kMaxBits   = 1 << 16;  // sanity bound on the set's bit count (a world set, not a HUD one)

// The actor's EndActorInterface: vt+0x90 valid, vt+0xA0 its data X (X+0x10 weak main mesh, X+0x40 the pair list).
constexpr std::uint32_t kIfaceValid = 0x90;
constexpr std::uint32_t kIfaceData  = 0xA0;
constexpr std::uint32_t kDataMesh   = 0x10;
constexpr std::uint32_t kDataPairs  = 0x40;  // TArray<{float dist, float angle}>: data, Num +8, Max +0xC
constexpr platform::PerBuild<std::uint32_t> kMeshDist  = {0x1234, 0x1224};
constexpr platform::PerBuild<std::uint32_t> kMeshAngle = {0x1238, 0x1228};

// Past this distance from player 2's camera no End.Animation.* threshold can change a decision (the largest, the wild
// animals' 300 m force-update distance, plus a margin).
constexpr float kReach        = 33000.0f;
constexpr auto  kSummaryEvery = std::chrono::seconds(10);

// The tick-time measure (RE-p2-detail-drop-p1-facing.md §2). Its first instruction crosses a block in 1.004, so the
// second is replaced there (both a store into the caller's home area).
constexpr std::uint8_t   kMeasureFirstTwo[]  = {0x48, 0x89, 0x5C, 0x24, 0x10,   // mov [rsp+10h],rbx
                                                0x48, 0x89, 0x74, 0x24, 0x18};  // mov [rsp+18h],rsi
constexpr std::size_t    kFirstInstruction   = 5;
constexpr std::size_t    kSecondInstruction  = 5;
constexpr std::uintptr_t kCameraDiffDegrees  = 0x1470B99A0;  // End.Animation.SignificanceCameraDiffDegrees (60)
constexpr platform::PerBuild<std::uint32_t> kMeshMode = {0x122A, 0x121B};  // ShouldUpdateTransform's mode (1 = angle)
constexpr std::uint32_t  kComponentOwner     = 0xA0;
using platform::kDegreesPerRadian;
using platform::kRadiansPerDegree;

using PassFn        = std::uintptr_t (*)(void* a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d);
using InterfaceOfFn = void* (*)(const void* object, const void* cls);
using IfaceValidFn  = std::uint8_t (*)(void* iface);
using IfaceDataFn   = void* (*)(void* iface);
using MeasureFn     = void (*)(void* mesh, const float* location, const float* forward);
using GrowFn        = void (*)(void* array);

platform::CallRedirect g_clearCall;
platform::CallRedirect g_evalCall;
InterfaceOfFn          g_interfaceOf    = nullptr;
MeasureFn              g_measure        = nullptr;
GrowFn                 g_grow           = nullptr;
std::uintptr_t         g_worldGlobal    = 0;
std::uintptr_t         g_classGlobal    = 0;

std::atomic<std::uint32_t> g_frame{0};    // engine ticks (game thread writes)
std::atomic<std::uint32_t> g_refilled{0};  // the frame the game last refilled the pair lists (any thread writes)
std::atomic<std::uint32_t> g_refills{0};   // refills seen (any thread), for the summary
// Set at the end of each refill, taken by the next evaluation: one set of player 2's pairs per refill. (Live
// 2026-10-10: the evaluation runs before the refill within a frame, so it reads the lists the previous frame's refill
// left; a "same frame" test never matched.)
std::atomic<bool> g_fresh{false};
bool                       g_split = false;  // game thread

struct Item
{
    void* data;
    void* mesh;
};
std::vector<Item> g_items;  // game thread, reused

struct Counts
{
    unsigned      frames = 0, skipped = 0, evaluations = 0;
    std::int64_t  lagSum = 0;  // frame minus the refill's stamp, at each evaluation
    std::uint64_t elements = 0, inReach = 0, appended = 0;
    double        micros = 0, maxMicros = 0;
};
Counts                                g_counts;
platform::Every g_summary{kSummaryEvery};

platform::EntryRedirect g_measureEntry;
const float*            g_cameraDiff = nullptr;  // the game's cvar, degrees

// Player 2's camera for the tick-time measures (last frame's pose, as player 1's snapshot), with player 2's character
// and mount for the log. Written by the game thread once per engine tick, read by the workers that tick the meshes
// (live 2026-10-11: every measure from player 1's snapshot runs on a worker). A seqlock: the version is odd while the
// game thread writes, and a reader that sees it odd or changed keeps player 1's answer.
struct TwoCamera
{
    std::atomic<std::uint32_t>        version{0};
    std::atomic<bool>                 valid{false};
    std::array<std::atomic<float>, 3> location{};
    std::array<std::atomic<float>, 3> forward{};
    std::atomic<const void*>          member{nullptr};
    std::atomic<const void*>          mount{nullptr};
};
TwoCamera g_twoCamera;

// A reader's copy.
struct CameraCopy
{
    float       location[4]{};
    float       forward[4]{};
    const void* member = nullptr;
    const void* mount  = nullptr;
};

struct TickCounts
{
    std::atomic<unsigned>      calls{0}, nearer{0}, onWorkers{0}, torn{0};
    std::atomic<std::uint64_t> ticks{0};  // QueryPerformanceCounter ticks spent
};
TickCounts g_tick;  // any thread

// Player 2's character's and mount's meshes: their last state (Debug on change). Keyed by address only, never read
// through: a freed mesh's address reused by another mesh can at worst skip or add one log line. Few meshes, and
// cleared whenever the split stops being shown.
struct Watched
{
    const void* mesh  = nullptr;
    int         state = -1;
};
std::array<Watched, 8> g_watched{};
std::mutex             g_watchedLock;  // the workers note, the game thread clears

/// Game thread: publishes player 2's camera (or none) for the workers' measures.
void PublishTwoCamera(bool valid, const charapop::Camera& two, const void* member, const void* mount) noexcept
{
    const auto version = g_twoCamera.version.load(std::memory_order_relaxed);
    g_twoCamera.version.store(version + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_twoCamera.valid.store(valid, std::memory_order_relaxed);
    for (std::size_t i = 0; i < std::size(charapop::Camera{}.location); ++i)
    {
        g_twoCamera.location[i].store(two.location[i], std::memory_order_relaxed);
        g_twoCamera.forward[i].store(two.forward[i], std::memory_order_relaxed);
    }
    g_twoCamera.member.store(member, std::memory_order_relaxed);
    g_twoCamera.mount.store(mount, std::memory_order_relaxed);
    g_twoCamera.version.store(version + 2, std::memory_order_release);
}

/// Any thread: player 2's camera as last published. False if there is none, or the game thread kept writing it.
bool ReadTwoCamera(CameraCopy& out) noexcept
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto before = g_twoCamera.version.load(std::memory_order_acquire);
        if ((before & 1u) != 0)
        {
            continue;
        }
        const bool valid = g_twoCamera.valid.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < std::size(charapop::Camera{}.location); ++i)
        {
            out.location[i] = g_twoCamera.location[i].load(std::memory_order_relaxed);
            out.forward[i]  = g_twoCamera.forward[i].load(std::memory_order_relaxed);
        }
        out.member = g_twoCamera.member.load(std::memory_order_relaxed);
        out.mount  = g_twoCamera.mount.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_twoCamera.version.load(std::memory_order_relaxed) == before)
        {
            return valid;
        }
    }
    g_tick.torn.fetch_add(1, std::memory_order_relaxed);
    return false;
}

/// Logs a change of a mesh of player 2's character or mount: its mode and whether player 1's camera alone would have
/// skipped its bone refresh and bone physics (mode 1 and an angle past the limit; the camera-cut exception aside).
/// Any thread.
void NoteMesh(void* mesh, float angleOne, float angleNow, const CameraCopy& two) noexcept
{
    const void* owner = ReadPointer(mesh, kComponentOwner);
    if (owner == nullptr || (owner != two.member && (owner != two.mount || two.mount == two.member)))
    {
        return;
    }
    const auto  mode  = ReadField<std::uint8_t>(mesh, kMeshMode);
    const float limit = (g_cameraDiff != nullptr ? *g_cameraDiff : 60.0f) * kRadiansPerDegree;
    const bool  byOne = mode == 1 && angleOne >= limit;
    const bool  now   = mode == 1 && angleNow >= limit;
    const int   state = mode | (byOne ? 0x100 : 0) | (now ? 0x200 : 0);
    const std::lock_guard lock(g_watchedLock);
    Watched*              slot = nullptr;
    for (Watched& w : g_watched)
    {
        if (w.mesh == mesh || (slot == nullptr && w.mesh == nullptr))
        {
            slot = &w;
            if (w.mesh == mesh)
            {
                break;
            }
        }
    }
    if (slot == nullptr || (slot->mesh == mesh && slot->state == state))
    {
        return;
    }
    slot->mesh  = mesh;
    slot->state = state;
    platform::LogDebug(L"anim tick time: {} mesh {}: mode {}, angle from player 1's camera {:.0f} deg, kept {:.0f} deg "
                       L"(limit {:.0f}); player 1's camera alone would {} its bones and bone physics; they {}",
                       owner == two.member ? L"player 2's character's" : L"player 2's mount's", NameOf(mesh), mode,
                       angleOne * kDegreesPerRadian, angleNow * kDegreesPerRadian, limit * kDegreesPerRadian,
                       byOne ? L"skip" : L"update", now ? L"are skipped" : L"update");
}

/// The game's measure of a mesh's distance and angle. Called by a mesh tick from player 1's snapshot, it measures
/// again from player 2's camera and keeps the minimum of each value (any thread: the workers tick the meshes).
void MeasureTwoViews(void* mesh, const float* location, const float* forward) noexcept
{
    const auto original = reinterpret_cast<MeasureFn>(g_measureEntry.Original());
    original(mesh, location, forward);
    if (!generation::Active())
    {
        return;
    }
    // Both of the game's callers (the budgeted mesh tick and 1.004 0x140AFD2B8 / 1.005 0x141983790) pass stack copies
    // of player 1's snapshot (W+0x8C0, W+0x8E0), so every call here is a measure from player 1's camera (live
    // 2026-10-11: a test for W+0x8C0's own address never matched). The mod's own measures call the original directly.
    CameraCopy two;
    if (!ReadTwoCamera(two))
    {
        return;  // the split is not shown, or player 2's camera is being written: player 1's answer stands
    }
    LARGE_INTEGER start{}, stop{};
    QueryPerformanceCounter(&start);
    const std::uint32_t distAt = kMeshDist, angleAt = kMeshAngle;
    const float         distOne  = ReadField<float>(mesh, distAt);
    const float         angleOne = ReadField<float>(mesh, angleAt);
    original(mesh, two.location, two.forward);
    const float distTwo  = ReadField<float>(mesh, distAt);
    const float angleTwo = ReadField<float>(mesh, angleAt);
    const float angle    = std::min(angleOne, angleTwo);
    WriteField<float>(mesh, distAt, std::min(distOne, distTwo));
    WriteField<float>(mesh, angleAt, angle);
    QueryPerformanceCounter(&stop);
    g_tick.calls.fetch_add(1, std::memory_order_relaxed);
    g_tick.nearer.fetch_add(distTwo < distOne || angleTwo < angleOne ? 1u : 0u, std::memory_order_relaxed);
    g_tick.onWorkers.fetch_add(generation::OnGameThread() ? 0u : 1u, std::memory_order_relaxed);
    g_tick.ticks.fetch_add(static_cast<std::uint64_t>(stop.QuadPart - start.QuadPart), std::memory_order_relaxed);
    if (platform::Enabled(platform::Level::Debug))
    {
        NoteMesh(mesh, angleOne, angle, two);
    }
}

/// The game's last call in its refill (a worker, on a normal pass): the pair lists are this frame's.
std::uintptr_t Refilled(void* a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_refilled.store(g_frame.load(std::memory_order_relaxed), std::memory_order_release);
    g_refills.fetch_add(1, std::memory_order_relaxed);
    g_fresh.store(true, std::memory_order_release);
    return reinterpret_cast<PassFn>(g_clearCall.Original())(a, b, c, d);
}

/// Each registered actor near player 2's camera gets player 2's {distance, angle} in its pair list (game thread, before
/// the parallel evaluation: nothing else touches the lists or the meshes' two fields now).
void AppendPlayerTwo() noexcept
{
    const charapop::Camera two = charapop::CameraOf(generation::PlayerTwoController());
    auto*       world = static_cast<std::byte*>(ReadPointer(reinterpret_cast<const void*>(g_worldGlobal), 0));
    const void* cls   = ReadPointer(reinterpret_cast<const void*>(g_classGlobal), 0);
    if (!two.read || world == nullptr || cls == nullptr)
    {
        ++g_counts.skipped;
        return;
    }
    if (ReadPointer(world, kSetData) == nullptr)
    {
        ++g_counts.skipped;  // the walk below refuses an unreadable bit count itself
        return;
    }
    const float location[4] = {two.location[0], two.location[1], two.location[2], 0.0f};
    const float forward[4]  = {two.forward[0], two.forward[1], two.forward[2], 0.0f};
    g_items.clear();
    const auto visit = [&](std::uintptr_t element) {
        void* actor = leader_scope::ObjectAt(reinterpret_cast<const std::byte*>(element) + kElemActor);
        if (actor == nullptr)
        {
            return true;
        }
        ++g_counts.elements;
        float at[3]{};
        if (!charapop::ActorLocation(actor, at) || charapop::Distance(at, two.location) > kReach)
        {
            return true;
        }
        void* iface = nullptr;
        if (!platform::CallGuarded(g_interfaceOf, iface, static_cast<const void*>(actor), cls) || iface == nullptr)
        {
            return true;
        }
        const auto   valid = reinterpret_cast<IfaceValidFn>(ReadPointer(ReadPointer(iface, 0), kIfaceValid));
        const auto   get   = reinterpret_cast<IfaceDataFn>(ReadPointer(ReadPointer(iface, 0), kIfaceData));
        std::uint8_t ok    = 0;
        void*        pairs = nullptr;
        if (valid == nullptr || get == nullptr || !platform::CallGuarded(valid, ok, iface) || ok == 0 ||
            !platform::CallGuarded(get, pairs, iface) || pairs == nullptr)
        {
            return true;
        }
        void* mesh = leader_scope::ObjectAt(static_cast<std::byte*>(pairs) + kDataMesh);
        if (mesh != nullptr && g_items.size() < g_items.capacity())
        {
            g_items.push_back({pairs, mesh});  // within the capacity reserved at install: never allocates
        }
        return true;
    };
    platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(world) + kSetData, kElemSize, visit, kMaxBits);
    // One pair per actor (several registrations can share an actor).
    std::sort(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return a.data < b.data; });
    const auto end = std::unique(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return a.data == b.data; });
    const std::uint32_t distAt = kMeshDist, angleAt = kMeshAngle;
    for (auto it = g_items.begin(); it != end; ++it)
    {
        ++g_counts.inReach;
        // The game's measure writes the mesh's two fields; they are put back (the evaluation recomputes them anyway).
        const auto savedDist  = ReadField<float>(it->mesh, distAt);
        const auto savedAngle = ReadField<float>(it->mesh, angleAt);
        if (!platform::CallGuarded(g_measure, it->mesh, location, forward))
        {
            continue;
        }
        const float pair[2] = {ReadField<float>(it->mesh, distAt), ReadField<float>(it->mesh, angleAt)};
        WriteField<float>(it->mesh, distAt, savedDist);
        WriteField<float>(it->mesh, angleAt, savedAngle);
        // Appended as the game appends: Num++, the game's grow past Max, then the pair.
        auto*      list = static_cast<std::byte*>(it->data) + kDataPairs;
        const auto num  = ReadField<std::int32_t>(list, 8);
        WriteField<std::int32_t>(list, 8, num + 1);
        if (num + 1 > ReadField<std::int32_t>(list, 0xC) && !platform::CallGuarded(g_grow, static_cast<void*>(list)))
        {
            WriteField<std::int32_t>(list, 8, num);
            continue;
        }
        auto* slots = static_cast<float*>(ReadPointer(list, 0));
        if (slots == nullptr)
        {
            WriteField<std::int32_t>(list, 8, num);
            continue;
        }
        slots[2 * static_cast<std::size_t>(num)]     = pair[0];
        slots[2 * static_cast<std::size_t>(num) + 1] = pair[1];
        ++g_counts.appended;
    }
}

/// The allocator's parallel evaluation (game thread): player 2's pairs first, when the lists were refilled this frame.
std::uintptr_t Evaluate(void* a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    if (g_split && generation::Active() && generation::OnGameThread())
    {
        ++g_counts.evaluations;
        g_counts.lagSum += std::int64_t(g_frame.load(std::memory_order_relaxed)) -
                           std::int64_t(g_refilled.load(std::memory_order_acquire));
        if (g_fresh.exchange(false, std::memory_order_acq_rel))
        {
            LARGE_INTEGER start{}, stop{}, rate{};
            QueryPerformanceCounter(&start);
            AppendPlayerTwo();
            QueryPerformanceCounter(&stop);
            QueryPerformanceFrequency(&rate);
            const double micros = rate.QuadPart > 0 ? 1e6 * double(stop.QuadPart - start.QuadPart) / double(rate.QuadPart) : 0;
            ++g_counts.frames;
            g_counts.micros += micros;
            g_counts.maxMicros = std::max(g_counts.maxMicros, micros);
        }
        else
        {
            ++g_counts.skipped;  // not refilled since the last evaluation: an append would pile up
        }
    }
    return reinterpret_cast<PassFn>(g_evalCall.Original())(a, b, c, d);
}

/// The callee of the call at `site` (an E8 call), or 0.
std::uintptr_t CallTarget(const platform::GameImage& image, std::uintptr_t site) noexcept
{
    const auto at = image.Rebase(site, 5);
    std::uint8_t bytes[5]{};
    if (!at || !platform::CopyGuarded(bytes, reinterpret_cast<const void*>(*at), sizeof(bytes)) || bytes[0] != 0xE8)
    {
        return 0;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    return *at + 5 + static_cast<std::intptr_t>(rel);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto world = image.Rebase(kAnimWorld, sizeof(void*));
    const auto cls   = image.Rebase(kInterfaceClassGlobal, sizeof(void*));
    const auto iface = platform::FindFunction(image, kInterfaceOf);
    const auto meas  = platform::FindFunction(image, kMeasure);
    const auto grow  = CallTarget(image, kGrowCall);
    const auto growMapped = image.Rebase(0x14082DFE8);
    try
    {
        g_items.reserve(4096);
    }
    catch (...)
    {
    }
    if (!world || !cls || !iface || !meas || grow == 0 || !growMapped || grow != *growMapped ||
        g_items.capacity() < 4096)
    {
        platform::Log(L"anim two views: NOT installed (a function, global or the game's grow call differs); characters "
                      L"near player 2 keep player 1's camera's animation rate");
        return;
    }
    g_worldGlobal = *world;
    g_classGlobal = *cls;
    g_interfaceOf = reinterpret_cast<InterfaceOfFn>(*iface);
    g_measure     = reinterpret_cast<MeasureFn>(*meas);
    g_grow        = reinterpret_cast<GrowFn>(grow);
    const bool refill = g_clearCall.InstallResearch(image, kClearCall, facts::kFreeArrayData.address,
                                                    reinterpret_cast<std::uintptr_t>(&Refilled));
    const bool eval   = refill && g_evalCall.InstallResearch(image, kEvalCall, kEvalTarget,
                                                             reinterpret_cast<std::uintptr_t>(&Evaluate));
    if (refill && !eval)
    {
        g_clearCall.Restore();
    }
    // The tick-time measure: player 2's appends above call the game's measure directly (its trampoline), not this.
    bool       tickTime = false;
    const auto diff     = image.Rebase(kCameraDiffDegrees, sizeof(float));
    if (eval && diff)
    {
        g_cameraDiff = reinterpret_cast<const float*>(*diff);
        const std::span<const std::uint8_t> bytes(kMeasureFirstTwo);
        const auto replacement = reinterpret_cast<std::uintptr_t>(&MeasureTwoViews);
        tickTime = g_measureEntry.Install(*meas, image.EntryInstruction(kMeasure.address, bytes.first(kFirstInstruction)),
                                          replacement) ||
                   g_measureEntry.InstallAtSecondInstruction(*meas, bytes.first(kFirstInstruction),
                                                             bytes.subspan(kFirstInstruction, kSecondInstruction),
                                                             replacement);
        if (tickTime)
        {
            g_measure = reinterpret_cast<MeasureFn>(g_measureEntry.Original());
        }
    }
    platform::Log(L"anim two views: {}; at tick time, {}",
                  eval ? L"characters near player 2 animate by the nearer of the two cameras (the game's own pair list)"
                       : L"NOT installed (a call differs); characters near player 2 keep player 1's camera's rate",
                  tickTime ? L"meshes refresh their bones and bone physics by the nearer camera too"
                           : L"bone refresh and bone physics STILL follow player 1's camera (the measure differs)");
}

void Remove() noexcept
{
    if (g_measureEntry.IsInstalled() && !g_measureEntry.Restore())
    {
        platform::Log(L"anim two views: the measure was changed after us; left in place (it passes through)");
    }
    for (platform::CallRedirect* redirect : {&g_evalCall, &g_clearCall})
    {
        if (redirect->IsInstalled() && !redirect->Restore())
        {
            platform::Log(L"anim two views: a call was changed after us; left in place (it passes through)");
        }
    }
}

void Tick(bool splitShown) noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    g_frame.fetch_add(1, std::memory_order_relaxed);
    g_split = splitShown;
    if (splitShown && generation::Active())
    {
        void* const            controller = generation::PlayerTwoController();
        const charapop::Camera two        = charapop::CameraOf(controller);
        PublishTwoCamera(two.read, two, generation::Member(), ReadPointer(controller, facts::kControllerPawn));
    }
    else
    {
        PublishTwoCamera(false, charapop::Camera{}, nullptr, nullptr);
        const std::lock_guard lock(g_watchedLock);
        g_watched = {};
    }
    const auto now = std::chrono::steady_clock::now();
    if (!g_summary.Due(now))
    {
        return;
    }
    const auto calls  = platform::Take(g_tick.calls);
    const auto nearer = platform::Take(g_tick.nearer);
    const auto worker = platform::Take(g_tick.onWorkers);
    const auto torn   = platform::Take(g_tick.torn);
    const auto ticks  = platform::Take(g_tick.ticks);
    if (calls != 0 || torn != 0)
    {
        LARGE_INTEGER rate{};
        QueryPerformanceFrequency(&rate);
        const double ms = rate.QuadPart > 0 ? 1000.0 * double(ticks) / double(rate.QuadPart) : 0.0;
        platform::LogDebug(L"anim tick time ({} s): {} measures from player 1's snapshot also from player 2's camera ({} "
                           L"on workers), {} of them nearer for player 2; {:.3f} ms of thread time per frame on average; "
                           L"{} left player 1's (player 2's camera was being written)",
                           kSummaryEvery.count(), calls, worker, nearer,
                           ms / std::max(1.0, double(g_counts.evaluations)), torn);
    }
    const auto refills = platform::Take(g_refills);
    if (g_counts.frames != 0 || g_counts.skipped != 0)
    {
        const double frames = g_counts.frames != 0 ? g_counts.frames : 1;
        platform::LogDebug(L"anim two views ({} s): {} frame(s) with player 2's pairs ({} skipped; {} evaluation(s), {} "
                           L"refill(s) of the game's lists, the refill's frame lagging {:.2f} on average); per frame {:.0f} "
                           L"registered actor(s), {:.0f} near player 2's camera, {:.0f} pair(s) added; {:.3f} ms per frame "
                           L"(worst {:.3f})",
                           kSummaryEvery.count(), g_counts.frames, g_counts.skipped, g_counts.evaluations, refills,
                           g_counts.evaluations != 0 ? double(g_counts.lagSum) / g_counts.evaluations : 0.0,
                           double(g_counts.elements) / frames,
                           double(g_counts.inReach) / frames, double(g_counts.appended) / frames,
                           g_counts.micros / frames / 1000.0, g_counts.maxMicros / 1000.0);
    }
    g_counts = {};
}

}  // namespace lss::player_two::anim_two_views
