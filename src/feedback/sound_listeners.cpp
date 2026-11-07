#include "feedback/sound_listeners.h"

#include "feedback/listener_rules.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/every.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/seqlock.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "views/columns.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace lss::feedback::sound_listeners {

namespace {

using listener_rules::Vec3;

constexpr int kSeats     = players::kSeatsBuilt;          // the seats, 1..kSeats
constexpr int kListeners = kSeats + 1;                    // 0: player 1's (SEAD's own), then each seat's copy
constexpr int kColumns   = views::columns::kMaxColumns;   // the engine listeners read: one per column

// 1.004 addresses (1.005 through replaced-audio-listener.json and replaced-sound-listeners.json; the slot table and
// the gate C measure from research/RE-listener-per-seat.md §2.3 and §4.3, each read in both exes).
constexpr std::uintptr_t kPushSlot          = 0x1465E6058;  // SEAD audio device vt+0x1B8
constexpr std::uintptr_t kPush              = 0x14082ECE4;  // SeadAudioDevice_PushListener0ToSead(device)
constexpr std::uintptr_t kDynamicTestSlot   = 0x1457DA960;  // SQEX::Sd::Lay::DynamicSound vt+0x160
constexpr std::uintptr_t kDynamicTest       = 0x140B94018;  // SEAD_LayDynamic_UpdateAudible(object)
constexpr std::uintptr_t kDynamicUpdateSlot = 0x1457DA858;  // Lay::DynamicSound vt+0x58
constexpr std::uintptr_t kDynamicUpdate     = 0x1414624C0;  // SEAD_LayDynamic_Update_Doppler(object, dt): then the base
constexpr std::uintptr_t kPointTestSlot     = 0x1457DA5D0;  // Lay::PointSound vt+0x160
constexpr std::uintptr_t kPointTest         = 0x14127585C;  // SEAD_LayPoint_UpdateAudible(object): Point and SphWedge
constexpr std::uintptr_t kPointUpdateSlot   = 0x1457DA4C8;  // Lay::PointSound vt+0x58
constexpr std::uintptr_t kWedgeTestSlot     = 0x1457DA250;  // Lay::SphericalWedgeSound vt+0x160 (Point's test)
constexpr std::uintptr_t kWedgeUpdateSlot   = 0x1457DA148;  // Lay::SphericalWedgeSound vt+0x58 (the base update)
constexpr std::uintptr_t kPolygonTestSlot   = 0x1457DA410;  // Lay::PolygonSound vt+0x160
constexpr std::uintptr_t kSectorTestSlot    = 0x1457DA090;  // Lay::CylindricalSectorSound vt+0x160 (Polygon's test)
constexpr std::uintptr_t kAreaTest          = 0x14073F16C;  // SEAD_LayPolygon_UpdateAudible(object)
constexpr std::uintptr_t kBaseUpdate        = 0x1407AC83C;  // SEAD_LayObj_Update_Base(object, dt): Calc3D, wedge gain, voice
constexpr std::uintptr_t kVelocityCall      = 0x14082D51B;  // SEAD_Lay_UpdateFrame: E8 SEAD_LayListener_UpdateVelocity
constexpr std::uintptr_t kVelocity          = 0x14161261C;  // SEAD_LayListener_UpdateVelocity(): the singleton's velocity
constexpr std::uintptr_t kListenerGlobal    = 0x14939BAB0;  // g_SeadLayListener: the one SQEX::Sd::Lay::Listener
constexpr std::uintptr_t kClipReturn        = 0x140807DE8;  // the AutoSe processor update's distance clip, after its read
constexpr std::uintptr_t kClipUpdateCall    = 0x1408072F8;  // SeadAutoSeProc_TickById: E8 the processor update
constexpr std::uintptr_t kClipUpdate        = 0x140807D24;  // SeadAutoSeProc_Update_ClipAndLodFilter(proc, dt, frame, ...)
constexpr std::uintptr_t kMeasureCall       = 0x140807F05;  // the processor update: E8 the frame measure (gate C)
constexpr std::uintptr_t kMeasureReturn     = 0x140808365;  // SeadAutoSeProc_AnalyzeFrame_ListenerDistSq, after its read
constexpr std::uintptr_t kConvert           = 0x1470E3C48;  // AutoSe's position converter (a pointer; a copy by default)
constexpr std::uintptr_t kLodSlot           = 0x145D72670;  // UEndAutoSeCallback vt+0x2A8
constexpr std::uintptr_t kLod               = 0x143D6E0EC;  // EndAutoSeCallback_GetProcLODLevel_P1Listener(callback, char)
constexpr std::uintptr_t kPcListener        = 0x140BF1968;  // the player controller's GetAudioListenerPosition
inline constexpr platform::PerBuild<std::uint32_t> kPcListenerSlot{0xC70, 0xC78};

// SEAD_LayListener_Set(listener, position, front, up): writes the listener's position, previous position, front, up
// and matrix; 0, or -1 if the matrix fails.
constexpr std::uint8_t kListenerSetBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xD9, 0xB8};
constexpr platform::FunctionFact kListenerSet{L"SEAD_LayListener_Set", 0x1413E3AD0, kListenerSetBytes};
// SeadLayListener_GetPosition(out): the singleton's position, through SEAD's coordinate hook. Its first instruction,
// `mov [rsp+10h], rbx` (5 bytes, no RIP-relative operand, no branch lands inside it), takes the entry redirect.
constexpr std::uint8_t kGetPositionBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x60, 0x00, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kGetPosition{L"SeadLayListener_GetPosition", 0x140808FA0, kGetPositionBytes};
constexpr std::size_t kGetPositionFirst = 5;
// The gate C measure (record, analysis): `mov [rsp+18h], rbx; push rbp, rsi, rdi, r12, r13`, the same in both builds.
constexpr std::uint8_t kMeasureBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr platform::FunctionFact kMeasure{L"SeadAutoSeProc_AnalyzeFrame_ListenerDistSq", 0x140808208, kMeasureBytes};

using PushFn       = std::uint64_t (*)(void* device);
using TestFn       = std::uint64_t (*)(void* object);               // UpdateAudible (vt+0x160)
using UpdateFn     = std::uint64_t (*)(void* object, float seconds);  // Update (vt+0x58): both read xmm1 at entry
using VelocityFn   = std::uint64_t (*)(void*, void*, void*);  // takes no arguments; the registers pass through
using SetFn        = std::int32_t (*)(void* listener, float px, float py, float pz, float fx, float fy, float fz, float ux,
                                     float uy, float uz);
using PositionFn   = std::uint8_t (*)(void* out);
using ClipUpdateFn = std::uint64_t (*)(void* processor, float dt, void* frame, std::int32_t count, std::int32_t fifth,
                                       void* sixth);
using MeasureFn    = std::uint64_t (*)(void* record, void* analysis, void* third, void* fourth);  // no float argument
using ConvertFn    = const float* (*)(float* out, const float* in);  // 12-byte vectors
using LodFn        = std::uint64_t (*)(void* callback, void* character);
using PcListenerFn = void (*)(void* controller, void* location, void* front, void* right);

