#include "world/anim_every_view.h"

#include "facts/controller.h"
#include "facts/engine.h"
#include "facts/world.h"
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
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "scopes/leader_scope.h"
#include "world/charapop_facts.h"

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

namespace lss::world::anim_every_view {

namespace {

// 1.004 addresses; 1.005 through replaced-anim-two-views.json (every call site and function read from both exes).
using facts::kAnimWorldManager;
constexpr std::uintptr_t kClearCall   = 0x140A32B57;  // PropagateLinkedActorCamDist's last call: its arrays are refilled
constexpr std::uintptr_t kEvalCall    = 0x141001E92;  // AnimBudgetAllocator_Tick_SE: the parallel evaluation
constexpr std::uintptr_t kEvalTarget  = 0x1410025A0;  // ParallelFor_Wrapper
constexpr std::uintptr_t kGrowCall    = 0x140A32B0A;  // the game's own append: `mov rcx, array; call TArray8_ResizeGrow`
constexpr std::uint8_t   kGrowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56,
                                         0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x20, 0x00};
const platform::FunctionFact kGrow{L"TArray8_ResizeGrow", 0x14082DFE8, kGrowBytes};  // what the call must reach
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

// Past this distance from a seat's camera no End.Animation.* threshold can change a decision (the largest, the wild
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
// Set at the end of each refill, taken by the next evaluation: one set of the seats' pairs per refill. (Live
// 2026-10-10: the evaluation runs before the refill within a frame, so it reads the lists the previous frame's refill
// left; a "same frame" test never matched.)
std::atomic<bool> g_fresh{false};
bool                       g_split = false;  // game thread

struct Item
{
    void*        data;
    void*        mesh;
    std::uint8_t seats;  // bit s: within reach of seat s's camera
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

// A seat's camera for the tick-time measures (last frame's pose, as player 1's snapshot), with the seat's character
// and mount for the log; one per seat (g_seatCameras, by seat; index 0, player 1's, unused: player 1's is the game's
// snapshot). Written by the game thread once per engine tick, read by the workers that tick the meshes (live
// 2026-10-11: every measure from player 1's snapshot runs on a worker). A seqlock per seat: the version is odd while
// the game thread writes, and a reader that sees it odd or changed keeps the answer without that seat.
struct SeatCamera
{
    std::atomic<std::uint32_t>        version{0};
    std::atomic<bool>                 valid{false};
    std::array<std::atomic<float>, 3> location{};
    std::array<std::atomic<float>, 3> forward{};
    std::atomic<const void*>          member{nullptr};
    std::atomic<const void*>          mount{nullptr};
};
std::array<SeatCamera, players::kMaxPlayers> g_seatCameras;

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

// The seats' characters' and mounts' meshes: their last state (Debug on change), one shared table. Keyed by address
// only, never read through: a freed mesh's address reused by another mesh can at worst skip or add one log line. Few
// meshes, and cleared whenever no seat's camera is published.
struct Watched
{
    const void* mesh  = nullptr;
    int         state = -1;
};
std::array<Watched, 8> g_watched{};
std::mutex             g_watchedLock;  // the workers note, the game thread clears

/// Game thread: publishes `seat`'s camera (or none) for the workers' measures.
void PublishSeatCamera(int seat, bool valid, const charapop::Camera& pose, const void* member,
                       const void* mount) noexcept
{
    SeatCamera& camera  = g_seatCameras[static_cast<std::size_t>(seat)];
    const auto  version = camera.version.load(std::memory_order_relaxed);
    camera.version.store(version + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    camera.valid.store(valid, std::memory_order_relaxed);
    for (std::size_t i = 0; i < std::size(charapop::Camera{}.location); ++i)
    {
        camera.location[i].store(pose.location[i], std::memory_order_relaxed);
        camera.forward[i].store(pose.forward[i], std::memory_order_relaxed);
    }
    camera.member.store(member, std::memory_order_relaxed);
    camera.mount.store(mount, std::memory_order_relaxed);
    camera.version.store(version + 2, std::memory_order_release);
}

/// Any thread: `seat`'s camera as last published. False if there is none, or the game thread kept writing it.
bool ReadSeatCamera(int seat, CameraCopy& out) noexcept
{
    const SeatCamera& camera = g_seatCameras[static_cast<std::size_t>(seat)];
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto before = camera.version.load(std::memory_order_acquire);
        if ((before & 1u) != 0)
        {
            continue;
        }
        const bool valid = camera.valid.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < std::size(charapop::Camera{}.location); ++i)
        {
            out.location[i] = camera.location[i].load(std::memory_order_relaxed);
            out.forward[i]  = camera.forward[i].load(std::memory_order_relaxed);
        }
        out.member = camera.member.load(std::memory_order_relaxed);
        out.mount  = camera.mount.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (camera.version.load(std::memory_order_relaxed) == before)
        {
            return valid;
        }
    }
    g_tick.torn.fetch_add(1, std::memory_order_relaxed);
    return false;
}

/// Logs a change of a mesh of a seat's character or mount (`cameras[s]`, read for the seats in `read`): its mode and
/// whether player 1's camera alone would have skipped its bone refresh and bone physics (mode 1 and an angle past the
/// limit; the camera-cut exception aside). Any thread.
void NoteMesh(void* mesh, float angleOne, float angleNow, const CameraCopy (&cameras)[players::kMaxPlayers],
              std::uint8_t read) noexcept
{
    const void* owner = ReadPointer(mesh, kComponentOwner);
    int         seat  = -1;
    bool        mount = false;
    for (int s = 1; s <= players::kSeatsBuilt && owner != nullptr && seat < 0; ++s)
    {
        const CameraCopy& camera = cameras[s];
        if ((read & (1u << s)) != 0 &&
            (owner == camera.member || (owner == camera.mount && camera.mount != camera.member)))
        {
            seat  = s;
            mount = owner != camera.member;
        }
    }
    if (seat < 0)
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
    platform::LogDebug(L"anim tick time: {}'s {} mesh {}: mode {}, angle from player 1's camera {:.0f} deg, kept {:.0f} "
                       L"deg (limit {:.0f}); player 1's camera alone would {} its bones and bone physics; they {}",
                       players::SeatName(seat), mount ? L"mount's" : L"character's", players::NameOf(mesh), mode,
                       angleOne * kDegreesPerRadian, angleNow * kDegreesPerRadian, limit * kDegreesPerRadian,
                       byOne ? L"skip" : L"update", now ? L"are skipped" : L"update");
}

/// The game's measure of a mesh's distance and angle. Called by a mesh tick from player 1's snapshot, it measures
/// again from each seat's camera, in seat order, and keeps the minimum of each value (any thread: the workers tick the
/// meshes).
void MeasureEveryView(void* mesh, const float* location, const float* forward) noexcept
{
    const auto original = reinterpret_cast<MeasureFn>(g_measureEntry.Original());
    original(mesh, location, forward);
    if (!players::generation::AnyActive())
    {
        return;
    }
    // Both of the game's callers (the budgeted mesh tick and 1.004 0x140AFD2B8 / 1.005 0x141983790) pass stack copies
    // of player 1's snapshot (W+0x8C0, W+0x8E0), so every call here is a measure from player 1's camera (live
    // 2026-10-11: a test for W+0x8C0's own address never matched). The mod's own measures call the original directly.
    CameraCopy   cameras[players::kMaxPlayers];
    std::uint8_t read = 0;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        read |= ReadSeatCamera(seat, cameras[seat]) ? static_cast<std::uint8_t>(1u << seat) : std::uint8_t{0};
    }
    if (read == 0)
    {
        return;  // the split is not shown, or the seats' cameras are being written: player 1's answer stands
    }
    LARGE_INTEGER start{}, stop{};
    QueryPerformanceCounter(&start);
    const std::uint32_t distAt = kMeshDist, angleAt = kMeshAngle;
    const float         distOne  = ReadField<float>(mesh, distAt);
    const float         angleOne = ReadField<float>(mesh, angleAt);
    float               dist     = distOne;
    float               angle    = angleOne;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if ((read & (1u << seat)) == 0)
        {
            continue;
        }
        original(mesh, cameras[seat].location, cameras[seat].forward);
        dist  = std::min(dist, ReadField<float>(mesh, distAt));
        angle = std::min(angle, ReadField<float>(mesh, angleAt));
    }
    WriteField<float>(mesh, distAt, dist);
    WriteField<float>(mesh, angleAt, angle);
    QueryPerformanceCounter(&stop);
    g_tick.calls.fetch_add(1, std::memory_order_relaxed);
    g_tick.nearer.fetch_add(dist < distOne || angle < angleOne ? 1u : 0u, std::memory_order_relaxed);
    g_tick.onWorkers.fetch_add(players::generation::OnGameThread() ? 0u : 1u, std::memory_order_relaxed);
    g_tick.ticks.fetch_add(static_cast<std::uint64_t>(stop.QuadPart - start.QuadPart), std::memory_order_relaxed);
    if (platform::Enabled(platform::Level::Debug))
    {
        NoteMesh(mesh, angleOne, angle, cameras, read);
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

/// Each registered actor near a seat's camera gets that seat's {distance, angle} in its pair list, one pair per seat in
/// seat order (game thread, before the parallel evaluation: nothing else touches the lists or the meshes' two fields
/// now). The evaluation takes the minimum over the whole list (it walks Num entries, RE-p2-anim-optimisation-two-views.md
/// §1.2), and the list grows through the game's own grow, so a pair per seat fits.
void AppendSeatPairs() noexcept
{
    charapop::Camera cameras[players::kMaxPlayers];
    std::uint8_t     read = 0;  // bit s: seat s's camera was read
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (players::generation::Active(seat))
        {
            cameras[seat] = charapop::CameraOf(players::generation::ControllerOf(seat));
            read |= cameras[seat].read ? static_cast<std::uint8_t>(1u << seat) : std::uint8_t{0};
        }
    }
    auto*       world = static_cast<std::byte*>(ReadPointer(reinterpret_cast<const void*>(g_worldGlobal), 0));
    const void* cls   = ReadPointer(reinterpret_cast<const void*>(g_classGlobal), 0);
    if (read == 0 || world == nullptr || cls == nullptr)
    {
        ++g_counts.skipped;
        return;
    }
    if (ReadPointer(world, kSetData) == nullptr)
    {
        ++g_counts.skipped;  // the walk below refuses an unreadable bit count itself
        return;
    }
    g_items.clear();
    const auto visit = [&](std::uintptr_t element) {
        void* actor = scopes::leader_scope::ObjectAt(reinterpret_cast<const std::byte*>(element) + kElemActor);
        if (actor == nullptr)
        {
            return true;
        }
        ++g_counts.elements;
        float at[3]{};
        if (!charapop::ActorLocation(actor, at))
        {
            return true;
        }
        std::uint8_t reach = 0;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            // Written as "not beyond" so a NaN distance is kept, as the one-camera test kept it.
            if ((read & (1u << seat)) != 0 && !(charapop::Distance(at, cameras[seat].location) > kReach))
            {
                reach |= static_cast<std::uint8_t>(1u << seat);
            }
        }
        if (reach == 0)
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
        void* mesh = scopes::leader_scope::ObjectAt(static_cast<std::byte*>(pairs) + kDataMesh);
        if (mesh != nullptr && g_items.size() < g_items.capacity())
        {
            g_items.push_back({pairs, mesh, reach});  // within the capacity reserved at install: never allocates
        }
        return true;
    };
    platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(world) + kSetData, kElemSize, visit, kMaxBits);
    // One pair per actor and seat (several registrations can share an actor).
    std::sort(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return a.data < b.data; });
    const auto end = std::unique(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return a.data == b.data; });
    const std::uint32_t distAt = kMeshDist, angleAt = kMeshAngle;
    for (auto it = g_items.begin(); it != end; ++it)
    {
        ++g_counts.inReach;
        for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
        {
            if ((it->seats & (1u << seat)) == 0)
            {
                continue;
            }
            const charapop::Camera& camera      = cameras[seat];
            const float             location[4] = {camera.location[0], camera.location[1], camera.location[2], 0.0f};
            const float             forward[4]  = {camera.forward[0], camera.forward[1], camera.forward[2], 0.0f};
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
}

/// The allocator's parallel evaluation (game thread): the seats' pairs first, when the lists were refilled this frame.
std::uintptr_t Evaluate(void* a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    if (g_split && players::generation::AnyActive() && players::generation::OnGameThread())
    {
        ++g_counts.evaluations;
        g_counts.lagSum += std::int64_t(g_frame.load(std::memory_order_relaxed)) -
                           std::int64_t(g_refilled.load(std::memory_order_acquire));
        if (g_fresh.exchange(false, std::memory_order_acq_rel))
        {
            LARGE_INTEGER start{}, stop{}, rate{};
            QueryPerformanceCounter(&start);
            AppendSeatPairs();
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
    const auto world = image.Rebase(kAnimWorldManager, sizeof(void*));
    const auto cls   = image.Rebase(kInterfaceClassGlobal, sizeof(void*));
    const auto iface = platform::FindFunction(image, kInterfaceOf);
    const auto meas  = platform::FindFunction(image, kMeasure);
    const auto grow  = CallTarget(image, kGrowCall);
    const auto growMapped = platform::FindFunction(image, kGrow);
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
        const auto replacement = reinterpret_cast<std::uintptr_t>(&MeasureEveryView);
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
    if (!players::generation::OnGameThread())
    {
        return;
    }
    g_frame.fetch_add(1, std::memory_order_relaxed);
    g_split        = splitShown;
    bool published = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (splitShown && players::generation::Active(seat))
        {
            void* const            controller = players::generation::ControllerOf(seat);
            const charapop::Camera camera     = charapop::CameraOf(controller);
            PublishSeatCamera(seat, camera.read, camera, players::generation::MemberOf(seat),
                              ReadPointer(controller, facts::kControllerPawn));
            published = true;
        }
        else
        {
            PublishSeatCamera(seat, false, charapop::Camera{}, nullptr, nullptr);
        }
    }
    if (!published)
    {
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
        const wchar_t* const who = players::kSeatsBuilt == 1 ? L"player 2" : L"the further players";
        platform::LogDebug(L"anim tick time ({} s): {} measures from player 1's snapshot also from {}'s camera ({} on "
                           L"workers), {} of them nearer for {}; {:.3f} ms of thread time per frame on average; {} left "
                           L"player 1's ({}'s camera was being written)",
                           kSummaryEvery.count(), calls, who, worker, nearer, who,
                           ms / std::max(1.0, double(g_counts.evaluations)), torn, who);
    }
    const auto refills = platform::Take(g_refills);
    if (g_counts.frames != 0 || g_counts.skipped != 0)
    {
        const double frames = g_counts.frames != 0 ? g_counts.frames : 1;
        const wchar_t* const who = players::kSeatsBuilt == 1 ? L"player 2" : L"the further players";
        platform::LogDebug(L"anim two views ({} s): {} frame(s) with {}'s pairs ({} skipped; {} evaluation(s), {} "
                           L"refill(s) of the game's lists, the refill's frame lagging {:.2f} on average); per frame {:.0f} "
                           L"registered actor(s), {:.0f} near {}'s camera, {:.0f} pair(s) added; {:.3f} ms per frame "
                           L"(worst {:.3f})",
                           kSummaryEvery.count(), g_counts.frames, who, g_counts.skipped, g_counts.evaluations, refills,
                           g_counts.evaluations != 0 ? double(g_counts.lagSum) / g_counts.evaluations : 0.0,
                           double(g_counts.elements) / frames,
                           double(g_counts.inReach) / frames, who, double(g_counts.appended) / frames,
                           g_counts.micros / frames / 1000.0, g_counts.maxMicros / 1000.0);
    }
    g_counts = {};
}

}  // namespace lss::world::anim_every_view
