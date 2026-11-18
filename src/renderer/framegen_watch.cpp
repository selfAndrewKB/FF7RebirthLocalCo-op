#include "renderer/framegen_watch.h"

#include "facts/streamline.h"
#include "platform/command_registry.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "platform/vtable_entry.h"
#include "renderer/frame_generation.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lss::renderer::framegen_watch {

namespace {

namespace fg = renderer::frame_generation;

constexpr std::uint32_t kEveryViewport = 0xFFFFFFFF;  // `fg-off-viewport -1`: every viewport's options sent off
constexpr double        kRadToDeg      = 57.29577951308232;

// The buffer types the game tags (FG_MapTagToSLBufferType), in the summary's order.
struct BufferKind
{
    std::uint32_t  type;
    const wchar_t* name;
};
constexpr std::array<BufferKind, 5> kKinds{{{facts::kSlDepth, L"depth"},
                                            {facts::kSlMotionVectors, L"motion vectors"},
                                            {facts::kSlHudLessColor, L"HUD-less colour"},
                                            {facts::kSlUiColorAlpha, L"UI"},
                                            {facts::kSlBackbuffer, L"back buffer"}}};
constexpr int kBackBufferKind = 4;

struct TagSeen
{
    std::uint64_t                calls   = 0;  // this window
    std::uintptr_t               native  = 0;
    std::array<std::uint32_t, 4> extent{};     // top, left, width, height
    std::uint32_t                lifeIn  = 0;
    std::uint32_t                lifeOut = 0;
    bool                         command = false;
    std::int32_t                 result  = 0;
    bool                         seen    = false;
};

struct ViewportSeen
{
    // Options
    std::uint64_t options         = 0;  // calls this window
    std::uint32_t modeIn          = 0;
    std::uint32_t modeOut         = 0;
    std::uint32_t frames          = 0;
    std::uint32_t flags           = 0;
    std::uint32_t dynamicW        = 0;
    std::uint32_t dynamicH        = 0;
    std::uint64_t dynamicChanges  = 0;  // this window
    std::int32_t  optionsResult   = 0;
    // Constants
    std::uint64_t         constants       = 0;  // calls this window
    std::int32_t          constantsResult = 0;
    std::uint32_t         lastFrame       = 0;  // the frame of the last constants call (0: none yet)
    std::uint32_t         firstFrame      = 0;  // this window's first
    std::uint64_t         unread          = 0;  // this window: calls whose frame token could not be read
    bool                  dynamicKnown    = false;  // a dynamic size was seen (kept across windows)
    std::uint64_t         repeats         = 0;  // this window: a second constants call for the same frame
    std::uint64_t         skipped         = 0;  // this window: frames with no constants call between two calls
    std::uint64_t         resets          = 0;  // this window
    std::array<float, 3>  pos{};
    std::array<float, 3>  fwd{};
    std::array<float, 2>  jitter{};
    float                 fov             = 0.0f;  // radians
    float                 aspect          = 0.0f;
    // Frees and tags
    std::uint64_t                      frees = 0;
    std::array<TagSeen, kKinds.size()> tags{};
};

/// One tag of the frame in flight, for the same-frame check.
struct FrameTag
{
    std::uintptr_t native;
    std::uint32_t  viewport;
    int            kind;
    bool           untilPresent;  // sent valid until present (not copied at tag time)
};

using SetConstantsFn = std::int32_t (*)(const void*, const void*, const void*);
using FreeFn         = std::int32_t (*)(std::uint32_t, const void*);
using SetOptionsFn   = std::int32_t (*)(const void*, const void*);
using FrameIndexFn   = std::uint32_t (*)(const void*);  // sl::FrameToken's first virtual: operator uint32_t

platform::VTableEntry g_setConstants;
platform::VTableEntry g_free;
platform::VTableEntry g_setOptions;

std::atomic<bool>          g_installed{false};
std::atomic<std::uint32_t> g_offViewport{0};

// Any thread (the wrappers run on the render and RHI threads); the summary takes it on the game thread.
std::mutex                            g_lock;
std::map<std::uint32_t, ViewportSeen> g_viewports;
std::vector<std::uint32_t>            g_backBufferOrder;  // this window's back-buffer tags, by viewport, the first few
std::vector<std::uint32_t>            g_optionsOrder;     // this window's options calls, by viewport, the first few
std::uint64_t                         g_windowCalls = 0;
std::vector<FrameTag>                 g_frameTags;        // the frame in flight's input tags
bool                                  g_inPresent   = false;  // the last tag was a back-buffer tag
std::uint64_t                         g_frames      = 0;  // frames closed this window
std::array<std::uint64_t, kKinds.size()> g_sharedFrames{};  // this window: frames with a kind shared until present
std::array<bool, kKinds.size()>          g_sharedLogged{};  // the first sharing of each kind logged at Info

// Game thread.
std::uintptr_t        g_enabledAt   = 0;
std::uintptr_t        g_activeAt    = 0;
int                   g_lastEnabled = -1;
int                   g_lastActive  = -1;
ULONGLONG             g_nextSummary = 0;
constexpr ULONGLONG   kSummaryMs    = 5000;
constexpr std::size_t kOrderKept    = 16;
constexpr std::size_t kFrameTagsKept = 64;  // a frame's input tags: four per view

std::uint32_t ViewportOf(const void* handle) noexcept
{
    std::uint32_t id = 0xFFFFFFFF;
    platform::Read(handle, facts::kSlViewportId, id);
    return id;
}

template <class T>
T At(const std::uint8_t* bytes, std::size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

int KindIndex(std::uint32_t type) noexcept
{
    for (std::size_t i = 0; i < kKinds.size(); ++i)
    {
        if (kKinds[i].type == type)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void NoteOrder(std::vector<std::uint32_t>& order, std::uint32_t viewport)
{
    if (order.size() < kOrderKept)
    {
        order.push_back(viewport);
    }
}

/// Closes the frame in flight (under g_lock): any resource of one kind tagged for two viewports and sent valid until
/// present means a viewport reads another's at present.
void CloseFrame()
{
    ++g_frames;
    for (std::size_t k = 0; k < kKinds.size(); ++k)
    {
        if (static_cast<int>(k) == kBackBufferKind)
        {
            continue;  // the back buffer is one texture by design: each viewport tags its own part
        }
        bool shared = false;
        for (std::size_t i = 0; i < g_frameTags.size() && !shared; ++i)
        {
            const FrameTag& a = g_frameTags[i];
            for (std::size_t j = i + 1; j < g_frameTags.size() && !shared; ++j)
            {
                const FrameTag& b = g_frameTags[j];
                shared = a.kind == static_cast<int>(k) && b.kind == a.kind && a.native == b.native && a.native != 0 &&
                         a.viewport != b.viewport && (a.untilPresent || b.untilPresent);
            }
        }
        if (shared)
        {
            ++g_sharedFrames[k];
            if (!g_sharedLogged[k])
            {
                g_sharedLogged[k] = true;
                platform::Log(L"frame generation watch: {} tagged for two viewports in one frame, valid until present: "
                              L"by present every one of them reads the last one's",
                              kKinds[k].name);
            }
        }
    }
    g_frameTags.clear();
}

/// renderer/frame_generation's observer: each slSetTag call as forwarded.
void ObserveTag(const fg::TagCall& call)
{
    try
    {
        const std::uint32_t         id = ViewportOf(call.viewport);
        std::lock_guard<std::mutex> lock(g_lock);
        ViewportSeen&               seen = g_viewports[id];
        ++g_windowCalls;
        for (std::uint32_t i = 0; i < call.count; ++i)
        {
            const std::uint8_t* tag  = call.sent + i * facts::kSlTagSize;
            const int           kind = KindIndex(At<std::uint32_t>(tag, facts::kSlTagType));
            if (kind < 0)
            {
                continue;
            }
            TagSeen&       t        = seen.tags[kind];
            const auto     resource = At<std::uintptr_t>(tag, facts::kSlTagResource);
            std::uintptr_t native   = 0;
            if (resource != 0)
            {
                platform::Read(reinterpret_cast<const void*>(resource), facts::kSlResourceNative, native);
            }
            ++t.calls;
            t.seen    = true;
            t.native  = native;
            t.extent  = At<std::array<std::uint32_t, 4>>(tag, facts::kSlTagExtent);
            t.lifeIn  = call.lifeIn[i];
            t.lifeOut = At<std::uint32_t>(tag, facts::kSlTagLifecycle);
            t.command = call.command != nullptr;
            t.result  = call.result;
            if (kind == kBackBufferKind)
            {
                NoteOrder(g_backBufferOrder, id);
                g_inPresent = true;
                continue;
            }
            // The first input tag after the present's back-buffer tags starts the next frame.
            if (g_inPresent)
            {
                CloseFrame();
                g_inPresent = false;
            }
            if (g_frameTags.size() < kFrameTagsKept)
            {
                g_frameTags.push_back({native, id, kind, t.lifeOut != facts::kSlOnlyValidNow});
            }
        }
    }
    catch (...)
    {
    }
}

/// The frame a constants call belongs to, through sl::FrameToken's own operator; 0 if it cannot be read.
std::uint32_t FrameOf(const void* token) noexcept
{
    void*         vtable = nullptr;
    FrameIndexFn  index  = nullptr;
    std::uint32_t frame  = 0;
    if (token == nullptr || !platform::Read(token, vtable) || vtable == nullptr ||
        !platform::Read(static_cast<const std::uint8_t*>(vtable) + facts::kSlFrameTokenIndexSlot * sizeof(void*), index) ||
        index == nullptr || !platform::CallGuarded(index, frame, token))
    {
        return 0;
    }
    return frame;
}

std::int32_t SetConstants(const void* constants, const void* frame, const void* viewport)
{
    const std::int32_t result = reinterpret_cast<SetConstantsFn>(g_setConstants.Original())(constants, frame, viewport);
    try
    {
        std::array<std::uint8_t, facts::kSlConstantsSize> copy{};
        const bool          read  = constants != nullptr && platform::CopyGuarded(copy.data(), constants, copy.size());
        const std::uint32_t index = FrameOf(frame);
        const std::uint32_t id    = ViewportOf(viewport);
        bool                reset = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            ViewportSeen&               seen = g_viewports[id];
            ++seen.constants;
            ++g_windowCalls;
            seen.constantsResult = result;
            seen.unread += index == 0 ? 1 : 0;
            if (index != 0)
            {
                if (seen.firstFrame == 0)
                {
                    seen.firstFrame = index;
                }
                if (index == seen.lastFrame)
                {
                    ++seen.repeats;
                }
                else if (seen.lastFrame != 0 && index > seen.lastFrame + 1)
                {
                    seen.skipped += index - seen.lastFrame - 1;
                }
                seen.lastFrame = index;
            }
            if (read)
            {
                seen.pos    = At<std::array<float, 3>>(copy.data(), facts::kSlConstantsPos);
                seen.fwd    = At<std::array<float, 3>>(copy.data(), facts::kSlConstantsFwd);
                seen.jitter = At<std::array<float, 2>>(copy.data(), facts::kSlConstantsJitter);
                seen.fov    = At<float>(copy.data(), facts::kSlConstantsFov);
                seen.aspect = At<float>(copy.data(), facts::kSlConstantsAspect);
                reset       = copy[facts::kSlConstantsReset] != 0;
                seen.resets += reset ? 1 : 0;
            }
        }
        if (reset)
        {
            platform::LogDebug(L"frame generation watch: viewport {} resets its generated frames (frame {})", id, index);
        }
    }
    catch (...)
    {
    }
    return result;
}

std::int32_t FreeResources(std::uint32_t feature, const void* viewport)
{
    const std::int32_t  result = reinterpret_cast<FreeFn>(g_free.Original())(feature, viewport);
    const std::uint32_t id     = ViewportOf(viewport);
    platform::LogDebug(L"frame generation watch: viewport {}'s resources freed (feature {}, result {})", id, feature,
                       result);
    try
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ++g_viewports[id].frees;
    }
    catch (...)
    {
    }
    return result;
}

std::int32_t SetOptions(const void* viewport, const void* options)
{
    const std::uint32_t                          id = ViewportOf(viewport);
    std::array<std::uint8_t, facts::kSlOptionsSize> copy{};
    const bool  copied  = options != nullptr && platform::CopyGuarded(copy.data(), options, facts::kSlOptionsSize);
    const auto  modeIn  = copied ? At<std::uint32_t>(copy.data(), facts::kSlOptionsMode) : 0u;
    const void* forward = options;
    const std::uint32_t off = g_offViewport.load(std::memory_order_relaxed);
    if (copied && off != 0 && (id == off || off == kEveryViewport) && modeIn != facts::kSlModeOff)
    {
        std::memcpy(copy.data() + facts::kSlOptionsMode, &facts::kSlModeOff, sizeof(facts::kSlModeOff));
        forward = copy.data();
    }
    const std::int32_t result = reinterpret_cast<SetOptionsFn>(g_setOptions.Original())(viewport, forward);
    if (!copied)
    {
        return result;
    }
    try
    {
        const auto w = At<std::uint32_t>(copy.data(), facts::kSlOptionsDynamic);
        const auto h = At<std::uint32_t>(copy.data(), facts::kSlOptionsDynamic + sizeof(std::uint32_t));
        std::uint32_t oldW = 0, oldH = 0;
        bool          changed = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            ViewportSeen&               seen = g_viewports[id];
            ++seen.options;
            ++g_windowCalls;
            changed = seen.dynamicKnown && (seen.dynamicW != w || seen.dynamicH != h);
            oldW    = seen.dynamicW;
            oldH    = seen.dynamicH;
            seen.dynamicChanges += changed ? 1 : 0;
            seen.dynamicKnown = true;
            seen.modeIn        = modeIn;
            seen.modeOut       = At<std::uint32_t>(copy.data(), facts::kSlOptionsMode);
            seen.frames        = At<std::uint32_t>(copy.data(), facts::kSlOptionsFrames);
            seen.flags         = At<std::uint32_t>(copy.data(), facts::kSlOptionsFlags);
            seen.dynamicW      = w;
            seen.dynamicH      = h;
            seen.optionsResult = result;
            NoteOrder(g_optionsOrder, id);
        }
        if (changed)
        {
            platform::LogDebug(L"frame generation watch: viewport {}'s dynamic size {}x{} -> {}x{}", id, oldW, oldH, w, h);
        }
    }
    catch (...)
    {
    }
    return result;
}

struct Slot
{
    const wchar_t*         name;
    std::uintptr_t         ida;
    platform::VTableEntry* entry;
    std::uintptr_t         replacement;
};

void ResetWindow()
{
    g_viewports.clear();
    g_backBufferOrder.clear();
    g_optionsOrder.clear();
    g_frameTags.clear();
    g_windowCalls  = 0;
    g_frames       = 0;
    g_inPresent    = false;
    g_sharedFrames = {};
    g_sharedLogged = {};
}

/// Places the three wrappers and the tag observer, all or none; `why` says what refused. Game thread.
bool Install(std::wstring& why)
{
    if (g_installed.load())
    {
        return true;
    }
    if (!fg::TagWrapperPlaced())
    {
        why = L"renderer/frame_generation's slSetTag wrapper is not in (Streamline not loaded yet, or it stood aside: see "
              L"its start-up line)";
        return false;
    }
    const auto image = platform::GameImage::Locate();
    if (!image)
    {
        why = L"the game's image was not found";
        return false;
    }
    const std::array<Slot, 3> slots{
        {{L"slSetConstants", facts::kSlSetConstants, &g_setConstants, reinterpret_cast<std::uintptr_t>(&SetConstants)},
         {L"slFreeResources", facts::kSlFreeResources, &g_free, reinterpret_cast<std::uintptr_t>(&FreeResources)},
         {L"slDLSSGSetOptions", facts::kSlDLSSGSetOptions, &g_setOptions, reinterpret_cast<std::uintptr_t>(&SetOptions)}}};
    std::array<std::uintptr_t, 3> at{};
    std::array<std::uintptr_t, 3> held{};
    std::wstring                  found;
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        const auto live = image->Rebase(slots[i].ida, sizeof(void*));
        if (!live || !platform::Read(*live, held[i]))
        {
            why = std::format(L"{}'s pointer global could not be read", slots[i].name);
            return false;
        }
        at[i]                    = *live;
        const std::wstring owner = held[i] != 0 ? platform::ModuleNameAt(held[i]) : std::wstring{};
        if (owner.rfind(L"sl.", 0) != 0)
        {
            why = held[i] == 0 ? std::format(L"{} is not loaded (the game loads DLSS-G's functions only where frame "
                                             L"generation can run)",
                                             slots[i].name)
                               : std::format(L"{} points into '{}', not a Streamline module (another tool changed it?)",
                                             slots[i].name, owner);
            return false;
        }
        found += std::format(L"{}{} in {}", i == 0 ? L"" : L", ", slots[i].name, owner);
    }
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        if (!slots[i].entry->Replace(at[i], held[i], slots[i].replacement))
        {
            std::wstring stuck;
            for (std::size_t j = 0; j < i; ++j)
            {
                if (!slots[j].entry->Restore())
                {
                    stuck += std::format(L"{}{}", stuck.empty() ? L"" : L", ", slots[j].name);
                }
            }
            why = std::format(L"{}'s pointer could not be swapped (changed meanwhile, or not writable){}", slots[i].name,
                              stuck.empty() ? std::wstring{}
                                            : std::format(L"; {} changed after us and stays wrapped (it forwards); "
                                                          L"a later fg-watch on is refused",
                                                          stuck));
            return false;
        }
    }
    g_enabledAt = image->Rebase(facts::kFgEnabled).value_or(0);
    g_activeAt  = image->Rebase(facts::kFgActiveThisFrame).value_or(0);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ResetWindow();
    }
    fg::SetTagObserver(&ObserveTag);
    g_lastEnabled = g_lastActive = -1;
    g_nextSummary = GetTickCount64() + kSummaryMs;
    g_installed.store(true);
    platform::Log(L"frame generation watch: on ({}; the tags through renderer/frame_generation's wrapper); every call "
                  L"forwarded as the game makes it",
                  found);
    return true;
}