// Fields (RE-p2-audio-listener.md §2-§3, RE-listener-per-seat.md; the same in both builds).
constexpr std::uint32_t kGameListeners       = 0x80;   // FAudioDevice ListenerTransforms (game thread): data, then num
constexpr std::uint32_t kGameListenerStride  = 0x40;
constexpr std::uint32_t kAudioListeners      = 0x130;  // FAudioDevice Listeners (audio thread): data, then num
constexpr std::uint32_t kAudioListenerStride = 0xC0;
constexpr std::uint32_t kRotation            = 0x00;   // FTransform quaternion (x, y, z, w)
constexpr std::uint32_t kTranslation         = 0x10;   // FTransform translation, in both arrays' elements
constexpr std::uint32_t kListenerSize        = 0xB0;   // SQEX::Sd::Lay::Listener
constexpr std::uint32_t kListenerPosition    = 0x30;
constexpr std::uint32_t kListenerMagnify     = 0xA0;
constexpr std::uint32_t kObjectAudible       = 0x1F0;  // u8: the answer UpdateAudible wrote
constexpr std::uint32_t kObjectRange         = 0x1F4;  // the audible range
constexpr std::uint32_t kObjectPosition      = 0x1E0;  // the position the test measured (written by UpdateAudible)
constexpr std::uint32_t kDynamicSource       = 0x220;  // DynamicSound: the world source position
constexpr std::uint32_t kPointSource         = 0x210;  // PointSound, SphericalWedgeSound: the placed position
constexpr std::uint32_t kDynamicRelative     = 0x259;  // u8 1: a listener-relative source
constexpr std::uint32_t kDynamicIgnoreMag    = 0x25B;  // u8: ignores the magnification
constexpr std::uint32_t kFramePosition       = 0x35C;  // the AutoSe frame's character position, which the clip measures
constexpr std::uint32_t kFrameMeasured       = 0x350;  // the AutoSe frame's point the gate C measure reads
constexpr std::uint32_t kAnalysisFrame       = 0x20;   // the measure's second argument: its frame
constexpr auto          kSummaryEvery        = std::chrono::seconds(10);
constexpr float         kMetres              = 0.01f;
constexpr float         kFreshCm     = 100.0f;  // a seat's engine listener this near its controller's own follows its camera
constexpr int           kStaleTicks  = 8;       // ticks it may differ (a camera cut lands a tick apart) before it goes off
constexpr float         kSamePlaceCm = 500.0f;  // the push's entry at a seat's column this near the game thread's check is it
constexpr int           kSnapshotFrames = 4;    // SEAD frames a seat's listener may be set from the last good push read
constexpr int           kPositionTries  = 256;  // spins while a swap runs, before the latest clean answer
constexpr std::size_t   kChoiceBits  = 12;      // 4096 remembered choices; ~250 live SEAD objects
constexpr std::size_t   kChoiceProbe = 8;
constexpr float         kInfinite    = std::numeric_limits<float>::infinity();

// The SEAD sound classes, in the order of their slots below. The first kChosenKinds take the nearest listener; the
// area sounds are measured from player 1's (their test measures a shape's point nearest the listener: a choice would
// need a trial per listener, saving +0x1E0..+0x1FF and +0x520..+0x53F) and are counted only.
enum Kind : int
{
    kMoving,   // DynamicSound: every spatialized engine sound
    kPlaced,   // PointSound
    kWedge,    // SphericalWedgeSound
    kPolygon,  // PolygonSound (an area)
    kSector,   // CylindricalSectorSound (an area)
    kKinds
};
constexpr int kChosenKinds = kPolygon;

struct KindFacts
{
    const wchar_t* name;
    std::uintptr_t testSlot;
    std::uintptr_t test;
    std::uintptr_t updateSlot;  // 0: not chosen
    std::uintptr_t update;
    std::uint32_t  source;      // where the sound is (the area sounds: the point their test measured, after it)
};
constexpr KindFacts kKindFacts[kKinds] = {
    {L"moving", kDynamicTestSlot, kDynamicTest, kDynamicUpdateSlot, kDynamicUpdate, kDynamicSource},
    {L"placed", kPointTestSlot, kPointTest, kPointUpdateSlot, kBaseUpdate, kPointSource},
    {L"wedge", kWedgeTestSlot, kPointTest, kWedgeUpdateSlot, kBaseUpdate, kPointSource},
    {L"polygon area", kPolygonTestSlot, kAreaTest, 0, 0, kObjectPosition},
    {L"sector area", kSectorTestSlot, kAreaTest, 0, 0, kObjectPosition},
};

// The hooks whose thread is noted once.
enum Hook : int
{
    kPushHook,
    kAudibleHook,
    kVelocityHook,
    kPositionHook,
    kLodHook,
    kHooks
};
constexpr const wchar_t* kHookNames[kHooks] = {L"the listener push", L"the sound classes' audibility tests",
                                               L"the listeners' SEAD frame", L"the AutoSe listener position",
                                               L"the footstep LOD"};

// The audio thread's view after each push: the engine's listener count; per column its position and the front and up
// the push would send SEAD for it; SEAD's own listener position. NaN: not read.
enum ListenerField : std::size_t
{
    kCount,
    kColumnX,  // column c: position at kColumnX + kPerColumn c, then front, then up
    kPerColumn      = 9,
    kSeadX          = kColumnX + kPerColumn * kColumns,
    kListenerFields = kSeadX + 3
};
constexpr std::size_t kFrontAt = 3;
constexpr std::size_t kUpAt    = 6;
static_assert(kColumns == 3, "the Trace line names three engine listeners");
// The latest sound another player's listener would have heard but the chosen one cut (SEAD thread).
enum ExampleField : std::size_t
{
    kExampleKind,
    kExampleOwner,   // the listener that would have heard it
    kExampleChosen,  // the listener it was measured from
    kExampleRange,
    kExampleToChosen,
    kExampleToOwner,
    kExampleX,
    kExampleFields = kExampleX + 3
};

platform::SeqFloats<kListenerFields> g_listeners;
platform::SeqFloats<kExampleFields>  g_example;
// Each seat's listener place as the game thread checked it this tick (NaN: off), for the SEAD thread: the audio side's
// entry at the seat's column trails it, and after a column change still holds the previous view's place for a moment.
platform::SeqFloats<3 * kListeners>  g_checked;

// Placed once at install on the game thread, before the hooks that read them go live.
platform::VTableEntry                           g_pushSlot, g_lodSlot, g_pcSlot;
std::array<platform::VTableEntry, kKinds>       g_testSlots;
std::array<platform::VTableEntry, kChosenKinds> g_updateSlots;
platform::CallRedirect                          g_velocityCall, g_clipCall, g_measureCall;
platform::EntryRedirect                         g_positionEntry;
std::atomic<void**>     g_listenerGlobal{nullptr};  // the address of SEAD's listener pointer; written by Install
std::uintptr_t          g_convert      = 0;         // live address of AutoSe's converter pointer
SetFn                   g_set          = nullptr;
std::uintptr_t          g_clipReturn   = 0;
std::uintptr_t          g_measureReturn = 0;
std::uintptr_t          g_pcListenerFn = 0;         // live; the slot's expected function
std::atomic<std::uintptr_t> g_pcVTable{0};          // the controller vtable whose listener slot is ours
std::atomic<bool>       g_installed{false};
// The levers that may read another player's listener (cleared first at remove). Each kind's choice needs its test and
// its update both placed: an update must run from the listener its test chose.
std::atomic<bool>                           g_seatsAllowed{false};
std::array<std::atomic<bool>, kChosenKinds> g_paired{};
std::atomic<bool>                           g_autoSeAllowed{false};
// A hook could not be put back at remove: the updates still ours answer as the game's from then on.
std::atomic<bool>                           g_leftBehind{false};

std::atomic<void*>                     g_device{nullptr};  // the first audio device whose push ran (the game's SEAD one)
std::array<std::atomic<DWORD>, kHooks> g_threads{};
std::array<std::atomic<bool>, kHooks>  g_otherThread{};  // a hook ran on a second thread
// Each seat's column while its listener is on (-1: off), written by the game thread, read by the SEAD thread.
std::array<std::atomic<int>, kListeners> g_seatColumn{};
std::atomic<unsigned> g_swap{0};  // odd while SEAD's listener pointer names a seat's listener (a seqlock)

// SEAD's thread only: each seat's listener, a copy of the game's object set every SEAD frame; whether it was set this
// frame; each sound's listener, chosen in its test and used again in its update.
struct alignas(16) ListenerObject
{
    std::array<std::byte, kListenerSize> bytes{};
};
std::array<ListenerObject, kListeners>    g_copies{};
std::array<bool, kListeners>              g_copied{};
std::array<bool, kListeners>              g_on{};
std::array<std::atomic<bool>, kListeners> g_onSeen{};  // g_on for the game thread's log
listener_rules::ChoiceTable<kChoiceBits, kChoiceProbe> g_choices;
std::uint32_t                      g_frame = 1;       // SEAD frames, counted where the seats' listeners are set
std::array<float, kListenerFields> g_lastSeen{};      // the last push read that succeeded
int                                g_lastSeenAge = kSnapshotFrames + 1;
std::array<float, 3 * kListeners>  g_lastChecked{};   // the last check read that succeeded

// The AutoSe position's latest clean answer, per calling thread.
struct Answer
{
    bool                                  held = false;
    std::uint8_t                          result = 0;
    alignas(16) std::array<std::byte, 16> position{};
};
thread_local Answer t_answer;
// Inside an AutoSe processor update: its frame, whose character the distance clip measures (null outside it).
thread_local const void* t_clipFrame = nullptr;
// Inside the gate C measure: its frame, whose point it measures (null outside it).
thread_local const void* t_measureFrame = nullptr;
// A seat's LOD pass: the controller the PC listener slot answers for (null outside it).
thread_local void* t_lodController = nullptr;

