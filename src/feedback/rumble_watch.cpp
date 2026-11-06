#include "feedback/rumble_watch.h"

#include "facts/engine.h"
#include "platform/every.h"
#include "platform/frame_watch.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/generation.h"
#include "players/object_fields.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>

namespace lss::feedback::rumble_watch {

namespace {

using Clock = std::chrono::steady_clock;

// RE-p2-rumble.md §2.2, §5 (this build).
// AEndPlayerController vt+0xC90 (vtable 0x1467C85A0): SendFFValuesToInputInterface(PC, IInputInterface*, id), stock.
constexpr std::uint8_t kSendBytes[] = {0xB8, 0x68, 0x00, 0x00, 0x00};  // mov eax, 68h (then the stack probe)
constexpr platform::SlotFact kSendSlot{L"the controller's SendFFValuesToInputInterface slot", 0x1467C9230,
                                       {L"SendFFValuesToInputInterface", 0x1407BF920, kSendBytes}};
// The XInput device's SetChannelValues (its vtable 0x146959448, slot 6): (device, id, const FForceFeedbackValues*).
constexpr std::uint8_t kPadSetBytes[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x60, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kPadSlot{L"the XInput device's SetChannelValues slot", 0x146959478,
                                      {L"XInputDevice_SetChannelValues", 0x1407BF308, kPadSetBytes}};
// The platform application (pointer global); the send's interface is app+0x80, so the code is at interface+0x160.
using facts::kPlatformApplication;
using facts::kPlatformAppDeviceCode;
// APlayerController (stock 4.26 fields, this build's offsets).
constexpr std::uint32_t kPlayerInput   = 0x350;
constexpr std::uint32_t kEffectsNum    = 0x360;  // ActiveForceFeedbackEffects.Num
constexpr std::uint32_t kValues        = 0x3B8;  // FForceFeedbackValues: LeftLarge, LeftSmall, RightLarge, RightSmall
constexpr std::uint32_t kFlags         = 0x450;
constexpr std::uint8_t  kFeedbackOn    = 0x20;   // bForceFeedbackEnabled
constexpr std::uint32_t kFeedbackScale = 0x454;

constexpr std::size_t kMaxEvents = 24;    // per frame
constexpr auto        kStatusEvery = std::chrono::seconds(10);

using SendFn = std::uint64_t (*)(void* controller, void* input, std::int32_t id);
using PadFn  = std::uint64_t (*)(void* device, std::int32_t id, const void* values);

struct Event
{
    bool                 send   = false;  // a PC's send; else a pad write
    int                  who    = 0;      // the send's PC by player number (1, then the seats' 2 and 3), or 0 (another)
    std::int32_t         id     = 0;
    std::int32_t         code   = 0;      // the send: the device code then
    std::int32_t         effects = -1;    // the send: the PC's active effects
    bool                 on     = true;   // the send: bForceFeedbackEnabled
    int                  depth  = 0;      // the pad write: 1 the outer call, 2 inside a negative code's call
    std::array<float, 4> values{};
    bool Zero() const noexcept { return values[0] == 0.0f && values[1] == 0.0f && values[2] == 0.0f && values[3] == 0.0f; }
    bool SameShape(const Event& other) const noexcept
    {
        return send == other.send && who == other.who && id == other.id && code == other.code && on == other.on &&
               depth == other.depth && Zero() == other.Zero();
    }
};

struct Frame
{
    std::uint64_t                  index = 0;
    std::array<Event, kMaxEvents>  events{};
    std::size_t                    count = 0;
    std::size_t                    dropped = 0;
    bool NonZero() const noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!events[i].Zero())
            {
                return true;
            }
        }
        return false;
    }
    bool SameShape(const Frame& other) const noexcept
    {
        if (count != other.count)
        {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!events[i].SameShape(other.events[i]))
            {
                return false;
            }
        }
        return true;
    }
};