std::wstring Hex(std::uintptr_t value)
{
    return value == 0 ? std::wstring(L"none") : std::format(L"{:#x}", value);
}

std::wstring Ids(const std::vector<std::uint32_t>& order)
{
    std::wstring text;
    for (const std::uint32_t id : order)
    {
        text += std::format(L"{}{}", text.empty() ? L"" : L" ", id);
    }
    return text.empty() ? std::wstring(L"none") : text;
}

const wchar_t* TagsNowText(fg::TagsNow now) noexcept
{
    return now == fg::TagsNow::Off ? L"off" : now == fg::TagsNow::Depth ? L"depth" : L"all";
}

void LogSummary()
{
    std::map<std::uint32_t, ViewportSeen>    viewports;
    std::vector<std::uint32_t>               backBuffers;
    std::vector<std::uint32_t>               options;
    std::uint64_t                            calls  = 0;
    std::uint64_t                            frames = 0;
    std::array<std::uint64_t, kKinds.size()> shared{};
    {
        std::lock_guard<std::mutex> lock(g_lock);
        viewports   = g_viewports;
        backBuffers = std::move(g_backBufferOrder);
        options     = std::move(g_optionsOrder);
        calls       = g_windowCalls;
        frames      = g_frames;
        shared      = g_sharedFrames;
        g_backBufferOrder.clear();
        g_optionsOrder.clear();
        g_windowCalls  = 0;
        g_frames       = 0;
        g_sharedFrames = {};
        for (auto& [id, seen] : g_viewports)
        {
            seen.options = seen.constants = seen.frees = seen.repeats = seen.skipped = seen.resets = seen.unread = 0;
            seen.dynamicChanges = 0;
            seen.firstFrame     = 0;
            for (TagSeen& t : seen.tags)
            {
                t.calls = 0;
            }
        }
    }
    if (calls == 0)
    {
        return;
    }
    const std::uint32_t off = g_offViewport.load();
    platform::Log(L"frame generation watch: {} call(s) in {:.0f} s over {} viewport(s), {} frame(s); options order {}; "
                  L"back-buffer tag order {}; switches: viewport off {}, tags copied at tag time {}",
                  calls, kSummaryMs / 1000.0, viewports.size(), frames, Ids(options), Ids(backBuffers),
                  off == 0 ? std::wstring(L"none") : off == kEveryViewport ? std::wstring(L"all") : std::to_wstring(off),
                  TagsNowText(fg::GetTagsNow()));
    for (const auto& [id, seen] : viewports)
    {
        std::wstring line = std::format(
            L"  viewport {}: options {} (mode {}{}, frames {}, flags {:#x}, dynamic {}x{}{}, result {}); constants {} "
            L"(frames {}..{}{}, repeats {}, skipped {}, resets {}, result {}; camera at ({:.0f}, {:.0f}, {:.0f}) facing "
            L"({:.2f}, {:.2f}, {:.2f}), FOV {:.1f} deg, aspect {:.3f}, jitter ({:.3f}, {:.3f})); frees {}",
            id, seen.options, seen.modeIn,
            seen.modeOut != seen.modeIn ? std::format(L" sent as {}", seen.modeOut) : std::wstring{}, seen.frames,
            seen.flags, seen.dynamicW, seen.dynamicH,
            seen.dynamicChanges != 0 ? std::format(L", changed {} time(s)", seen.dynamicChanges) : std::wstring{},
            seen.optionsResult, seen.constants, seen.firstFrame, seen.lastFrame,
            seen.unread != 0 ? std::format(L", {} with no readable frame", seen.unread) : std::wstring{}, seen.repeats,
            seen.skipped,
            seen.resets, seen.constantsResult, seen.pos[0], seen.pos[1], seen.pos[2], seen.fwd[0], seen.fwd[1],
            seen.fwd[2], seen.fov * kRadToDeg, seen.aspect, seen.jitter[0], seen.jitter[1], seen.frees);
        for (std::size_t k = 0; k < kKinds.size(); ++k)
        {
            const TagSeen& t = seen.tags[k];
            if (!t.seen)
            {
                continue;
            }
            line += std::format(L"; {} x{} {} {}x{} at ({},{}) life {}{}{} result {}", kKinds[k].name, t.calls,
                                Hex(t.native), t.extent[2], t.extent[3], t.extent[1], t.extent[0], t.lifeIn,
                                t.lifeOut != t.lifeIn ? std::format(L" sent as {}", t.lifeOut) : std::wstring{},
                                t.command ? L"" : L" (no command list)", t.result);
        }
        platform::Log(L"{}", line);
    }
    std::wstring sharedText;
    for (std::size_t k = 0; k < kKinds.size(); ++k)
    {
        if (shared[k] != 0)
        {
            sharedText += std::format(L"{}{} in {} frame(s)", sharedText.empty() ? L"" : L"; ", kKinds[k].name, shared[k]);
        }
    }
    platform::Log(L"  one resource for two viewports in the same frame, valid until present: {}",
                  sharedText.empty() ? std::wstring(L"none") : sharedText);
}