struct KindCounts
{
    std::atomic<unsigned>                         checked{0};
    std::atomic<unsigned>                         relative{0};  // listener-relative moving sounds (player 1's mix)
    std::array<std::atomic<unsigned>, kListeners> measured{};   // measured from listener k
    std::array<std::atomic<unsigned>, kListeners> heard{};      //   ... and heard
    std::array<std::atomic<unsigned>, kListeners> missed{};     // cut by the chosen listener, within listener k's reach
};
std::array<KindCounts, kKinds> g_kinds;

struct Counts
{
    std::atomic<unsigned>                         pushes{0};
    std::atomic<unsigned>                         otherDevicePushes{0};  // a push of another device, not read
    std::atomic<unsigned>                         setFailures{0};        // the game's setter refused a seat's values
    std::array<std::atomic<unsigned>, kListeners> updates{};             // sound updates run from seat k's listener
    std::atomic<unsigned>                         untested{0};           // updates whose sound was not tested this frame
    std::atomic<unsigned>                         tableFull{0};          // choices refused for want of an entry
    std::atomic<unsigned>                         examples{0};
    std::atomic<unsigned>                         reads{0};              // AutoSe listener position reads
    std::atomic<unsigned>                         readWaits{0};          //   ... that waited out a swap
    std::atomic<unsigned>                         readHeld{0};           //   ... answered with the latest clean answer
    std::atomic<unsigned>                         clipReads{0};          // gate A
    std::array<std::atomic<unsigned>, kListeners> clipFrom{};            //   ... answered with seat k's listener
    std::atomic<unsigned>                         measures{0};           // gate C
    std::array<std::atomic<unsigned>, kListeners> measureFrom{};         //   ... answered with seat k's listener
    std::atomic<unsigned>                         lods{0};               // gate B: levels asked
    std::array<std::atomic<unsigned>, kListeners> lodAsked{};            //   ... asked again for seat k
    std::atomic<unsigned>                         lodLowered{0};         //   ... lowered by a seat's level
    std::atomic<unsigned>                         lodElsewhere{0};       //   ... off the game thread
};
Counts           g_counts;
std::atomic<int> g_lodKept{-2}, g_lodLower{-2};  // the latest lowered level pair

// Game thread.
std::array<void*, kListeners> g_controllers{};
std::array<Vec3, kListeners>  g_at{};          // each listener's place this tick (0: the engine's first)
std::array<bool, kListeners>  g_live{};        //   ... and whether it is on
std::array<int, kListeners>   g_stale{};       // ticks a seat's engine listener has not followed its camera
std::array<int, kListeners>   g_columnSeen{};  // each seat's column last tick (-1 none)
std::array<int, kListeners>   g_stateLogged{};
std::array<int, kListeners>   g_onLogged{};
int                           g_gameCountLogged = -1;
std::array<bool, kHooks>      g_threadLogged{};
std::array<bool, kHooks>      g_otherLogged{};
platform::Every               g_summary{kSummaryEvery};
unsigned                      g_exampleSeen = 0;
std::uintptr_t                g_pcTried     = 0;  // the controller vtable last tried
// The window's largest gaps, cm: each seat's entry against its controller's own listener; each column no seat's own
// view fills against entry 0. -1: not read this window.
std::array<float, kListeners> g_controllerGap{};
std::array<float, kColumns>   g_otherColumnGap{};

void Note(Hook hook) noexcept
{
    auto&       slot = g_threads[static_cast<std::size_t>(hook)];
    const DWORD here = GetCurrentThreadId();
    DWORD       seen = slot.load(std::memory_order_relaxed);
    if (seen == 0 && slot.compare_exchange_strong(seen, here, std::memory_order_relaxed))
    {
        return;
    }
    if (seen != here)
    {
        g_otherThread[static_cast<std::size_t>(hook)].store(true, std::memory_order_relaxed);
    }
}

bool OnThreadOf(Hook hook) noexcept
{
    return g_threads[static_cast<std::size_t>(hook)].load(std::memory_order_relaxed) == GetCurrentThreadId();
}

/// On SEAD's frame thread (the one that sets the seats' listeners): the choices, the copies and the swaps are its alone.
bool OnSeadFrame() noexcept
{
    return OnThreadOf(kVelocityHook);
}

Vec3 Read3(const void* base, std::uint32_t offset) noexcept
{
    Vec3 out{};
    std::memcpy(out.data(), static_cast<const std::byte*>(base) + offset, sizeof(out));
    return out;
}

Vec3 Read3Guarded(const void* base, std::uint32_t offset, bool& ok) noexcept
{
    Vec3 out{};
    ok = base != nullptr && platform::CopyGuarded(out.data(), static_cast<const std::byte*>(base) + offset, sizeof(out));
    return out;
}

template <std::size_t N>
Vec3 At(const std::array<float, N>& seen, std::size_t index) noexcept
{
    return {seen[index], seen[index + 1], seen[index + 2]};
}

bool Valid(const Vec3& at) noexcept
{
    return std::isfinite(at[0]) && std::isfinite(at[1]) && std::isfinite(at[2]);
}

/// SEAD's listener singleton (created and destroyed on SEAD's thread at its start and end only).
const void* ListenerOne() noexcept
{
    void** const global = g_listenerGlobal.load(std::memory_order_relaxed);
    return global != nullptr ? std::atomic_ref<void*>(*global).load(std::memory_order_relaxed) : nullptr;
}

/// SEAD's thread: runs the game's code with SEAD's listener pointer naming seat `k`'s listener, then restores it. The
/// swap counter is odd meanwhile, for the AutoSe reader on other threads. A swap inside a swap (no game path is known
/// to make one) only moves the pointer: the outer one keeps the counter odd until the first restore.
template <class Run>
std::uint64_t Swapped(int k, Run&& run) noexcept
{
    void** const           global = g_listenerGlobal.load(std::memory_order_relaxed);
    std::atomic_ref<void*> pointer(*global);
    void* const            before = pointer.load(std::memory_order_relaxed);
    const auto             v      = g_swap.load(std::memory_order_relaxed);
    const bool             outer  = (v & 1u) == 0;
    if (outer)
    {
        g_swap.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }
    pointer.store(g_copies[static_cast<std::size_t>(k)].bytes.data(), std::memory_order_relaxed);
    const std::uint64_t result = run();
    pointer.store(before, std::memory_order_relaxed);
    if (outer)
    {
        g_swap.store(v + 2, std::memory_order_release);
    }
    return result;
}

/// Audio thread: the game's push of listener [0], then the engine's listeners as the push reads them (each column's
/// place, and the front and up the push would send SEAD for it) and SEAD's own listener. Only the first device's pushes
/// are read, by the push's first thread (the snapshot's one writer; a second thread is logged).
std::uint64_t Push(void* device) noexcept
{
    const auto result = reinterpret_cast<PushFn>(g_pushSlot.Original())(device);
    void*      first  = nullptr;
    if (!g_device.compare_exchange_strong(first, device, std::memory_order_relaxed) && first != device)
    {
        g_counts.otherDevicePushes.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    Note(kPushHook);  // after the device test: the thread noted is the SEAD device's
    if (!OnThreadOf(kPushHook))
    {
        return result;
    }
    g_counts.pushes.fetch_add(1, std::memory_order_relaxed);
    std::array<float, kListenerFields> out{};
    out.fill(std::nanf(""));
    const std::byte* data  = nullptr;
    std::int32_t     count = 0;
    if (!platform::CopyGuarded(&data, static_cast<const std::byte*>(device) + kAudioListeners, sizeof(data)) ||
        data == nullptr ||
        !platform::CopyGuarded(&count, static_cast<const std::byte*>(device) + kAudioListeners + 8, sizeof(count)))
    {
        count = 0;
    }
    out[kCount] = static_cast<float>(count);
    for (std::int32_t i = 0; i < kColumns && i < count; ++i)
    {
        const std::byte* element = data + static_cast<std::size_t>(kAudioListenerStride) * i;
        float            q[4]    = {};
        bool             ok      = false;
        const Vec3       at      = Read3Guarded(element, kTranslation, ok);
        if (!ok || !platform::CopyGuarded(q, element + kRotation, sizeof(q)))
        {
            continue;
        }
        const Vec3        front = listener_rules::SeadFront(q);
        const Vec3        up    = listener_rules::SeadUp(q);
        const std::size_t base  = kColumnX + kPerColumn * static_cast<std::size_t>(i);
        std::copy(at.begin(), at.end(), out.begin() + base);
        std::copy(front.begin(), front.end(), out.begin() + base + kFrontAt);
        std::copy(up.begin(), up.end(), out.begin() + base + kUpAt);
    }
    // SEAD's own listener, never during a swap (the pointer names a seat's copy then): left unread.
    const auto swap = g_swap.load(std::memory_order_acquire);
    if (const void* listener = ListenerOne(); listener != nullptr && (swap & 1u) == 0)
    {
        bool       ok   = false;
        const Vec3 sead = Read3Guarded(listener, kListenerPosition, ok);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ok && g_swap.load(std::memory_order_relaxed) == swap)
        {
            std::copy(sead.begin(), sead.end(), out.begin() + kSeadX);
        }
    }
    g_listeners.Publish(out);
    return result;
}