platform::VTableEntry g_send;
platform::VTableEntry g_pad;
std::uintptr_t        g_app = 0;  // the pointer global's live address

// Game thread.
Frame             g_frame;
Frame             g_logged;  // the last frame logged (its shape)
int               g_depth = 0;
platform::Every g_status{kStatusEvery};
struct Counts
{
    std::uint32_t frames = 0, sendsOther = 0, padWrites = 0, nonZeroFrames = 0;
    std::uint32_t sends[players::kMaxPlayers]{};  // by player: 0 player 1's controller, then each seat's
};
std::atomic<std::uint32_t> g_offThread{0};  // calls off the game thread (counted from any thread)

Counts g_counts;

std::array<float, 4> ValuesAt(const void* at) noexcept
{
    std::array<float, 4> values{};
    if (at != nullptr)
    {
        platform::CopyGuarded(values.data(), at, sizeof(values));
    }
    return values;
}

std::wstring Values(const Event& event)
{
    return event.Zero() ? std::wstring(L"0")
                        : std::format(L"[{:.2f} {:.2f} {:.2f} {:.2f}]", event.values[0], event.values[1], event.values[2],
                                      event.values[3]);
}

std::wstring Describe(const Frame& frame)
{
    std::wstring text;
    for (std::size_t i = 0; i < frame.count; ++i)
    {
        const Event& event = frame.events[i];
        text += i == 0 ? L"" : L" | ";
        if (event.send)
        {
            text += std::format(L"{} send id {} (device code {}){} {} effects {}",
                                event.who == 0 ? std::wstring(L"another PC") : std::format(L"PC{}", event.who), event.id,
                                event.code, event.on ? L"" : L" FEEDBACK OFF", Values(event), event.effects);
        }
        else
        {
            text += std::format(L"{}pad {} {}", event.depth > 1 ? L"  " : L"", event.id, Values(event));
        }
    }
    if (frame.dropped != 0)
    {
        text += std::format(L" | (+{} not kept)", frame.dropped);
    }
    return text;
}

/// Logs the frame just ended if any value was non-zero or its shape changed.
void Flush() noexcept
{
    if (g_frame.count == 0)
    {
        return;
    }
    ++g_counts.frames;
    const bool nonZero = g_frame.NonZero();
    g_counts.nonZeroFrames += nonZero ? 1 : 0;
    if (platform::Enabled(platform::Level::Trace) && (nonZero || !g_frame.SameShape(g_logged)))
    {
        g_logged = g_frame;
        try
        {
            platform::LogTrace(L"rumble: frame {}: {}", g_frame.index, Describe(g_frame));
        }
        catch (...)
        {
        }
    }
    g_frame.count   = 0;
    g_frame.dropped = 0;
}

void LogStatus() noexcept
{
    const auto now = Clock::now();
    if (!players::generation::AnyActive() || !platform::Enabled(platform::Level::Debug) || !g_status.Due(now))
    {
        return;
    }
    const auto offThread = platform::Take(g_offThread);
    const auto* app = reinterpret_cast<const std::uint8_t*>(ReadAt<std::uintptr_t>(g_app));
    try
    {
        // Player 1's controller, then the controller of each seat whose player is in.
        std::wstring sends, controllers;
        for (int p = 0; p < players::kMaxPlayers; ++p)
        {
            const void* controller = players::generation::ControllerOf(p);
            if (p != 0 && !players::generation::Active(p))
            {
                continue;
            }
            sends += std::format(L"PC{} {}, ", p + 1, g_counts.sends[p]);
            controllers += std::format(L"; PC{} feedback {} scale {:.2f} effects {}", p + 1,
                                       (ReadField<std::uint8_t>(controller, kFlags) & kFeedbackOn) != 0 ? L"on" : L"OFF",
                                       ReadField<float>(controller, kFeedbackScale), EffectCount(controller));
            if (p != 0)
            {
                controllers += std::format(L", PlayerInput {}",
                                           ReadField<std::uintptr_t>(controller, kPlayerInput) != 0 ? L"present" : L"MISSING");
            }
        }
        platform::LogDebug(L"rumble (10 s): {} frame(s), {} with a non-zero value; sends {}other {}; {} pad write(s){}; device "
                           L"code now {}{}",
                           g_counts.frames, g_counts.nonZeroFrames, sends, g_counts.sendsOther, g_counts.padWrites,
                           offThread != 0 ? std::format(L", {} call(s) off the game thread", offThread) : std::wstring(),
                           ReadField<std::int32_t>(app, kPlatformAppDeviceCode), controllers);
    }
    catch (...)
    {
    }
    g_counts = {};
}