void RemoveWrappers() noexcept
{
    if (!g_installed.load())
    {
        return;
    }
    fg::SetTagObserver(nullptr);
    // In reverse; an entry another tool changed after us stays ours (the DLL is pinned, the wrapper forwards).
    const bool options   = g_setOptions.Restore();
    const bool frees     = g_free.Restore();
    const bool constants = g_setConstants.Restore();
    g_installed.store(false);
    platform::Log(L"frame generation watch: off ({})", options && frees && constants
                                                           ? L"the game's pointers back"
                                                           : L"a pointer changed after us stays wrapped; it forwards");
}

}  // namespace

void Tick() noexcept
{
    if (!g_installed.load())
    {
        return;
    }
    std::uint8_t enabled = 0;
    std::uint8_t active  = 0;
    if (g_enabledAt != 0 && g_activeAt != 0 && platform::Read(g_enabledAt, enabled) &&
        platform::Read(g_activeAt, active) && (enabled != g_lastEnabled || active != g_lastActive))
    {
        g_lastEnabled = enabled;
        g_lastActive  = active;
        platform::Log(L"frame generation watch: frame generation {}, {} this frame",
                      enabled ? L"initialised" : L"NOT initialised", active ? L"active" : L"not active");
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextSummary)
    {
        return;
    }
    g_nextSummary = now + kSummaryMs;
    try
    {
        LogSummary();
    }
    catch (...)
    {
    }
}