/// SEAD's thread, after the game's own velocity update of its listener: each seat's listener set by the game's setter
/// from the engine's listener at the seat's column, then given its velocity by the game's update with the pointer
/// swapped to it (Rebirth's update reads the global), as SEAD's command processing and frame do for the singleton. A
/// seat that is off, or whose values the setter refuses, is off for the frame (its sounds go back to the nearest
/// listener that is on). A push read that crosses a publish uses the last good one, up to kSnapshotFrames old.
void SetSeatListeners(void* a, void* b, void* c) noexcept
{
    const void* const                  one = ListenerOne();
    std::array<float, kListenerFields> fresh{};
    if (g_listeners.Read(fresh))
    {
        g_lastSeen    = fresh;
        g_lastSeenAge = 0;
    }
    else if (g_lastSeenAge <= kSnapshotFrames)
    {
        ++g_lastSeenAge;
    }
    const auto& seen = g_lastSeen;
    const bool  read = g_seatsAllowed.load(std::memory_order_relaxed) && one != nullptr && g_set != nullptr &&
                      g_lastSeenAge <= kSnapshotFrames;
    std::array<float, 3 * kListeners> checked{};
    if (g_checked.Read(checked))
    {
        g_lastChecked = checked;
    }
    const auto velocity = reinterpret_cast<VelocityFn>(g_velocityCall.Original());
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i      = static_cast<std::size_t>(seat);
        const int  column = g_seatColumn[i].load(std::memory_order_relaxed);
        bool       on     = false;
        if (read && column >= 1 && column < kColumns && static_cast<float>(column) < seen[kCount])
        {
            const std::size_t base  = kColumnX + kPerColumn * static_cast<std::size_t>(column);
            const Vec3        at    = At(seen, base);
            const Vec3        front = At(seen, base + kFrontAt);
            const Vec3        up    = At(seen, base + kUpAt);
            const Vec3        place = At(g_lastChecked, 3 * i);
            if (Valid(at) && Valid(front) && Valid(up) && Valid(place) &&
                listener_rules::Distance(at, place) <= kSamePlaceCm)
            {
                std::byte* const copy = g_copies[i].bytes.data();
                if (!g_copied[i])
                {
                    std::memcpy(copy, one, kListenerSize);  // the vtable, the identity matrix, the magnification
                    g_copied[i] = true;
                }
                // The magnification (SEAD's command 5 may change it on the singleton), per listener in storage only.
                std::memcpy(copy + kListenerMagnify, static_cast<const std::byte*>(one) + kListenerMagnify, sizeof(float));
                on = g_set(copy, at[0], at[1], at[2], front[0], front[1], front[2], up[0], up[1], up[2]) == 0;
                if (on)
                {
                    Swapped(seat, [&] { return velocity(a, b, c); });
                }
                else
                {
                    g_counts.setFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        g_on[i] = on;
        g_onSeen[i].store(on, std::memory_order_relaxed);
    }
}

std::uint64_t Velocity(void* a, void* b, void* c) noexcept
{
    const auto result = reinterpret_cast<VelocityFn>(g_velocityCall.Original())(a, b, c);
    Note(kVelocityHook);
    if (OnSeadFrame())
    {
        ++g_frame;
        SetSeatListeners(a, b, c);
    }
    return result;
}

/// SEAD's thread: listener k's object, if it is on this frame (0 is the singleton: always, while it exists).
const void* ListenerOf(int k) noexcept
{
    if (k == 0)
    {
        return ListenerOne();
    }
    return k > 0 && k < kListeners && g_on[static_cast<std::size_t>(k)] ? g_copies[static_cast<std::size_t>(k)].bytes.data()
                                                                       : nullptr;
}

/// SEAD's thread: each listener's distance to `source` (infinite for a listener that is off).
std::array<float, kListeners> Distances(const Vec3& source) noexcept
{
    std::array<float, kListeners> distances{};
    for (int k = 0; k < kListeners; ++k)
    {
        const void* const listener             = ListenerOf(k);
        distances[static_cast<std::size_t>(k)] = listener != nullptr
                                                     ? listener_rules::Distance(Read3(listener, kListenerPosition), source)
                                                     : kInfinite;
    }
    return distances;
}

/// The sound's audible range as the game's test compares it: a moving sound's times the listener's magnification
/// (every copy mirrors the singleton's) unless it ignores it.
float RangeOf(int kind, const void* object) noexcept
{
    const float range = ReadField<float>(object, kObjectRange);
    if (kind != kMoving || ReadField<std::uint8_t>(object, kDynamicIgnoreMag) != 0)
    {
        return range;
    }
    const void* const one = ListenerOne();
    return one != nullptr ? range * ReadField<float>(one, kListenerMagnify) : range;
}

/// After the game's test of one sound: its counts, and the sounds the chosen listener cut that another would hear.
void Count(int kind, const void* object, int chosen, bool here) noexcept
{
    KindCounts& counts = g_kinds[static_cast<std::size_t>(kind)];
    const auto  c      = static_cast<std::size_t>(chosen);
    counts.measured[c].fetch_add(1, std::memory_order_relaxed);
    if (ReadField<std::uint8_t>(object, kObjectAudible) != 0)
    {
        counts.heard[c].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float range = RangeOf(kind, object);
    if (!here || range <= 0.0f)
    {
        return;
    }
    const Vec3 source    = Read3(object, kKindFacts[kind].source);
    const auto distances = Distances(source);
    for (int k = 0; k < kListeners; ++k)
    {
        const float to = distances[static_cast<std::size_t>(k)];
        if (k == chosen || !listener_rules::Within(range, to))
        {
            continue;
        }
        counts.missed[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed);
        g_example.Publish({static_cast<float>(kind), static_cast<float>(k), static_cast<float>(chosen), range,
                           distances[c], to, source[0], source[1], source[2]});
        g_counts.examples.fetch_add(1, std::memory_order_relaxed);
    }
}

/// A sound's audibility test, from the nearest player's listener for the kinds that choose (the game's own test either
/// way). Its choice is kept for its update.
template <int K>
std::uint64_t Audible(void* object) noexcept
{
    const auto test = reinterpret_cast<TestFn>(g_testSlots[K].Original());
    Note(kAudibleHook);
    KindCounts& counts = g_kinds[K];
    counts.checked.fetch_add(1, std::memory_order_relaxed);
    const bool relative = K == kMoving && ReadField<std::uint8_t>(object, kDynamicRelative) == 1;
    const bool sead     = OnSeadFrame();
    const bool here     = sead && g_seatsAllowed.load(std::memory_order_relaxed) && ListenerOne() != nullptr;
    int        chosen   = 0;
    if (relative)
    {
        counts.relative.fetch_add(1, std::memory_order_relaxed);
    }
    else if constexpr (K < kChosenKinds)
    {
        if (here && g_paired[K].load(std::memory_order_relaxed))
        {
            chosen = listener_rules::ChooseNearest(g_choices.Held(object, g_frame),
                                                   Distances(Read3(object, kKindFacts[K].source)), RangeOf(K, object));
        }
    }
    // Every test on SEAD's frame thread records its listener (player 1's too), so the update that follows in this
    // frame runs from it, whatever the mod did before.
    if (sead && K < kChosenKinds && !g_choices.Record(object, chosen, g_frame))
    {
        chosen = 0;
        g_counts.tableFull.fetch_add(1, std::memory_order_relaxed);
    }
    if (relative)
    {
        return test(object);  // relative to player 1's camera, its update too
    }
    const std::uint64_t result = chosen == 0 ? test(object) : Swapped(chosen, [&] { return test(object); });
    Count(K, object, chosen, here);
    return result;
}

constexpr TestFn kAudibleHooks[kKinds] = {&Audible<kMoving>, &Audible<kPlaced>, &Audible<kWedge>, &Audible<kPolygon>,
                                          &Audible<kSector>};

/// A sound's update (doppler, gain, pan, the voice's start and stop), from the listener its test chose.
template <int K>
std::uint64_t Update(void* object, float seconds) noexcept
{
    const auto update = reinterpret_cast<UpdateFn>(g_updateSlots[K].Original());
    if (!OnSeadFrame() || g_leftBehind.load(std::memory_order_relaxed))
    {
        return update(object, seconds);
    }
    // The frame's own record, not the switches: a test that chose a seat's listener in this frame is followed by an
    // update from it even if the mod is removed between the two loops.
    const int k = g_choices.ChosenIn(object, g_frame);
    if (k == listener_rules::kNoChoice)
    {
        g_counts.untested.fetch_add(1, std::memory_order_relaxed);
    }
    if (k <= 0 || ListenerOf(k) == nullptr)
    {
        return update(object, seconds);
    }
    g_counts.updates[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed);
    return Swapped(k, [&] { return update(object, seconds); });
}

constexpr UpdateFn kUpdateHooks[kChosenKinds] = {&Update<kMoving>, &Update<kPlaced>, &Update<kWedge>};

/// The AutoSe processor update (gate A's caller): its frame is noted for the distance clip.
std::uint64_t ClipUpdate(void* processor, float dt, void* frame, std::int32_t count, std::int32_t fifth,
                         void* sixth) noexcept
{
    const void* const outer = t_clipFrame;
    t_clipFrame             = frame;
    const auto result =
        reinterpret_cast<ClipUpdateFn>(g_clipCall.Original())(processor, dt, frame, count, fifth, sixth);
    t_clipFrame = outer;
    return result;
}

/// The gate C measure (the rustle and swing range is compared with the distance it stores): its frame is noted.
std::uint64_t Measure(void* record, void* analysis, void* third, void* fourth) noexcept
{
    const void* const outer = t_measureFrame;
    t_measureFrame          = analysis != nullptr ? ReadPointer(analysis, kAnalysisFrame) : nullptr;
    const auto result = reinterpret_cast<MeasureFn>(g_measureCall.Original())(record, analysis, third, fourth);
    t_measureFrame    = outer;
    return result;
}

/// A gate's read, on whichever thread AutoSe runs (seen live: not the game thread, and more than one): the
/// nearest player's listener to the frame's point at `offset` (player 1's, the game's answer in `out`, unless a seat's
/// is nearer). The seats' places come from the game thread's check of this tick (g_checked; NaN while
/// off) and go through AutoSe's own converter, as the getter sends the singleton's. Returns the listener answered.
int NearestForGate(void* out, const void* frame, std::uint32_t offset) noexcept
{
    std::array<float, 3 * kListeners> checked{};
    if (frame == nullptr || g_convert == 0 || !g_autoSeAllowed.load(std::memory_order_relaxed) ||
        !g_checked.Read(checked))
    {
        return 0;
    }
    const auto convert =
        std::atomic_ref<ConvertFn>(*reinterpret_cast<ConvertFn*>(g_convert)).load(std::memory_order_relaxed);
    if (convert == nullptr)
    {
        return 0;
    }
    const Vec3                    point = Read3(frame, offset);
    std::array<Vec3, kListeners>  places{};
    std::array<float, kListeners> distances{};
    distances.fill(kInfinite);
    places[0]    = Read3(out, 0);
    distances[0] = listener_rules::Distance(point, places[0]);
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i     = static_cast<std::size_t>(seat);
        const Vec3 place = At(checked, 3 * i);
        if (!Valid(place))
        {
            continue;
        }
        float              converted[3]{};
        const float* const answer = convert(converted, place.data());
        if (answer != nullptr)
        {
            places[i]    = {answer[0], answer[1], answer[2]};
            distances[i] = listener_rules::Distance(point, places[i]);
        }
    }
    const int k = listener_rules::Nearest(distances);
    if (k > 0)
    {
        std::memcpy(out, places[static_cast<std::size_t>(k)].data(), sizeof(float) * 3);  // the fourth stays 1.0
    }
    return k;
}

/// The AutoSe listener position (off SEAD's thread): the game's read, never during a swap.
std::uint8_t ReadPosition(void* out) noexcept
{
    const auto read = reinterpret_cast<PositionFn>(g_positionEntry.Original());
    if (OnSeadFrame())
    {
        return read(out);  // never inside a swap: swaps run only on this thread, around the game's object calls
    }
    bool waited = false;
    for (int attempt = 0; attempt < kPositionTries; ++attempt)
    {
        const auto before = g_swap.load(std::memory_order_acquire);
        if ((before & 1u) != 0)
        {
            waited = true;
            _mm_pause();
            continue;
        }
        alignas(16) std::array<std::byte, 16> position{};
        const std::uint8_t result = read(position.data());
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_swap.load(std::memory_order_relaxed) != before)
        {
            waited = true;
            continue;
        }
        if (waited)
        {
            g_counts.readWaits.fetch_add(1, std::memory_order_relaxed);
        }
        if (result != 0)  // the game writes the position only when it answers
        {
            std::memcpy(out, position.data(), position.size());
        }
        t_answer = {true, result, position};
        return result;
    }
    g_counts.readHeld.fetch_add(1, std::memory_order_relaxed);
    if (!t_answer.held)
    {
        return read(out);  // no clean answer yet on this thread: the game's read
    }
    if (t_answer.result != 0)
    {
        std::memcpy(out, t_answer.position.data(), t_answer.position.size());
    }
    return t_answer.result;
}

/// The AutoSe listener position's entry: the shielded read; for the clip (gate A) and the measure (gate C), the
/// nearest player's.
std::uint8_t Position(void* out) noexcept
{
    const auto back = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    Note(kPositionHook);
    g_counts.reads.fetch_add(1, std::memory_order_relaxed);
    const std::uint8_t result = ReadPosition(out);
    if (back == g_clipReturn)
    {
        g_counts.clipReads.fetch_add(1, std::memory_order_relaxed);
        const int k = result != 0 ? NearestForGate(out, t_clipFrame, kFramePosition) : 0;
        g_counts.clipFrom[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed);
    }
    else if (back == g_measureReturn)
    {
        g_counts.measures.fetch_add(1, std::memory_order_relaxed);
        const int k = result != 0 ? NearestForGate(out, t_measureFrame, kFrameMeasured) : 0;
        g_counts.measureFrom[static_cast<std::size_t>(k)].fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

/// The footstep detection's LOD level: the game's level from player 1's listener, then from each seat's
/// whose listener is on; the lowest.
std::uint64_t Lod(void* callback, void* character) noexcept
{
    const auto level  = reinterpret_cast<LodFn>(g_lodSlot.Original());
    const auto result = level(callback, character);
    Note(kLodHook);
    g_counts.lods.fetch_add(1, std::memory_order_relaxed);
    const auto first = static_cast<std::int32_t>(result);
    if (listener_rules::LodKeepsFirst(first))
    {
        return result;  // no filter, or the finest level already
    }
    if (!players::generation::OnGameThread())
    {
        g_counts.lodElsewhere.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto vt = g_pcVTable.load(std::memory_order_relaxed);
    if (vt == 0)
    {
        return result;
    }
    std::uint64_t kept      = result;
    std::int32_t  keptLevel = first;
    for (int seat = 1; seat <= kSeats && !listener_rules::LodKeepsFirst(keptLevel); ++seat)
    {
        const auto  i          = static_cast<std::size_t>(seat);
        void* const controller = g_controllers[i];
        if (!g_live[i] || controller == nullptr || VTableOf(controller) != vt)
        {
            continue;
        }
        void* const saved = t_lodController;
        t_lodController   = controller;
        const auto other  = level(callback, character);
        t_lodController   = saved;
        g_counts.lodAsked[i].fetch_add(1, std::memory_order_relaxed);
        if (listener_rules::LodLowered(keptLevel, static_cast<std::int32_t>(other)))
        {
            kept      = other;
            keptLevel = static_cast<std::int32_t>(other);
        }
    }
    if (keptLevel != first)
    {
        g_counts.lodLowered.fetch_add(1, std::memory_order_relaxed);
        g_lodKept.store(first, std::memory_order_relaxed);
        g_lodLower.store(keptLevel, std::memory_order_relaxed);
    }
    return kept;
}

/// The player controller's GetAudioListenerPosition: in a seat's LOD pass, that seat's controller answers.
void PcListener(void* controller, void* location, void* front, void* right) noexcept
{
    void* const other = t_lodController;
    reinterpret_cast<PcListenerFn>(g_pcSlot.Original())(other != nullptr ? other : controller, location, front, right);
}

bool Place(const platform::GameImage& image, platform::VTableEntry& entry, std::uintptr_t slot, std::uintptr_t fn,
           void* replacement) noexcept
{
    const auto at = image.Rebase(slot, sizeof(void*));
    const auto to = image.Rebase(fn);
    return at && to && entry.Replace(*at, *to, reinterpret_cast<std::uintptr_t>(replacement));
}

/// Game thread: the controller's listener slot, once player 1's controller exists (its vtable is known only then).
void PlaceControllerSlot() noexcept
{
    if (g_pcSlot.Original() != 0 || g_pcListenerFn == 0 || g_lodSlot.Original() == 0)
    {
        return;
    }
    const auto vt = VTableOf(players::generation::PlayerOneController());
    if (vt == 0 || vt == g_pcTried)
    {
        return;
    }
    g_pcTried     = vt;
    const bool ok = g_pcSlot.Replace(vt + kPcListenerSlot, g_pcListenerFn, reinterpret_cast<std::uintptr_t>(&PcListener));
    g_pcVTable.store(ok ? vt : 0, std::memory_order_relaxed);
    platform::Log(L"sound listeners: footstep detection's LOD {}",
                  ok ? L"measured from every player's listener (the controller's listener slot)"
                     : L"stays player 1's (player 1's controller's listener slot differs)");
}

/// The controller's own listener position (the call the viewport's draw makes per view), or false.
bool ControllerListener(void* controller, Vec3& out) noexcept
{
    const auto   vt = VTableOf(controller);
    PcListenerFn fn = nullptr;
    if (vt == 0 || !platform::CopyGuarded(&fn, reinterpret_cast<const void*>(vt + kPcListenerSlot), sizeof(fn)) ||
        fn == nullptr)
    {
        return false;
    }
    float front[3] = {};
    float right[3] = {};
    return platform::CallGuarded(fn, controller, static_cast<void*>(out.data()), static_cast<void*>(front),
                                 static_cast<void*>(right));
}

/// The game-thread listeners' count and entries (the viewport's SetListener copy), or -1.
int GameListeners(std::array<Vec3, kColumns>& entries) noexcept
{
    const void*  device = g_device.load(std::memory_order_relaxed);
    const void*  data   = nullptr;
    std::int32_t count  = -1;
    if (device == nullptr ||
        !platform::CopyGuarded(&data, static_cast<const std::byte*>(device) + kGameListeners, sizeof(data)) ||
        data == nullptr ||
        !platform::CopyGuarded(&count, static_cast<const std::byte*>(device) + kGameListeners + 8, sizeof(count)))
    {
        return -1;
    }
    for (std::int32_t i = 0; i < kColumns && i < count; ++i)
    {
        bool ok = false;
        entries[static_cast<std::size_t>(i)] =
            Read3Guarded(static_cast<const std::byte*>(data) + static_cast<std::size_t>(kGameListenerStride) * i,
                         kTranslation, ok);
        if (!ok)
        {
            entries[static_cast<std::size_t>(i)].fill(std::nanf(""));  // never valid, never widens a window's largest
        }
    }
    return count;
}

float Widen(float gap, float value) noexcept
{
    return std::max(gap, value);
}

std::wstring Metres(float cm)
{
    return cm < 0.0f || !std::isfinite(cm) ? std::wstring(L"-") : std::format(L"{:.1f} m", cm * kMetres);
}

std::wstring ListenerName(int k)
{
    return players::SeatName(k);  // 0: player 1
}

void LogThreads() noexcept
{
    for (int hook = 0; hook < kHooks; ++hook)
    {
        const auto  i  = static_cast<std::size_t>(hook);
        const DWORD id = g_threads[i].load(std::memory_order_relaxed);
        if (id != 0 && !g_threadLogged[i])
        {
            g_threadLogged[i] = true;
            platform::LogDebug(L"sound listeners: {} runs on {} (thread {})", kHookNames[hook],
                               id == GetCurrentThreadId() ? L"the game thread" : L"another thread", id);
        }
        if (g_otherThread[i].load(std::memory_order_relaxed) && !g_otherLogged[i])
        {
            g_otherLogged[i] = true;
            platform::LogDebug(L"sound listeners: {} also ran on a second thread; there it counts only and every sound "
                               L"is player 1's",
                               kHookNames[hook]);
        }
    }
}

// Why a seat's listener is off (game thread), for the line on change.
enum SeatState : int
{
    kSeatOn,
    kSeatOut,          // no player in the seat
    kSeatNoView,       // their own view is not on screen (player 1's picture in their column, the single view)
    kSeatNoEntry,      // the engine has no listener at the column yet
    kSeatNotFollowing  // the engine's listener at the column stopped following the seat's camera
};

/// Game thread: each seat's listener from the engine's listener at its column, on while their own view is on screen
/// and that entry follows the seat's own camera; logged on change.
void TickSeats(const std::array<Vec3, kColumns>& entries, int count,
               const std::array<int, players::kMaxPlayers>& ownColumns) noexcept
{
    g_at[0]   = entries[0];
    g_live[0] = count >= 1 && Valid(entries[0]);
    std::array<float, 3 * kListeners> checked{};
    checked.fill(std::nanf(""));
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto  i          = static_cast<std::size_t>(seat);
        void* const controller = players::generation::Active(seat) ? players::generation::ControllerOf(seat) : nullptr;
        g_controllers[i]       = controller;
        const int column       = controller != nullptr ? ownColumns[i] : -1;
        SeatState state        = kSeatOn;
        if (controller == nullptr)
        {
            state = kSeatOut;
        }
        else if (column < 1 || column >= kColumns)
        {
            state = kSeatNoView;
        }
        else if (column >= count)
        {
            state = kSeatNoEntry;
        }
        if (column != g_columnSeen[i] || state != kSeatOn)
        {
            g_stale[i] = kStaleTicks + 1;  // a new or regained column holds another view's place until the engine sets it
        }
        g_columnSeen[i] = state == kSeatOn ? column : -1;
        Vec3 own{};
        if (state == kSeatOn && ControllerListener(controller, own) && Valid(entries[static_cast<std::size_t>(column)]))
        {
            const float gap    = listener_rules::Distance(own, entries[static_cast<std::size_t>(column)]);
            g_controllerGap[i] = Widen(g_controllerGap[i], gap);
            g_stale[i]         = gap > kFreshCm ? g_stale[i] + 1 : 0;
        }
        else if (state == kSeatOn)
        {
            ++g_stale[i];  // its camera or the entry could not be read
        }
        if (state == kSeatOn && g_stale[i] > kStaleTicks)
        {
            state = kSeatNotFollowing;
        }
        g_live[i] = state == kSeatOn;
        g_at[i]   = state == kSeatOn ? entries[static_cast<std::size_t>(column)] : Vec3{};
        g_seatColumn[i].store(state == kSeatOn ? column : -1, std::memory_order_relaxed);
        if (state == kSeatOn)
        {
            std::copy(g_at[i].begin(), g_at[i].end(), checked.begin() + 3 * static_cast<std::ptrdiff_t>(seat));
        }
        if (state != g_stateLogged[i])
        {
            g_stateLogged[i] = state;
            constexpr const wchar_t* kWhy[] = {L"", L"no player", L"their own view is not on screen",
                                               L"the engine has no listener at their column yet",
                                               L"the engine's listener at their column does not follow their camera"};
            if (state == kSeatOn)
            {
                platform::Log(L"sound listeners: {}'s listener follows their view (column {}); sounds near them are "
                              L"measured from it",
                              players::SeatName(seat), column + 1);
            }
            else
            {
                platform::Log(L"sound listeners: {}'s listener off ({}); their sounds go to the nearest listener on",
                              players::SeatName(seat), kWhy[state]);
            }
        }
        const int on = g_onSeen[i].load(std::memory_order_relaxed) ? 1 : 0;
        if (on != g_onLogged[i])
        {
            g_onLogged[i] = on;
            // Debug: SEAD drops a listener for a frame or two at a camera cut (the two threads' places differ).
            platform::LogDebug(L"sound listeners: SEAD {} {}'s listener", on != 0 ? L"now sets" : L"no longer sets",
                               players::SeatName(seat));
        }
    }
    g_checked.Publish(checked);
}

std::wstring PerSeat(std::array<std::atomic<unsigned>, kListeners>& counts, const wchar_t* what)
{
    std::wstring text;
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        if (const unsigned n = platform::Take(counts[static_cast<std::size_t>(seat)]); n != 0)
        {
            text += std::format(L", {} {} {}'s", n, what, players::SeatName(seat));
        }
    }
    return text;
}

std::wstring Summary()
{
    std::array<float, kListenerFields> seen{};
    const bool   read = g_listeners.Read(seen);
    std::wstring text = std::format(L"{} pushes; the engine has {} listener(s)", platform::Take(g_counts.pushes),
                                    read ? std::to_wstring(static_cast<int>(seen[kCount])) : std::wstring(L"no"));
    if (read && Valid(At(seen, kSeadX)) && Valid(At(seen, kColumnX)))
    {
        text += std::format(L"; SEAD's own listener {} from the engine's first",
                            Metres(listener_rules::Distance(At(seen, kSeadX), At(seen, kColumnX))));
    }
    if (const unsigned other = platform::Take(g_counts.otherDevicePushes); other != 0)
    {
        text += std::format(L" ({} pushes of another audio device not read)", other);
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        const auto i = static_cast<std::size_t>(seat);
        if (g_controllers[i] == nullptr)
        {
            continue;
        }
        const float apart = g_live[i] && g_live[0] ? listener_rules::Distance(g_at[i], g_at[0]) : -1.0f;
        text += std::format(L"; {}: column {}, {} from player 1, listener {} (their view's listener up to {} from their "
                            L"controller's)",
                            players::SeatName(seat), g_columnSeen[i] >= 0 ? std::to_wstring(g_columnSeen[i] + 1) : L"-",
                            Metres(apart), g_onSeen[i].load(std::memory_order_relaxed) ? L"on" : L"off",
                            Metres(g_controllerGap[i]));
    }
    for (int column = 1; column < kColumns; ++column)
    {
        if (g_otherColumnGap[static_cast<std::size_t>(column)] >= 0.0f)
        {
            text += std::format(L"; column {} (no player's own view) up to {} from player 1's listener", column + 1,
                                Metres(g_otherColumnGap[static_cast<std::size_t>(column)]));
        }
    }
    if (const unsigned failures = platform::Take(g_counts.setFailures); failures != 0)
    {
        text += std::format(L"; the game's setter refused player 2's or 3's listener {} time(s)", failures);
    }
    if (const unsigned full = platform::Take(g_counts.tableFull); full != 0)
    {
        text += std::format(L"; {} sound(s) kept on player 1's listener for want of a choice entry", full);
    }
    text += L" | sounds:";
    for (int kind = 0; kind < kKinds; ++kind)
    {
        KindCounts&    counts  = g_kinds[static_cast<std::size_t>(kind)];
        const unsigned checked = platform::Take(counts.checked);
        const unsigned rel     = platform::Take(counts.relative);
        const unsigned fromOne = platform::Take(counts.measured[0]);
        const unsigned heard   = platform::Take(counts.heard[0]);
        std::wstring   detail;
        for (int seat = 1; seat <= kSeats; ++seat)
        {
            const auto     i        = static_cast<std::size_t>(seat);
            const unsigned measured = platform::Take(counts.measured[i]);
            const unsigned heardBy  = platform::Take(counts.heard[i]);
            if (measured != 0)
            {
                detail += std::format(L", {} measured from {}'s listener ({} heard)", measured, players::SeatName(seat),
                                      heardBy);
            }
        }
        std::wstring missed;
        for (int k = 0; k < kListeners; ++k)
        {
            if (const unsigned cut = platform::Take(counts.missed[static_cast<std::size_t>(k)]); cut != 0)
            {
                missed += std::format(L", {} cut within {}'s reach", cut, ListenerName(k));
            }
        }
        if (checked != 0)
        {
            text += std::format(L" {} {} checked, {} heard from player 1's listener (of {}){}{}{}{};",
                                kKindFacts[kind].name, checked, heard, fromOne, detail,
                                rel != 0 ? std::format(L", {} on player 1's listener (listener-relative)", rel)
                                         : std::wstring(),
                                missed, kind >= kChosenKinds && !missed.empty() ? L" (an area: player 1's, approximate)" : L"");
        }
    }
    if (const unsigned untested = platform::Take(g_counts.untested); untested != 0)
    {
        text += std::format(L" {} updates of a sound not tested this frame or refused a choice entry (player 1's "
                            L"listener);",
                            untested);
    }
    for (int seat = 1; seat <= kSeats; ++seat)
    {
        if (const unsigned updates = platform::Take(g_counts.updates[static_cast<std::size_t>(seat)]); updates != 0)
        {
            text += std::format(L" {} updates from {}'s listener;", updates, players::SeatName(seat));
        }
    }
    platform::Take(g_counts.clipFrom[0]);
    platform::Take(g_counts.measureFrom[0]);
    platform::Take(g_counts.lodAsked[0]);
    const unsigned lowered = platform::Take(g_counts.lodLowered);
    text += std::format(L" | AutoSe listener reads {} ({} waited out a swap, {} took the latest clean answer); the "
                        L"footstep clip {}{}; the rustle and swing measure {}{}; footstep LOD levels {}{}, lowered {}{} "
                        L"(off the game thread {})",
                        platform::Take(g_counts.reads), platform::Take(g_counts.readWaits),
                        platform::Take(g_counts.readHeld), platform::Take(g_counts.clipReads),
                        PerSeat(g_counts.clipFrom, L"from"), platform::Take(g_counts.measures),
                        PerSeat(g_counts.measureFrom, L"from"), platform::Take(g_counts.lods),
                        PerSeat(g_counts.lodAsked, L"asked again for"), lowered,
                        lowered != 0 ? std::format(L", latest {} -> {}", g_lodKept.load(std::memory_order_relaxed),
                                                   g_lodLower.load(std::memory_order_relaxed))
                                     : std::wstring{},
                        platform::Take(g_counts.lodElsewhere));
    const unsigned                    examples = g_counts.examples.load(std::memory_order_relaxed);
    std::array<float, kExampleFields> example{};
    if (examples != g_exampleSeen && g_example.Read(example))
    {
        g_exampleSeen    = examples;
        const int kind   = std::clamp(static_cast<int>(example[kExampleKind]), 0, kKinds - 1);
        const int owner  = std::clamp(static_cast<int>(example[kExampleOwner]), 0, kListeners - 1);
        const int chosen = std::clamp(static_cast<int>(example[kExampleChosen]), 0, kListeners - 1);
        text += std::format(L" | last sound cut within another player's reach: a {} sound at ({:.0f}, {:.0f}, {:.0f}), "
                            L"range {}, measured from {}'s listener at {}, {} from {}'s",
                            kKindFacts[kind].name, example[kExampleX], example[kExampleX + 1], example[kExampleX + 2],
                            Metres(example[kExampleRange]), ListenerName(chosen), Metres(example[kExampleToChosen]),
                            Metres(example[kExampleToOwner]), ListenerName(owner));
    }
    return text;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    for (auto& column : g_seatColumn)
    {
        column.store(-1, std::memory_order_relaxed);
    }
    g_stateLogged.fill(-1);
    g_onLogged.fill(0);
    g_columnSeen.fill(-1);
    g_lastChecked.fill(std::nanf(""));  // no place checked yet: no seat is set before the game thread's first check
    g_controllerGap.fill(-1.0f);
    g_otherColumnGap.fill(-1.0f);
    const auto global = image.Rebase(kListenerGlobal, sizeof(void*));
    g_listenerGlobal.store(global ? reinterpret_cast<void**>(*global) : nullptr, std::memory_order_relaxed);
    const bool known = global.has_value();

    // The push (the engine's listeners for every column).
    const bool push = known && Place(image, g_pushSlot, kPushSlot, kPush, reinterpret_cast<void*>(&Push));

    // What the AutoSe reader's entry reads (on AutoSe's threads, once the shield is live): written before it.
    const auto convert     = image.Rebase(kConvert, sizeof(void*));
    const auto clipReturn  = image.Rebase(kClipReturn);
    const auto measureBack = image.Rebase(kMeasureReturn);
    g_convert              = convert ? *convert : 0;
    g_clipReturn           = clipReturn ? *clipReturn : 0;
    g_measureReturn        = measureBack ? *measureBack : 0;

    // The AutoSe reader first: once a swap can happen, it must already be shielded.
    bool       shield = false;
    const auto read   = known ? platform::FindFunction(image, kGetPosition) : std::nullopt;
    if (read)
    {
        const auto first = std::span(kGetPositionBytes).first(kGetPositionFirst);
        shield = g_positionEntry.Install(*read, image.EntryInstruction(kGetPosition.address, first),
                                         reinterpret_cast<std::uintptr_t>(&Position));
    }

    // The sound classes: each test (counted always), and for the kinds that choose, the update with it, once the
    // listeners can be set.
    const auto set       = shield ? platform::FindFunction(image, kListenerSet) : std::nullopt;
    g_set                = set ? reinterpret_cast<SetFn>(*set) : nullptr;
    const auto site      = image.Rebase(kVelocityCall, 5);
    const auto velocity  = image.Rebase(kVelocity);
    const bool settable  = push && g_set != nullptr && site && velocity;
    std::array<bool, kChosenKinds> paired{};
    for (int kind = 0; kind < kKinds; ++kind)
    {
        const auto&  row   = kKindFacts[kind];
        const auto   i     = static_cast<std::size_t>(kind);
        const bool   test  = Place(image, g_testSlots[i], row.testSlot, row.test,
                                   reinterpret_cast<void*>(kAudibleHooks[kind]));
        if (kind < kChosenKinds && test && settable)
        {
            paired[i] = Place(image, g_updateSlots[i], row.updateSlot, row.update,
                              reinterpret_cast<void*>(kUpdateHooks[kind]));
            g_paired[i].store(paired[i], std::memory_order_relaxed);
        }
    }

    // The AutoSe gates A and C: the processor update's frame and the measure's, for the nearest player's listener.
    const auto clipSite     = image.Rebase(kClipUpdateCall, 5);
    const auto clipUpdate   = image.Rebase(kClipUpdate);
    const auto measureSite  = image.Rebase(kMeasureCall, 5);
    const auto measure      = platform::FindFunction(image, kMeasure);
    const bool gates        = shield && g_convert != 0;
    const bool clip         = gates && g_clipReturn != 0 && clipSite && clipUpdate &&
                      g_clipCall.Install(*clipSite, *clipUpdate, reinterpret_cast<std::uintptr_t>(&ClipUpdate));
    const bool measured     = gates && g_measureReturn != 0 && measureSite && measure &&
                          g_measureCall.Install(*measureSite, *measure, reinterpret_cast<std::uintptr_t>(&Measure));
    g_autoSeAllowed.store(gates, std::memory_order_relaxed);

    // Gate B: the footstep LOD; its controller slot follows from Tick.
    const auto pc  = image.Rebase(kPcListener);
    g_pcListenerFn = pc ? *pc : 0;
    const bool lod = g_pcListenerFn != 0 && Place(image, g_lodSlot, kLodSlot, kLod, reinterpret_cast<void*>(&Lod));

    // Last, the seats' listeners themselves: the SEAD frame that sets them turns the choice on.
    g_seatsAllowed.store(settable, std::memory_order_relaxed);
    const bool frame = settable &&
                       g_velocityCall.Install(*site, *velocity, reinterpret_cast<std::uintptr_t>(&Velocity));
    g_installed.store(true, std::memory_order_relaxed);

    const auto onOff = [](bool on) { return on ? L"on" : L"OFF (a check failed)"; };
    platform::Log(L"sound listeners: each sound from the nearest player's listener {} (moving {}, placed {}, wedge {}; "
                  L"the AutoSe reader shielded {}); footstep detection: the distance clip from the nearest {}, the "
                  L"rustle and swing range from the nearest {}, the LOD {}; area sounds stay on player 1's listener "
                  L"(counted); a summary every 10 s at log level debug",
                  onOff(frame), onOff(frame && paired[kMoving]), onOff(frame && paired[kPlaced]),
                  onOff(frame && paired[kWedge]), onOff(shield), onOff(clip), onOff(measured), onOff(lod));
}

void Remove() noexcept
{
    if (!g_installed.exchange(false))
    {
        return;
    }
    // The seats first: from here every sound and every AutoSe read is player 1's again, then the hooks go.
    g_seatsAllowed.store(false, std::memory_order_relaxed);
    g_autoSeAllowed.store(false, std::memory_order_relaxed);
    bool kept = false;
    for (platform::CallRedirect* call : {&g_velocityCall, &g_clipCall, &g_measureCall})
    {
        kept = (call->IsInstalled() && !call->Restore()) || kept;
    }
    for (platform::VTableEntry* entry : {&g_pcSlot, &g_lodSlot, &g_pushSlot})
    {
        kept = (entry->Original() != 0 && !entry->Restore()) || kept;
    }
    for (auto& entry : g_updateSlots)
    {
        kept = (entry.Original() != 0 && !entry.Restore()) || kept;
    }
    for (auto& entry : g_testSlots)
    {
        kept = (entry.Original() != 0 && !entry.Restore()) || kept;
    }
    if (kept)
    {
        g_leftBehind.store(true, std::memory_order_relaxed);  // before the wait: an update still ours stops swapping now
    }
    // The AutoSe reader's shield last, once no swap can still be running: a SEAD call that entered a hook just before
    // its slot was restored may still be inside one. Wait for the counter to be even, then past one SEAD frame
    // (its loop sleeps 32 ms).
    for (int waited = 0; waited < 100 && (g_swap.load(std::memory_order_acquire) & 1u) != 0; ++waited)
    {
        Sleep(1);
    }
    Sleep(40);
    kept = (g_positionEntry.IsInstalled() && !g_positionEntry.Restore()) || kept;
    if (kept)
    {
        g_leftBehind.store(true, std::memory_order_relaxed);  // a sound's last choice may never be renewed now
        platform::Log(L"sound listeners: a redirect was changed after ours; left in place (it passes through)");
    }
}

void Tick(const std::array<int, players::kMaxPlayers>& ownColumns) noexcept
{
    if (!g_installed.load(std::memory_order_relaxed) || !players::generation::OnGameThread())
    {
        return;
    }
    try
    {
        PlaceControllerSlot();
        LogThreads();

        // The game thread's listeners (the viewport's SetListener copy): the seats' listeners, and the gaps.
        std::array<Vec3, kColumns> entries{};
        for (auto& entry : entries)
        {
            entry.fill(std::nanf(""));
        }
        const int count = GameListeners(entries);
        if (count != g_gameCountLogged && count >= 0)
        {
            g_gameCountLogged = count;
            platform::Log(L"sound listeners: the engine keeps {} listener(s) on the game thread", count);
        }
        TickSeats(entries, count, ownColumns);
        for (int column = 1; column < std::min(count, kColumns); ++column)
        {
            bool own = false;
            for (int seat = 1; seat <= kSeats; ++seat)
            {
                own = own || g_columnSeen[static_cast<std::size_t>(seat)] == column;
            }
            if (!own && Valid(entries[0]) && Valid(entries[static_cast<std::size_t>(column)]))
            {
                g_otherColumnGap[static_cast<std::size_t>(column)] =
                    Widen(g_otherColumnGap[static_cast<std::size_t>(column)],
                          listener_rules::Distance(entries[static_cast<std::size_t>(column)], entries[0]));
            }
        }
        if (platform::Enabled(platform::Level::Trace))
        {
            std::array<float, kListenerFields> seen{};
            if (g_listeners.Read(seen))
            {
                const std::size_t base1 = kColumnX + kPerColumn, base2 = kColumnX + 2 * kPerColumn;
                platform::LogTrace(L"sound listeners: SEAD ({:.0f}, {:.0f}, {:.0f}); engine {} listener(s): [0] ({:.0f}, "
                                   L"{:.0f}, {:.0f}) [1] ({:.0f}, {:.0f}, {:.0f}) [2] ({:.0f}, {:.0f}, {:.0f})",
                                   seen[kSeadX], seen[kSeadX + 1], seen[kSeadX + 2], static_cast<int>(seen[kCount]),
                                   seen[kColumnX], seen[kColumnX + 1], seen[kColumnX + 2], seen[base1], seen[base1 + 1],
                                   seen[base1 + 2], seen[base2], seen[base2 + 1], seen[base2 + 2]);
            }
        }

        const bool first = g_summary.Never();  // the first ask: the counts so far are thrown away
        if (!g_summary.Due(std::chrono::steady_clock::now()))
        {
            return;
        }
        const std::wstring summary = Summary();
        g_controllerGap.fill(-1.0f);
        g_otherColumnGap.fill(-1.0f);
        if (!first)
        {
            platform::LogDebug(L"sound listeners (10 s): {}", summary);
        }
    }
    catch (...)
    {
    }
}

}  // namespace lss::feedback::sound_listeners