/// Starts a new frame record when the engine's frame moved on.
void Begin() noexcept
{
    const auto index = platform::frame_watch::FrameIndex();
    if (index != g_frame.index)
    {
        Flush();
        g_frame.index = index;
        LogStatus();
    }
}

void Add(const Event& event) noexcept
{
    if (g_frame.count < kMaxEvents)
    {
        g_frame.events[g_frame.count++] = event;
    }
    else
    {
        ++g_frame.dropped;
    }
}

std::uint64_t Send(void* controller, void* input, std::int32_t id) noexcept
{
    const auto original = reinterpret_cast<SendFn>(g_send.Original());
    if (!players::generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return original(controller, input, id);
    }
    Begin();
    Event event;
    event.send    = true;
    const int seat = players::generation::SeatOfController(controller);
    event.who     = controller == players::generation::PlayerOneController() ? 1 : players::SeatIndex(seat) ? seat + 1 : 0;
    event.id      = id;
    event.code    = ReadField<std::int32_t>(input, kPlatformAppDeviceCode - 0x80);
    event.effects = EffectCount(controller);
    event.on      = (ReadField<std::uint8_t>(controller, kFlags) & kFeedbackOn) != 0;
    event.values  = ValuesAt(static_cast<const std::uint8_t*>(controller) + kValues);
    if (!event.on)
    {
        event.values = {};  // what the send passes then
    }
    (event.who != 0 ? g_counts.sends[event.who - 1] : g_counts.sendsOther) += 1;
    Add(event);
    return original(controller, input, id);
}

std::uint64_t PadSet(void* device, std::int32_t id, const void* values) noexcept
{
    const auto original = reinterpret_cast<PadFn>(g_pad.Original());
    if (!players::generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return original(device, id, values);
    }
    Begin();
    Event event;
    event.id     = id;
    event.depth  = g_depth + 1;
    event.values = ValuesAt(values);
    ++g_counts.padWrites;
    Add(event);
    ++g_depth;
    const auto result = original(device, id, values);
    --g_depth;
    return result;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto app   = image.Rebase(kPlatformApplication, sizeof(void*));
    g_app            = app ? *app : 0;
    const bool sends = platform::ReplaceSlot(image, kSendSlot, reinterpret_cast<std::uintptr_t>(&Send), g_send);
    const bool pads  = platform::ReplaceSlot(image, kPadSlot, reinterpret_cast<std::uintptr_t>(&PadSet), g_pad);
    platform::Log(L"rumble watch (logging only): the controllers' sends {}, the XInput pad writes {}",
                  sends ? L"watched" : L"NOT watched (the entry is not what the research read)",
                  pads ? L"watched" : L"NOT watched (the entry is not what the research read)");
}

std::int32_t EffectCount(const void* controller) noexcept
{
    return controller == nullptr ? -1 : ReadField<std::int32_t>(controller, kEffectsNum);
}

void Remove() noexcept
{
    for (platform::VTableEntry* entry : {&g_pad, &g_send})
    {
        if (!entry->Restore())
        {
            platform::Log(L"rumble watch: an entry was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::feedback::rumble_watch
