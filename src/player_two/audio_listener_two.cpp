#include "player_two/audio_listener_two.h"

#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/seqlock.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "player_two/charapop_facts.h"
#include "player_two/generation.h"
#include "player_two/listener_choice.h"
#include "player_two/object_fields.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>

#pragma intrinsic(_ReturnAddress)

namespace lss::player_two::audio_listener_two {

namespace {

// 1.004 addresses (1.005 through replaced-audio-listener.json; each read in both exes, scratchpad audio_facts.py and
// audio_fix_facts.py).
constexpr std::uintptr_t kPushSlot          = 0x1465E6058;  // SEAD audio device vt+0x1B8
constexpr std::uintptr_t kPush              = 0x14082ECE4;  // SeadAudioDevice_PushListener0ToSead(device)
constexpr std::uintptr_t kDynamicSlot       = 0x1457DA960;  // SQEX::Sd::Lay::DynamicSound vt+0x160
constexpr std::uintptr_t kDynamic           = 0x140B94018;  // SEAD_LayDynamic_UpdateAudible(object)
constexpr std::uintptr_t kPointSlot         = 0x1457DA5D0;  // SQEX::Sd::Lay::PointSound vt+0x160
constexpr std::uintptr_t kPoint             = 0x14127585C;  // SEAD_LayPoint_UpdateAudible(object)
constexpr std::uintptr_t kDynamicUpdateSlot = 0x1457DA858;  // Lay::DynamicSound vt+0x58
constexpr std::uintptr_t kDynamicUpdate     = 0x1414624C0;  // SEAD_LayDynamic_Update_Doppler(object): doppler, then the base
constexpr std::uintptr_t kPointUpdateSlot   = 0x1457DA4C8;  // Lay::PointSound vt+0x58
constexpr std::uintptr_t kBaseUpdate        = 0x1407AC83C;  // SEAD_LayObj_Update_Base(object): Calc3D, the voice's start/stop
constexpr std::uintptr_t kVelocityCall      = 0x14082D51B;  // SEAD_Lay_UpdateFrame: E8 SEAD_LayListener_UpdateVelocity
constexpr std::uintptr_t kVelocity          = 0x14161261C;  // SEAD_LayListener_UpdateVelocity(): the singleton's velocity
constexpr std::uintptr_t kListenerGlobal    = 0x14939BAB0;  // g_SeadLayListener: the one SQEX::Sd::Lay::Listener
constexpr std::uintptr_t kClipReturn        = 0x140807DE8;  // the AutoSe processor update's distance clip, after its read
constexpr std::uintptr_t kClipUpdateCall    = 0x1408072F8;  // SeadAutoSeProc_TickById: E8 the processor update
constexpr std::uintptr_t kClipUpdate        = 0x140807D24;  // SeadAutoSeProc_Update_ClipAndLodFilter(proc, dt, frame, ...)
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

using PushFn       = std::uint64_t (*)(void* device);
using ObjectFn     = std::uint64_t (*)(void* object);  // UpdateAudible and Update (vt+0x160, vt+0x58)
using VelocityFn   = std::uint64_t (*)(void*, void*, void*);  // takes no arguments; the registers pass through
using SetFn        = std::int64_t (*)(void* listener, float px, float py, float pz, float fx, float fy, float fz, float ux,
                                     float uy, float uz);
using PositionFn   = std::uint8_t (*)(void* out);
using ClipUpdateFn = std::uint64_t (*)(void* processor, float dt, void* frame, std::int32_t count, std::int32_t fifth,
                                       void* sixth);
using ConvertFn    = const float* (*)(float* out, const float* in);  // 12-byte vectors
using LodFn        = std::uint64_t (*)(void* callback, void* character);
using PcListenerFn = void (*)(void* controller, void* location, void* front, void* right);

// Fields (RE-p2-audio-listener.md §2-§3; the same in both builds).
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
constexpr std::uint32_t kObjectRange         = 0x1F4;  // the audible range (DynamicSound: x the magnification at load)
constexpr std::uint32_t kObjectPosition      = 0x1E0;  // the position the test measured (written by UpdateAudible)
constexpr std::uint32_t kDynamicSource       = 0x220;  // DynamicSound: the world source position
constexpr std::uint32_t kPointSource         = 0x210;  // PointSound: the placed position (copied to +0x1E0 by the test)
constexpr std::uint32_t kDynamicRelative     = 0x259;  // u8 1: a listener-relative source
constexpr std::uint32_t kDynamicIgnoreMag    = 0x25B;  // u8: ignores the magnification
constexpr std::uint32_t kFramePosition       = 0x35C;  // the AutoSe frame's character position, which the clip measures
constexpr auto          kSummaryEvery        = std::chrono::seconds(10);
constexpr float         kMetres              = 0.01f;
constexpr float         kHandOver            = 0.8f;   // a sound changes listener once the other is 20 % nearer
constexpr int           kPositionTries       = 256;    // spins while a swap runs, before the latest clean answer
constexpr std::size_t   kChoiceBits          = 12;     // 4096 remembered choices; ~250 live SEAD objects

/// N floats one thread writes and another reads (platform/seqlock.h).
template <std::size_t N>
using Floats = platform::SeqFloats<N>;

// The audio thread's view: the UE listener count, [0], [1] with its front and up as the device would push them, and
// SEAD's position after the push.
enum ListenerField : std::size_t
{
    kCount,
    kZeroX,
    kOneX     = kZeroX + 3,
    kFrontX   = kOneX + 3,
    kUpX      = kFrontX + 3,
    kSeadX    = kUpX + 3,
    kListenerFields = kSeadX + 3
};
Floats<kListenerFields> g_listeners;
// The latest sound player 2's listener would hear but its chosen listener culled: point (0) or moving (1), distances
// to both, range, factor, place.
enum ExampleField : std::size_t { kKind, kToOne, kToTwo, kRange, kMagnify, kWhereX, kExampleFields = kWhereX + 3 };
Floats<kExampleFields> g_example;

platform::VTableEntry   g_pushSlot, g_dynamicSlot, g_pointSlot, g_dynamicUpdateSlot, g_pointUpdateSlot;
platform::VTableEntry   g_lodSlot, g_pcSlot;
platform::CallRedirect  g_velocityCall, g_clipCall;
platform::EntryRedirect g_positionEntry;
std::uintptr_t          g_convert       = 0;  // live address of AutoSe's converter pointer
std::atomic<void**>     g_listenerGlobal{nullptr};  // the address of SEAD's listener pointer; written by Install
SetFn                   g_set           = nullptr;
std::uintptr_t          g_clipReturn    = 0;
std::uintptr_t          g_pcListenerFn  = 0;  // live; the slot's expected function
std::atomic<std::uintptr_t> g_pcVTable{0};    // the controller vtable whose listener slot is ours
std::atomic<void*>      g_device{nullptr};
std::atomic<DWORD>      g_audioThread{0}, g_seadThread{0};

// Player 2's SEAD listener: a copy of the game's listener object, set on SEAD's thread only.
alignas(16) std::array<std::byte, kListenerSize> g_two{};
bool                 g_twoCopied = false;     // SEAD thread
std::atomic<bool>    g_twoActive{false};      // written on SEAD's thread: listener 2 was set this SEAD frame
std::atomic<unsigned> g_swap{0};              // odd while SEAD's listener pointer names listener 2 (a seqlock)

// Each SEAD object's listener (SEAD thread only): chosen in its audibility test, used again in its update.
struct Choice
{
    const void* object = nullptr;
    bool        two    = false;
};
std::array<Choice, std::size_t{1} << kChoiceBits> g_choices{};

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

// Player 2's LOD pass: the controller the PC listener slot answers for (null outside it).
thread_local void* t_lodController = nullptr;

struct Counts
{
    std::atomic<unsigned> pushes{0};
    std::atomic<unsigned> checked{0};      // UpdateAudible calls (moving and point sounds)
    std::atomic<unsigned> relative{0};     // listener-relative moving sounds (always player 1's)
    std::atomic<unsigned> fromTwo{0};      // measured from player 2's listener
    std::atomic<unsigned> audibleTwo{0};   //   ... and audible
    std::atomic<unsigned> audibleOne{0};   // measured from player 1's listener and audible
    std::atomic<unsigned> missed{0};       // culled by the chosen listener, within range of player 2's
    std::atomic<unsigned> noTwo{0};        // checked while player 2's listener was off
    std::atomic<unsigned> updatesTwo{0};   // updates (doppler, gain, pan, voice) run from player 2's listener
    std::atomic<unsigned> reads{0};        // AutoSe listener position reads
    std::atomic<unsigned> clipReads{0};    //   ... by the distance clip
    std::atomic<unsigned> clipTwo{0};      //   ... answered with player 2's listener (nearer the character)
    std::atomic<unsigned> readWaits{0};    //   ... that waited out a swap
    std::atomic<unsigned> readHeld{0};     //   ... answered with the latest clean answer
    std::atomic<unsigned> lods{0};         // footstep LOD levels asked
    std::atomic<unsigned> lodTwo{0};       //   ... asked again for player 2
    std::atomic<unsigned> lodLowered{0};   //   ... lowered by player 2's level
    std::atomic<unsigned> lodElsewhere{0}; //   ... off the game thread (not asked for player 2)
};
Counts g_counts;
std::atomic<int> g_lodOne{-2}, g_lodTwo{-2};  // the latest lowered level pair

// Game thread.
std::chrono::steady_clock::time_point g_summaryAt{};
int                                   g_gameCount   = -1;
unsigned                              g_exampleSeen = 0;
bool                                  g_twoLogged   = false;
std::uintptr_t                        g_pcTried     = 0;  // the controller vtable last tried

std::array<float, 3> Read3(const void* object, std::uint32_t offset) noexcept
{
    return {ReadField<float>(object, offset), ReadField<float>(object, offset + 4), ReadField<float>(object, offset + 8)};
}

float Distance(const float* a, const float* b) noexcept
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

void NoteThread(std::atomic<DWORD>& slot) noexcept
{
    if (slot.load(std::memory_order_relaxed) == 0)
    {
        DWORD none = 0;
        slot.compare_exchange_strong(none, GetCurrentThreadId(), std::memory_order_relaxed);
    }
}

const void* ListenerOne() noexcept
{
    void** const global = g_listenerGlobal.load(std::memory_order_relaxed);
    return global != nullptr ? std::atomic_ref<void*>(*global).load(std::memory_order_relaxed) : nullptr;
}

/// Audio thread: the game's push of listener [0], then what it read, listener [1] as the push would send it, and what
/// SEAD holds.
std::uint64_t Push(void* device) noexcept
{
    const auto result = reinterpret_cast<PushFn>(g_pushSlot.Original())(device);
    NoteThread(g_audioThread);
    g_device.store(device, std::memory_order_relaxed);
    g_counts.pushes.fetch_add(1, std::memory_order_relaxed);
    std::array<float, kListenerFields> out{};
    const auto*  data  = static_cast<const std::byte*>(ReadPointer(device, kAudioListeners));
    const auto   count = data != nullptr ? ReadField<std::int32_t>(device, kAudioListeners + 8) : 0;
    out[kCount]        = static_cast<float>(count);
    for (std::int32_t i = 0; i < 2 && i < count; ++i)
    {
        const auto* element = data + static_cast<std::size_t>(kAudioListenerStride) * i;
        const auto  at      = Read3(element, kTranslation);
        for (std::size_t k = 0; k < 3; ++k)
        {
            out[(i == 0 ? kZeroX : kOneX) + k] = at[k];
        }
        if (i == 1)
        {
            // The device's two lines for [0]: front = Q·X, up = -(Q·Z).
            const float q[4] = {ReadField<float>(element, kRotation), ReadField<float>(element, kRotation + 4),
                                ReadField<float>(element, kRotation + 8), ReadField<float>(element, kRotation + 12)};
            const auto  front = listener_choice::RotateVector(q, {1.0f, 0.0f, 0.0f});
            const auto  up    = listener_choice::RotateVector(q, {0.0f, 0.0f, 1.0f});
            for (std::size_t k = 0; k < 3; ++k)
            {
                out[kFrontX + k] = front[k];
                out[kUpX + k]    = -up[k];
            }
        }
    }
    if (const void* listener = ListenerOne(); listener != nullptr)
    {
        const auto at = Read3(listener, kListenerPosition);
        for (std::size_t k = 0; k < 3; ++k)
        {
            out[kSeadX + k] = at[k];
        }
    }
    g_listeners.Publish(out);
    return result;
}

bool OnSeadThread() noexcept
{
    const DWORD sead = g_seadThread.load(std::memory_order_relaxed);
    return sead != 0 && sead == GetCurrentThreadId();
}

/// SEAD's thread: runs the game's code with SEAD's listener pointer naming listener 2, then restores it. The swap
/// counter is odd meanwhile, for the AutoSe reader on other threads.
template <class Run>
std::uint64_t Swapped(Run&& run) noexcept
{
    void** const          global = g_listenerGlobal.load(std::memory_order_relaxed);
    std::atomic_ref<void*> pointer(*global);
    void* const           one = pointer.load(std::memory_order_relaxed);
    const auto            v   = g_swap.load(std::memory_order_relaxed);
    g_swap.store(v + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    pointer.store(g_two.data(), std::memory_order_relaxed);
    const std::uint64_t result = run();
    pointer.store(one, std::memory_order_relaxed);
    g_swap.store(v + 2, std::memory_order_release);
    return result;
}

/// SEAD's thread, after the game's own velocity update: listener 2 set by the game's setter from UE's listener [1],
/// then its velocity by the game's update, as ProcessCommands and UpdateFrame do for the singleton.
void SetListenerTwo(void* a, void* b, void* c) noexcept
{
    const void* one = ListenerOne();
    std::array<float, kListenerFields> seen{};
    const bool filled = one != nullptr && g_set != nullptr && g_listeners.Read(seen) && seen[kCount] >= 2.0f &&
                        (seen[kOneX] != 0.0f || seen[kOneX + 1] != 0.0f || seen[kOneX + 2] != 0.0f);
    if (!filled)
    {
        g_twoActive.store(false, std::memory_order_relaxed);
        return;
    }
    if (!g_twoCopied)
    {
        std::memcpy(g_two.data(), one, kListenerSize);  // the vtable, the identity matrix, the magnification
        g_twoCopied = true;
    }
    std::memcpy(g_two.data() + kListenerMagnify, static_cast<const std::byte*>(one) + kListenerMagnify, sizeof(float));
    if (g_set(g_two.data(), seen[kOneX], seen[kOneX + 1], seen[kOneX + 2], seen[kFrontX], seen[kFrontX + 1],
              seen[kFrontX + 2], seen[kUpX], seen[kUpX + 1], seen[kUpX + 2]) != 0)
    {
        g_twoActive.store(false, std::memory_order_relaxed);
        return;
    }
    const auto velocity = reinterpret_cast<VelocityFn>(g_velocityCall.Original());
    Swapped([&] { return velocity(a, b, c); });
    g_twoActive.store(true, std::memory_order_relaxed);
}

std::uint64_t Velocity(void* a, void* b, void* c) noexcept
{
    const auto result = reinterpret_cast<VelocityFn>(g_velocityCall.Original())(a, b, c);
    NoteThread(g_seadThread);
    if (OnSeadThread())
    {
        SetListenerTwo(a, b, c);
    }
    return result;
}

bool SwapAllowed() noexcept
{
    return g_twoActive.load(std::memory_order_relaxed) && OnSeadThread();
}

Choice& ChoiceOf(const void* object) noexcept
{
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object) >> 4);
    return g_choices[(key * 0x9E3779B97F4A7C15ull) >> (64 - kChoiceBits)];
}

/// The nearer listener for a sound at `where`; a sound that already has one keeps it until the other is clearly
/// nearer, so a sound between the players does not flip its pan every SEAD frame.
bool ChooseTwo(const void* object, const float* where, const void* one) noexcept
{
    const auto  first  = Read3(one, kListenerPosition);
    const auto  second = Read3(g_two.data(), kListenerPosition);
    const float toOne  = Distance(where, first.data());
    const float toTwo  = Distance(where, second.data());
    Choice&     choice = ChoiceOf(object);
    choice = {object, listener_choice::ChooseNearer(choice.object == object, choice.two, toOne, toTwo, kHandOver)};
    return choice.two;
}

/// After the game's answer: the counts, and whether player 2's listener would hear what the chosen one culled.
void Tally(const void* object, bool moving, bool two) noexcept
{
    g_counts.checked.fetch_add(1, std::memory_order_relaxed);
    const bool audible = ReadField<std::uint8_t>(object, kObjectAudible) != 0;
    if (two)
    {
        g_counts.fromTwo.fetch_add(1, std::memory_order_relaxed);
        if (audible)
        {
            g_counts.audibleTwo.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (audible)
    {
        g_counts.audibleOne.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const void* listener = ListenerOne();
    if (listener == nullptr || !g_twoActive.load(std::memory_order_relaxed))
    {
        g_counts.noTwo.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The game's test, read from the object's own fields: magnification x range > distance from player 2's listener.
    const float range   = ReadField<float>(object, kObjectRange);
    const float magnify = !moving || ReadField<std::uint8_t>(object, kDynamicIgnoreMag) != 0
                              ? 1.0f
                              : ReadField<float>(listener, kListenerMagnify);
    const auto  where   = Read3(object, kObjectPosition);
    const auto  first   = Read3(listener, kListenerPosition);
    const auto  second  = Read3(g_two.data(), kListenerPosition);
    const float toTwo   = Distance(where.data(), second.data());
    if (!listener_choice::MissedByTwo(range, magnify, toTwo))
    {
        return;
    }
    g_counts.missed.fetch_add(1, std::memory_order_relaxed);
    g_example.Publish({moving ? 1.0f : 0.0f, Distance(where.data(), first.data()), toTwo, range, magnify, where[0],
                       where[1], where[2]});
}

/// A sound's audibility test, from the nearer listener (the game's own test either way).
std::uint64_t Audible(platform::VTableEntry& slot, void* object, bool moving, std::uint32_t source) noexcept
{
    const auto  test = reinterpret_cast<ObjectFn>(slot.Original());
    const void* one  = ListenerOne();
    if (!SwapAllowed() || one == nullptr)
    {
        const auto result = test(object);
        Tally(object, moving, false);
        return result;
    }
    if (moving && ReadField<std::uint8_t>(object, kDynamicRelative) == 1)
    {
        g_counts.relative.fetch_add(1, std::memory_order_relaxed);
        return test(object);
    }
    const auto where = Read3(object, source);
    const bool two   = ChooseTwo(object, where.data(), one);
    const auto result = two ? Swapped([&] { return test(object); }) : test(object);
    Tally(object, moving, two);
    return result;
}

std::uint64_t Dynamic(void* object) noexcept
{
    return Audible(g_dynamicSlot, object, true, kDynamicSource);
}

std::uint64_t Point(void* object) noexcept
{
    return Audible(g_pointSlot, object, false, kPointSource);
}

/// A sound's update (doppler, gain, pan, the voice's start and stop), from the listener its audibility test chose.
std::uint64_t Update(platform::VTableEntry& slot, void* object) noexcept
{
    const auto update = reinterpret_cast<ObjectFn>(slot.Original());
    if (!SwapAllowed())
    {
        return update(object);
    }
    const Choice& choice = ChoiceOf(object);
    if (choice.object != object || !choice.two)
    {
        return update(object);
    }
    g_counts.updatesTwo.fetch_add(1, std::memory_order_relaxed);
    return Swapped([&] { return update(object); });
}

std::uint64_t DynamicUpdate(void* object) noexcept
{
    return Update(g_dynamicUpdateSlot, object);
}

std::uint64_t PointUpdate(void* object) noexcept
{
    return Update(g_pointUpdateSlot, object);
}

/// The AutoSe processor update (footstep detection, gate A's caller): its frame is noted for the distance clip.
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

/// The distance clip's listener: player 1's (the game's answer) unless player 2's is nearer the character. Player 2's
/// position goes through AutoSe's own converter, as the getter sends the singleton's; it has no listener parameter.
void NearerForClip(void* out) noexcept
{
    const void* const frame = t_clipFrame;
    std::array<float, kListenerFields> seen{};
    if (frame == nullptr || g_convert == 0 || !g_twoActive.load(std::memory_order_relaxed) || !g_listeners.Read(seen) ||
        seen[kCount] < 2.0f)
    {
        return;
    }
    const auto convert = std::atomic_ref<ConvertFn>(*reinterpret_cast<ConvertFn*>(g_convert)).load(std::memory_order_relaxed);
    if (convert == nullptr)
    {
        return;
    }
    float       two[3]{};
    const float* converted = convert(two, &seen[kOneX]);
    const auto  character  = Read3(frame, kFramePosition);
    const auto* one        = static_cast<const float*>(out);
    if (converted == nullptr ||
        !listener_choice::NearerForClip(Distance(character.data(), converted), Distance(character.data(), one)))
    {
        return;
    }
    std::memcpy(out, converted, sizeof(float) * 3);  // the fourth float stays the getter's 1.0
    g_counts.clipTwo.fetch_add(1, std::memory_order_relaxed);
}

/// The AutoSe listener position (the footstep detection's clip and warp check; off SEAD's thread): the game's read,
/// never during a swap; for the clip, the nearer player's.
std::uint8_t ReadPosition(void* out) noexcept;

std::uint8_t Position(void* out) noexcept
{
    const bool clip = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_clipReturn;
    g_counts.reads.fetch_add(1, std::memory_order_relaxed);
    const std::uint8_t result = ReadPosition(out);
    if (clip)
    {
        g_counts.clipReads.fetch_add(1, std::memory_order_relaxed);
        if (result != 0)
        {
            NearerForClip(out);
        }
    }
    return result;
}

std::uint8_t ReadPosition(void* out) noexcept
{
    const auto read = reinterpret_cast<PositionFn>(g_positionEntry.Original());
    if (OnSeadThread())
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
        return read(out);  // no clean answer yet on this thread: the game's read, as before this feature
    }
    if (t_answer.result != 0)
    {
        std::memcpy(out, t_answer.position.data(), t_answer.position.size());
    }
    return t_answer.result;
}

/// The footstep detection's LOD level: the game's level from player 1's listener, then from player 2's; the lower.
std::uint64_t Lod(void* callback, void* character) noexcept
{
    const auto level  = reinterpret_cast<LodFn>(g_lodSlot.Original());
    const auto result = level(callback, character);
    g_counts.lods.fetch_add(1, std::memory_order_relaxed);
    const auto one = static_cast<std::int32_t>(result);
    if (listener_choice::LodKeepsFirst(one))
    {
        return result;  // no filter, or the finest level already
    }
    if (!generation::OnGameThread())
    {
        g_counts.lodElsewhere.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    void* const two = generation::PlayerTwoController();
    const auto  vt  = g_pcVTable.load(std::memory_order_relaxed);
    if (two == nullptr || vt == 0 || VTableOf(two) != vt)
    {
        return result;
    }
    void* const saved = t_lodController;
    t_lodController   = two;
    const auto second = level(callback, character);
    t_lodController   = saved;
    g_counts.lodTwo.fetch_add(1, std::memory_order_relaxed);
    const auto other = static_cast<std::int32_t>(second);
    if (!listener_choice::LodLowered(one, other))
    {
        return result;
    }
    g_counts.lodLowered.fetch_add(1, std::memory_order_relaxed);
    g_lodOne.store(one, std::memory_order_relaxed);
    g_lodTwo.store(other, std::memory_order_relaxed);
    return second;
}

/// The player controller's GetAudioListenerPosition: in player 2's LOD pass, player 2's controller answers.
void PcListener(void* controller, void* location, void* front, void* right) noexcept
{
    void* const two = t_lodController;
    reinterpret_cast<PcListenerFn>(g_pcSlot.Original())(two != nullptr ? two : controller, location, front, right);
}

bool Place(const platform::GameImage& image, platform::VTableEntry& entry, std::uintptr_t slot, std::uintptr_t fn,
           void* replacement) noexcept
{
    const auto at = image.Rebase(slot, sizeof(void*));
    const auto to = image.Rebase(fn);
    return at && to && entry.Replace(*at, *to, reinterpret_cast<std::uintptr_t>(replacement));
}

/// A sound class's test and update: both or neither (an update must run from the listener its test chose).
bool PlacePair(const platform::GameImage& image, platform::VTableEntry& test, std::uintptr_t testSlot,
               std::uintptr_t testFn, void* testReplacement, platform::VTableEntry& update, std::uintptr_t updateSlot,
               std::uintptr_t updateFn, void* updateReplacement) noexcept
{
    if (!Place(image, test, testSlot, testFn, testReplacement))
    {
        return false;
    }
    if (Place(image, update, updateSlot, updateFn, updateReplacement))
    {
        return true;
    }
    test.Restore();
    return false;
}

/// Game thread: the controller's listener slot, once player 1's controller exists (its vtable is known only then).
void PlaceControllerSlot() noexcept
{
    if (g_pcSlot.Original() != 0 || g_pcListenerFn == 0 || g_lodSlot.Original() == 0)
    {
        return;
    }
    const auto vt = VTableOf(generation::PlayerOneController());
    if (vt == 0 || vt == g_pcTried)
    {
        return;
    }
    g_pcTried       = vt;
    const bool ok   = g_pcSlot.Replace(vt + kPcListenerSlot, g_pcListenerFn, reinterpret_cast<std::uintptr_t>(&PcListener));
    g_pcVTable.store(ok ? vt : 0, std::memory_order_relaxed);
    platform::Log(L"audio listeners: footstep detection's LOD {}",
                  ok ? L"measured from player 2's listener too (the controller's listener slot)"
                     : L"stays player 1's (player 1's controller's listener slot differs)");
}

std::wstring Metres(float centimetres)
{
    return std::format(L"{:.0f} m", centimetres * kMetres);
}

/// Game thread: the UE listeners as the game thread holds them, against both players' cameras.
void NoteGameListeners() noexcept
{
    const void* device = g_device.load(std::memory_order_relaxed);
    if (device == nullptr)
    {
        return;
    }
    std::int32_t count = 0;
    const void*  data  = nullptr;
    if (!platform::CopyGuarded(&count, static_cast<const std::byte*>(device) + kGameListeners + 8, sizeof(count)) ||
        !platform::CopyGuarded(&data, static_cast<const std::byte*>(device) + kGameListeners, sizeof(data)) ||
        data == nullptr)
    {
        return;
    }
    if (count == g_gameCount)
    {
        return;
    }
    g_gameCount = count;
    std::wstring text;
    const void*  controllers[] = {generation::PlayerOneController(), generation::PlayerTwoController()};
    for (std::int32_t i = 0; i < count && i < 2; ++i)
    {
        float at[3]{};
        if (!platform::CopyGuarded(at, static_cast<const std::byte*>(data) + kGameListenerStride * i + kTranslation,
                                   sizeof(at)))
        {
            continue;
        }
        text += std::format(L"; [{}] at ({:.0f}, {:.0f}, {:.0f})", i, at[0], at[1], at[2]);
        for (int p = 0; p < 2; ++p)
        {
            const charapop::Camera camera = controllers[p] != nullptr ? charapop::CameraOf(controllers[p]) : charapop::Camera{};
            if (camera.read)
            {
                text += std::format(L", {} from player {}'s camera", Metres(Distance(at, camera.location)), p + 1);
            }
        }
    }
    platform::LogDebug(L"audio listeners: the engine holds {} listener(s) on the game thread{}", count, text);
}

void NoteListenerTwo() noexcept
{
    const bool on = g_twoActive.load(std::memory_order_relaxed);
    if (on == g_twoLogged)
    {
        return;
    }
    g_twoLogged = on;
    platform::LogDebug(L"audio listeners: player 2's SEAD listener {}", on ? L"ON: each sound measured from the nearer player"
                                                                          : L"off: every sound measured from player 1");
}

void LogSummary() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - g_summaryAt < kSummaryEvery)
    {
        return;
    }
    const bool first = g_summaryAt == std::chrono::steady_clock::time_point{};
    g_summaryAt      = now;
    auto take = [](std::atomic<unsigned>& count) { return count.exchange(0, std::memory_order_relaxed); };
    const auto pushes   = take(g_counts.pushes);
    const auto checked  = take(g_counts.checked);
    const auto relative = take(g_counts.relative);
    const auto fromTwo  = take(g_counts.fromTwo);
    const auto heardTwo = take(g_counts.audibleTwo);
    const auto heardOne = take(g_counts.audibleOne);
    const auto missed   = take(g_counts.missed);
    const auto noTwo    = take(g_counts.noTwo);
    const auto updates  = take(g_counts.updatesTwo);
    const auto reads    = take(g_counts.reads);
    const auto clips    = take(g_counts.clipReads);
    const auto clipTwo  = take(g_counts.clipTwo);
    const auto waits    = take(g_counts.readWaits);
    const auto held     = take(g_counts.readHeld);
    const auto lods     = take(g_counts.lods);
    const auto lodTwo   = take(g_counts.lodTwo);
    const auto lowered  = take(g_counts.lodLowered);
    const auto away     = take(g_counts.lodElsewhere);
    if (first || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    std::array<float, kListenerFields> seen{};
    std::wstring                       where = L"no push read";
    if (g_listeners.Read(seen))
    {
        where = std::format(L"{:.0f} listener(s); SEAD's own listener {} from [0]", seen[kCount],
                            Metres(Distance(&seen[kSeadX], &seen[kZeroX])));
        if (seen[kCount] >= 2.0f)
        {
            where += std::format(L"; [0] and [1] {} apart", Metres(Distance(&seen[kZeroX], &seen[kOneX])));
        }
    }
    platform::LogDebug(L"audio listeners ({} s): {} push(es) on the audio thread (thread {}; {}); player 2's listener {}; "
                       L"SEAD sound checks {} on SEAD's thread (thread {}): from player 2's listener {} ({} audible), "
                       L"audible from player 1's {}, culled within player 2's range {}, listener-relative {}, player 2's "
                       L"listener off {}; updates from player 2's listener {}",
                       kSummaryEvery.count(), pushes, g_audioThread.load(std::memory_order_relaxed), where,
                       g_twoActive.load(std::memory_order_relaxed) ? L"on" : L"off", checked,
                       g_seadThread.load(std::memory_order_relaxed), fromTwo, heardTwo, heardOne, missed, relative, noTwo,
                       updates);
    platform::LogDebug(L"audio listeners ({} s): AutoSe listener reads {} (the distance clip's {}, {} of them from player "
                       L"2's listener; {} waited out a swap, {} took the latest clean answer); footstep LOD levels {} "
                       L"(asked for player 2 {}, lowered {}{}; off the game thread {})",
                       kSummaryEvery.count(), reads, clips, clipTwo, waits, held, lods, lodTwo, lowered,
                       lowered != 0 ? std::format(L", latest {} -> {}", g_lodOne.load(std::memory_order_relaxed),
                                                  g_lodTwo.load(std::memory_order_relaxed))
                                    : std::wstring{},
                       away);
    std::array<float, kExampleFields> example{};
    const auto version = g_example.version.load(std::memory_order_relaxed);
    if (version != g_exampleSeen && g_example.Read(example))
    {
        g_exampleSeen = version;
        platform::LogDebug(L"audio listeners: the latest sound player 2's listener would hear but its chosen listener "
                           L"culled: a {} sound at ({:.0f}, {:.0f}, {:.0f}), {} from player 1's listener, {} from player "
                           L"2's; its audible range {} (x {:.2f})",
                           example[kKind] > 0.5f ? L"moving" : L"placed", example[kWhereX], example[kWhereX + 1],
                           example[kWhereX + 2], Metres(example[kToOne]), Metres(example[kToTwo]),
                           Metres(example[kRange]), example[kMagnify]);
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto global = image.Rebase(kListenerGlobal, sizeof(void*));
    g_listenerGlobal.store(global ? reinterpret_cast<void**>(*global) : nullptr, std::memory_order_relaxed);
    const bool known = global.has_value();

    // Logging (and the mailbox): the push.
    const bool push = Place(image, g_pushSlot, kPushSlot, kPush, reinterpret_cast<void*>(&Push));

    // The AutoSe reader first: once a swap can happen, it must already be shielded.
    bool       shield = false;
    const auto read   = known ? platform::FindFunction(image, kGetPosition) : std::nullopt;
    const auto clip   = image.Rebase(kClipReturn);
    if (read && clip)
    {
        g_clipReturn     = *clip;
        const auto first = std::span(kGetPositionBytes).first(kGetPositionFirst);
        shield = g_positionEntry.Install(*read, image.EntryInstruction(kGetPosition.address, first),
                                         reinterpret_cast<std::uintptr_t>(&Position));
    }

    // L1: the sounds' tests and updates, then the listener's per-frame setter (it turns the swaps on).
    const auto set     = shield ? platform::FindFunction(image, kListenerSet) : std::nullopt;
    g_set              = set ? reinterpret_cast<SetFn>(*set) : nullptr;
    const bool moving  = g_set != nullptr &&
                         PlacePair(image, g_dynamicSlot, kDynamicSlot, kDynamic, reinterpret_cast<void*>(&Dynamic),
                                   g_dynamicUpdateSlot, kDynamicUpdateSlot, kDynamicUpdate,
                                   reinterpret_cast<void*>(&DynamicUpdate));
    const bool placed  = g_set != nullptr &&
                         PlacePair(image, g_pointSlot, kPointSlot, kPoint, reinterpret_cast<void*>(&Point),
                                   g_pointUpdateSlot, kPointUpdateSlot, kBaseUpdate, reinterpret_cast<void*>(&PointUpdate));
    const auto site    = image.Rebase(kVelocityCall, 5);
    const auto target  = image.Rebase(kVelocity);
    const bool second  = push && (moving || placed) && site && target &&
                         g_velocityCall.Install(*site, *target, reinterpret_cast<std::uintptr_t>(&Velocity));

    // Gate A: the AutoSe processor update's frame, for the clip's nearer listener (only once the swaps can run).
    const auto convert     = image.Rebase(kConvert, sizeof(void*));
    const auto updateSite  = image.Rebase(kClipUpdateCall, 5);
    const auto update      = image.Rebase(kClipUpdate);
    g_convert              = convert ? *convert : 0;
    const bool clipNearest = second && g_convert != 0 && updateSite && update &&
                             g_clipCall.Install(*updateSite, *update, reinterpret_cast<std::uintptr_t>(&ClipUpdate));

    // L3: the footstep LOD; its controller slot follows from Tick.
    const auto pc  = image.Rebase(kPcListener);
    g_pcListenerFn = pc ? *pc : 0;
    const bool lod = g_pcListenerFn != 0 && Place(image, g_lodSlot, kLodSlot, kLod, reinterpret_cast<void*>(&Lod));

    auto outcome = [](bool ok) { return ok ? L"on" : L"OFF (a check failed)"; };
    platform::Log(L"audio listeners: player 2's SEAD listener {} (moving sounds {}, placed sounds {}; the AutoSe reader "
                  L"shielded {}); footstep detection from the nearer listener: the distance clip {}, the LOD {}; "
                  L"logging: the push {}",
                  outcome(second), outcome(moving), outcome(placed), outcome(shield), outcome(clipNearest), outcome(lod),
                  outcome(push));
}

void Remove() noexcept
{
    for (platform::CallRedirect* call : {&g_velocityCall, &g_clipCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"audio listeners: a call was changed after us; left in place (it passes through)");
        }
    }
    g_twoActive.store(false, std::memory_order_relaxed);
    for (platform::VTableEntry* entry : {&g_pcSlot, &g_lodSlot, &g_dynamicSlot, &g_dynamicUpdateSlot, &g_pointSlot,
                                         &g_pointUpdateSlot, &g_pushSlot})
    {
        if (entry->Original() != 0)
        {
            entry->Restore();
        }
    }
    if (g_positionEntry.IsInstalled() && !g_positionEntry.Restore())
    {
        platform::Log(L"audio listeners: the AutoSe reader's entry was changed after us; left in place (it passes through)");
    }
}

void Tick() noexcept
{
    if (!generation::OnGameThread())
    {
        return;
    }
    try
    {
        PlaceControllerSlot();
        if (!platform::Enabled(platform::Level::Debug))
        {
            return;
        }
        NoteGameListeners();
        NoteListenerTwo();
        LogSummary();
    }
    catch (...)
    {
    }
}

}  // namespace lss::player_two::audio_listener_two