void Remove() noexcept
{
    RemoveWrappers();
}

void RegisterCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"fg-watch",
                       .shape = control::Shape::OnOff,
                       .help  = L"diagnosis: wrap the game's Streamline calls (DLSS-G) and log them per viewport "
                                L"every 5 s (off at start)",
                       .set   = [](const Value& v) {
                           if (!std::get<bool>(v))
                           {
                               RemoveWrappers();
                               return Outcome{true, L"frame generation watch off"};
                           }
                           std::wstring why;
                           if (!Install(why))
                           {
                               platform::Log(L"frame generation watch: not placed: {}", why);
                               return Outcome{false, why};
                           }
                           return Outcome{true, L"frame generation watch on"};
                       },
                       .get = [] { return Value{g_installed.load()}; }});
    control::Register({.name  = L"fg-off-viewport",
                       .shape = control::Shape::Integer,
                       .min   = -1,
                       .max   = 0x7FFFFFFF,
                       .help  = L"test: send this Streamline viewport's DLSS-G options with mode off (0: none; -1: "
                                L"every viewport; the ids are in the fg-watch summary)",
                       .set   = [](const Value& v) {
                           const auto id = static_cast<std::uint32_t>(std::get<std::int64_t>(v));
                           g_offViewport.store(id);
                           platform::Log(L"frame generation watch: {}",
                                         id == 0                ? std::wstring(L"every viewport's options sent as the game makes them")
                                         : id == kEveryViewport ? std::wstring(L"every viewport's options sent with mode off")
                                                                : std::format(L"viewport {}'s options sent with mode off", id));
                           return Outcome{};
                       },
                       .get = [] {
                           const std::uint32_t id = g_offViewport.load();
                           return Value{id == kEveryViewport ? std::int64_t{-1} : static_cast<std::int64_t>(id)};
                       }});
    control::Register({.name  = L"fg-tags-now",
                       .shape = control::Shape::Word,
                       .words = {L"off", L"depth", L"all"},
                       .help  = L"test: which tags go to Streamline as only valid now while views are split: off (the "
                                L"game's), depth (the mod's rule, the default), all (depth, motion vectors, HUD-less "
                                L"colour and UI)",
                       .set   = [](const Value& v) {
                           const auto&       word = std::get<std::wstring>(v);
                           const fg::TagsNow now  = word == L"depth" ? fg::TagsNow::Depth
                                                    : word == L"all" ? fg::TagsNow::All
                                                                     : fg::TagsNow::Off;
                           fg::SetTagsNow(now);
                           platform::Log(L"frame generation: tags copied at tag time while split: {} (the console)",
                                         now == fg::TagsNow::Off     ? L"none (the game's lifecycles)"
                                         : now == fg::TagsNow::Depth ? L"depth (the mod's rule)"
                                                                     : L"depth, motion vectors, HUD-less colour and UI");
                           return Outcome{};
                       },
                       .get = [] { return Value{std::wstring(TagsNowText(fg::GetTagsNow()))}; }});
}

}  // namespace lss::renderer::framegen_watch
