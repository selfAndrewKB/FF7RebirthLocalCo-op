#include "player_two/anim_two_views.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "player_two/charapop_facts.h"
#include "player_two/generation.h"
#include "player_two/leader_scope.h"
#include "player_two/object_fields.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lss::player_two::anim_two_views {

namespace {

// 1.004 addresses; 1.005 through replaced-anim-two-views.json (every call site and function read from both exes).
constexpr std::uintptr_t kAnimWorld   = 0x14939BC48;  // the animation world manager W
constexpr std::uintptr_t kClearCall   = 0x140A32B57;  // PropagateLinkedActorCamDist's last call: its arrays are refilled
constexpr std::uintptr_t kClearTarget = 0x14079F7F0;  // ME_FreeResourceBindingArray
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

// W's registrations: a TSparseArray of 56-byte elements (data, then the allocation TBitArray: inline words at +0x808, a
// heap pointer at +0x818 when used, the bit count at +0x820). An element's actor is a weak pointer at +0x18.
constexpr std::uint32_t kSetData     = 0x7F8;
constexpr std::uint32_t kSetBits     = 0x808;
constexpr std::uint32_t kSetBitsHeap = 0x818;
constexpr std::uint32_t kSetNumBits  = 0x820;
constexpr std::uint32_t kElemSize    = 56;
constexpr std::uint32_t kElemActor   = 0x18;
constexpr std::int32_t  kMaxBits     = 1 << 16;  // sanity bound on the set's bit count

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
std::chrono::steady_clock::time_point g_summaryAt{};

/// The game's last call in its refill (a worker, on a normal pass): the pair lists are this frame's.
std::uintptr_t Refilled(void* a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_refilled.store(g_frame.load(std::memory_order_relaxed), std::memory_order_release);
    g_refills.fetch_add(1, std::memory_order_relaxed);
    g_fresh.store(true, std::memory_order_release);
    return reinterpret_cast<PassFn>(g_clearCall.Original())(a, b, c, d);
}

bool BitSet(const std::uint32_t* words, std::int32_t i) noexcept
{
    return ((words[i >> 5] >> (i & 31)) & 1u) != 0;
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
    auto*      data    = static_cast<std::byte*>(ReadPointer(world, kSetData));
    const auto numBits = ReadField<std::int32_t>(world, kSetNumBits);
    auto*      words   = static_cast<const std::uint32_t*>(ReadPointer(world, kSetBitsHeap));
    if (words == nullptr)
    {
        words = reinterpret_cast<const std::uint32_t*>(world + kSetBits);
    }
    if (data == nullptr || numBits <= 0 || numBits > kMaxBits)
    {
        ++g_counts.skipped;
        return;
    }
    const float location[4] = {two.location[0], two.location[1], two.location[2], 0.0f};
    const float forward[4]  = {two.forward[0], two.forward[1], two.forward[2], 0.0f};
    g_items.clear();
    for (std::int32_t i = 0; i < numBits; ++i)
    {
        if (!BitSet(words, i))
        {
            continue;
        }
        void* actor = leader_scope::ObjectAt(data + static_cast<std::size_t>(i) * kElemSize + kElemActor);
        if (actor == nullptr)
        {
            continue;
        }
        ++g_counts.elements;
        float at[3]{};
        if (!charapop::ActorLocation(actor, at) || charapop::Distance(at, two.location) > kReach)
        {
            continue;
        }
        void* iface = nullptr;
        if (!platform::CallGuarded(g_interfaceOf, iface, static_cast<const void*>(actor), cls) || iface == nullptr)
        {
            continue;
        }
        const auto   valid = reinterpret_cast<IfaceValidFn>(ReadPointer(ReadPointer(iface, 0), kIfaceValid));
        const auto   get   = reinterpret_cast<IfaceDataFn>(ReadPointer(ReadPointer(iface, 0), kIfaceData));
        std::uint8_t ok    = 0;
        void*        pairs = nullptr;
        if (valid == nullptr || get == nullptr || !platform::CallGuarded(valid, ok, iface) || ok == 0 ||
            !platform::CallGuarded(get, pairs, iface) || pairs == nullptr)
        {
            continue;
        }
        void* mesh = leader_scope::ObjectAt(static_cast<std::byte*>(pairs) + kDataMesh);
        if (mesh != nullptr && g_items.size() < g_items.capacity())
        {
            g_items.push_back({pairs, mesh});  // within the capacity reserved at install: never allocates
        }
    }
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

bool Redirect(const platform::GameImage& image, platform::CallRedirect& redirect, std::uintptr_t site,
              std::uintptr_t target, std::uintptr_t replacement) noexcept
{
    const auto at = image.Rebase(site, 5);
    const auto to = image.Rebase(target);
    return at && to && redirect.Install(*at, *to, replacement);
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
    const bool refill = Redirect(image, g_clearCall, kClearCall, kClearTarget, reinterpret_cast<std::uintptr_t>(&Refilled));
    const bool eval   = refill && Redirect(image, g_evalCall, kEvalCall, kEvalTarget, reinterpret_cast<std::uintptr_t>(&Evaluate));
    if (refill && !eval)
    {
        g_clearCall.Restore();
    }
    platform::Log(L"anim two views: {}",
                  eval ? L"characters near player 2 animate by the nearer of the two cameras (the game's own pair list)"
                       : L"NOT installed (a call differs); characters near player 2 keep player 1's camera's rate");
}

void Remove() noexcept
{
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
    g_split   = splitShown;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_summaryAt < kSummaryEvery)
    {
        return;
    }
    g_summaryAt = now;
    const auto refills = g_refills.exchange(0, std::memory_order_relaxed);
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
