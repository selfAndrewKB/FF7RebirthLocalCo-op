#include "renderer/framegen_watch.h"

#include "platform/command_registry.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/vtable_entry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lss::renderer::framegen_watch {

namespace {

// The game's Streamline function pointers (research/RE-p2-framegen-2.md). Each global holds a pointer into
// sl.interposer.dll that the game loads at the call (`call qword ptr [rip+g]`, `mov rax, [rip+g]`), never caching it,
// filled once when the game creates its D3D12 device (SL_LoadInterposerProcs, SL_InitD3D12DeviceAndFeatures).
constexpr std::uintptr_t kSlSetTag          = 0x1491F72C8;  // slSetTag(viewport, tags, count, command list)
constexpr std::uintptr_t kSlSetConstants    = 0x1491F72D8;  // slSetConstants(constants, frame token, viewport)
constexpr std::uintptr_t kSlFreeResources   = 0x1491F72C0;  // slFreeResources(feature, viewport)
constexpr std::uintptr_t kSlDLSSGSetOptions = 0x1491F7350;  // slDLSSGSetOptions(viewport, options)
// The frame generation method's state bytes (the interface's IsEnabled and SetActiveThisFrame slots read and write them).
constexpr std::uintptr_t kFgEnabled         = 0x146EF8A28;  // u8: DLSS-G initialised
constexpr std::uintptr_t kFgActiveThisFrame = 0x146EF8A2A;  // u8: r.FrameGeneration.Method != 0 at the last draw

// Streamline's structs, as the game builds them (research/RE-p2-framegen.md; Streamline's sl.h and sl_dlss_g.h).
constexpr std::size_t   kViewportId     = 0x20;  // u32 in sl::ViewportHandle
constexpr std::size_t   kTagSize        = 0x40;  // sl::ResourceTag
constexpr std::size_t   kTagResource    = 0x20;  // sl::Resource*
constexpr std::size_t   kTagType        = 0x28;  // u32 sl::BufferType
constexpr std::size_t   kTagLifecycle   = 0x2C;  // u32 sl::ResourceLifecycle
constexpr std::size_t   kTagExtent      = 0x30;  // u32 top, left, width, height
constexpr std::size_t   kResourceNative = 0x28;  // ID3D12Resource* in sl::Resource
constexpr std::size_t   kOptionsSize    = 0x70;  // sl::DLSSGOptions
constexpr std::size_t   kOptionsMode    = 0x20;  // u32 sl::DLSSGMode
constexpr std::size_t   kOptionsFrames  = 0x24;  // u32 numFramesToGenerate
constexpr std::size_t   kOptionsFlags   = 0x28;  // u32 sl::DLSSGFlags
constexpr std::size_t   kOptionsDynamic = 0x2C;  // u32 dynamic resolution width, height
constexpr std::uint32_t kOnlyValidNow   = 0;     // sl::ResourceLifecycle::eOnlyValidNow
constexpr std::uint32_t kModeOff        = 0;     // sl::DLSSGMode::eOff
constexpr std::size_t   kMaxTags        = 8;     // the game tags one at a time
constexpr std::uint32_t kEveryViewport  = 0xFFFFFFFF;  // `fg-off-viewport -1`: every viewport's options sent off

// The buffer types the game tags (FG_MapTagToSLBufferType), in the summary's order.
struct BufferKind
{
    std::uint32_t  type;
    const wchar_t* name;
    bool           input;  // a frame input `fg-tags-now all` copies (the back buffer is not)
};
constexpr std::array<BufferKind, 5> kKinds{{{0, L"depth", true},
                                            {1, L"motion vectors", true},
                                            {2, L"HUD-less colour", true},
                                            {23, L"UI", true},
                                            {53, L"back buffer", false}}};
constexpr std::size_t kBackBufferKind = 4;

enum class TagsNow : std::uint8_t
{
    Off,
    Depth,
    All,
};

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
    std::uint64_t                      options         = 0;  // calls this window
    std::uint64_t                      constants       = 0;
    std::uint64_t                      frees           = 0;
    std::uint32_t                      modeIn          = 0;
    std::uint32_t                      modeOut         = 0;
    std::uint32_t                      frames          = 0;
    std::uint32_t                      flags           = 0;
    std::uint32_t                      dynamicW        = 0;
    std::uint32_t                      dynamicH        = 0;
    std::int32_t                       optionsResult   = 0;
    std::int32_t                       constantsResult = 0;
    std::array<TagSeen, kKinds.size()> tags{};
};

using SetTagFn       = std::int32_t (*)(const void*, const void*, std::uint32_t, void*);
using SetConstantsFn = std::int32_t (*)(const void*, const void*, const void*);
using FreeFn         = std::int32_t (*)(std::uint32_t, const void*);
using SetOptionsFn   = std::int32_t (*)(const void*, const void*);

platform::VTableEntry g_setTag;
platform::VTableEntry g_setConstants;
platform::VTableEntry g_free;
platform::VTableEntry g_setOptions;

std::atomic<bool>          g_installed{false};
std::atomic<std::uint32_t> g_offViewport{0};
std::atomic<TagsNow>       g_tagsNow{TagsNow::Off};

// Any thread (the wrappers run on the render and RHI threads); the summary takes it on the game thread.
std::mutex                            g_lock;
std::map<std::uint32_t, ViewportSeen> g_viewports;
std::vector<std::uint32_t>            g_backBufferOrder;  // this window's back-buffer tags, by viewport, the first few
std::vector<std::uint32_t>            g_optionsOrder;     // this window's options calls, by viewport, the first few
std::uint64_t                         g_windowCalls = 0;

// Game thread.
std::uintptr_t        g_enabledAt   = 0;
std::uintptr_t        g_activeAt    = 0;
int                   g_lastEnabled = -1;
int                   g_lastActive  = -1;
ULONGLONG             g_nextSummary = 0;
constexpr ULONGLONG   kSummaryMs    = 5000;
constexpr std::size_t kOrderKept    = 16;

std::uint32_t ViewportOf(const void* handle) noexcept
{
    std::uint32_t id = 0xFFFFFFFF;
    platform::Read(handle, kViewportId, id);
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

bool CopiedNow(std::uint32_t type, TagsNow now) noexcept
{
    const int kind = KindIndex(type);
    return kind >= 0 && kKinds[kind].input && (now == TagsNow::All || (now == TagsNow::Depth && kind == 0));
}

void NoteOrder(std::vector<std::uint32_t>& order, std::uint32_t viewport)
{
    if (order.size() < kOrderKept)
    {
        order.push_back(viewport);
    }
}

std::int32_t SetTag(const void* viewport, const void* tags, std::uint32_t count, void* command)
{
    const TagsNow                                 now = g_tagsNow.load(std::memory_order_relaxed);
    std::array<std::uint8_t, kTagSize * kMaxTags> copy{};
    const bool copied = tags != nullptr && count > 0 && count <= kMaxTags &&
                        platform::CopyGuarded(copy.data(), tags, kTagSize * count);
    std::array<std::uint32_t, kMaxTags> lifeIn{};
    const void*                         forward = tags;
    if (copied)
    {
        bool changed = false;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint8_t* tag = copy.data() + i * kTagSize;
            lifeIn[i]         = At<std::uint32_t>(tag, kTagLifecycle);
            // Copying at tag time needs the command list Streamline records the copy on.
            if (command != nullptr && lifeIn[i] != kOnlyValidNow && CopiedNow(At<std::uint32_t>(tag, kTagType), now))
            {
                std::memcpy(tag + kTagLifecycle, &kOnlyValidNow, sizeof(kOnlyValidNow));
                changed = true;
            }
        }
        forward = changed ? copy.data() : tags;
    }
    const std::int32_t result = reinterpret_cast<SetTagFn>(g_setTag.Original())(viewport, forward, count, command);
    if (!copied)
    {
        return result;
    }
    try
    {
        const std::uint32_t         id = ViewportOf(viewport);
        std::lock_guard<std::mutex> lock(g_lock);
        ViewportSeen&               seen = g_viewports[id];
        ++g_windowCalls;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uint8_t* tag  = copy.data() + i * kTagSize;
            const int           kind = KindIndex(At<std::uint32_t>(tag, kTagType));
            if (kind < 0)
            {
                continue;
            }
            TagSeen&             t        = seen.tags[kind];
            const auto           resource = At<std::uintptr_t>(tag, kTagResource);
            std::uintptr_t       native   = 0;
            if (resource != 0)
            {
                platform::Read(reinterpret_cast<const void*>(resource), kResourceNative, native);
            }
            ++t.calls;
            t.seen    = true;
            t.native  = native;
            t.extent  = At<std::array<std::uint32_t, 4>>(tag, kTagExtent);
            t.lifeIn  = lifeIn[i];
            t.lifeOut = At<std::uint32_t>(tag, kTagLifecycle);
            t.command = command != nullptr;
            t.result  = result;
            if (kind == static_cast<int>(kBackBufferKind))
            {
                NoteOrder(g_backBufferOrder, id);
            }
        }
    }
    catch (...)
    {
    }
    return result;
}

std::int32_t SetConstants(const void* constants, const void* frame, const void* viewport)
{
    const std::int32_t result = reinterpret_cast<SetConstantsFn>(g_setConstants.Original())(constants, frame, viewport);
    try
    {
        const std::uint32_t         id = ViewportOf(viewport);
        std::lock_guard<std::mutex> lock(g_lock);
        ViewportSeen&               seen = g_viewports[id];
        ++seen.constants;
        seen.constantsResult = result;
        ++g_windowCalls;
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
    const std::uint32_t                    id = ViewportOf(viewport);
    std::array<std::uint8_t, kOptionsSize> copy{};
    const bool  copied  = options != nullptr && platform::CopyGuarded(copy.data(), options, kOptionsSize);
    const auto  modeIn  = copied ? At<std::uint32_t>(copy.data(), kOptionsMode) : 0u;
    const void* forward = options;
    const std::uint32_t off = g_offViewport.load(std::memory_order_relaxed);
    if (copied && off != 0 && (id == off || off == kEveryViewport) && modeIn != kModeOff)
    {
        std::memcpy(copy.data() + kOptionsMode, &kModeOff, sizeof(kModeOff));
        forward = copy.data();
    }
    const std::int32_t result = reinterpret_cast<SetOptionsFn>(g_setOptions.Original())(viewport, forward);
    if (!copied)
    {
        return result;
    }
    try
    {
        std::lock_guard<std::mutex> lock(g_lock);
        ViewportSeen&               seen = g_viewports[id];
        ++seen.options;
        ++g_windowCalls;
        seen.modeIn        = modeIn;
        seen.modeOut       = At<std::uint32_t>(copy.data(), kOptionsMode);
        seen.frames        = At<std::uint32_t>(copy.data(), kOptionsFrames);
        seen.flags         = At<std::uint32_t>(copy.data(), kOptionsFlags);
        seen.dynamicW      = At<std::uint32_t>(copy.data(), kOptionsDynamic);
        seen.dynamicH      = At<std::uint32_t>(copy.data(), kOptionsDynamic + sizeof(std::uint32_t));
        seen.optionsResult = result;
        NoteOrder(g_optionsOrder, id);
    }
    catch (...)
    {
    }
    return result;
}

/// The file name of the module holding `address`, or empty.
std::wstring ModuleOf(std::uintptr_t address) noexcept
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
    {
        return {};
    }
    wchar_t     path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    try
    {
        std::wstring name(path, length);
        const auto   slash = name.find_last_of(L"\\/");
        return slash == std::wstring::npos ? name : name.substr(slash + 1);
    }
    catch (...)
    {
        return {};
    }
}

struct Slot
{
    const wchar_t*         name;
    std::uintptr_t         ida;
    platform::VTableEntry* entry;
    std::uintptr_t         replacement;
};

/// Places the four wrappers, all or none; `why` says what refused. Game thread.
bool Install(std::wstring& why)
{
    if (g_installed.load())
    {
        return true;
    }
    const auto image = platform::GameImage::Locate();
    if (!image)
    {
        why = L"the game's image was not found";
        return false;
    }
    const std::array<Slot, 4> slots{
        {{L"slSetTag", kSlSetTag, &g_setTag, reinterpret_cast<std::uintptr_t>(&SetTag)},
         {L"slSetConstants", kSlSetConstants, &g_setConstants, reinterpret_cast<std::uintptr_t>(&SetConstants)},
         {L"slFreeResources", kSlFreeResources, &g_free, reinterpret_cast<std::uintptr_t>(&FreeResources)},
         {L"slDLSSGSetOptions", kSlDLSSGSetOptions, &g_setOptions, reinterpret_cast<std::uintptr_t>(&SetOptions)}}};
    std::array<std::uintptr_t, 4> at{};
    std::array<std::uintptr_t, 4> held{};
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
        const std::wstring owner = held[i] != 0 ? ModuleOf(held[i]) : std::wstring{};
        if (owner.rfind(L"sl.", 0) != 0)
        {
            why = held[i] == 0 ? std::format(L"{} is not loaded (the game loads Streamline with its D3D12 device, and "
                                             L"DLSS-G's functions only where frame generation can run)",
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
    g_enabledAt = image->Rebase(kFgEnabled).value_or(0);
    g_activeAt  = image->Rebase(kFgActiveThisFrame).value_or(0);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_viewports.clear();
        g_backBufferOrder.clear();
        g_optionsOrder.clear();
        g_windowCalls = 0;
    }
    g_lastEnabled = g_lastActive = -1;
    g_nextSummary = GetTickCount64() + kSummaryMs;
    g_installed.store(true);
    platform::Log(L"frame generation watch: on ({}); every call forwarded as the game makes it", found);
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

const wchar_t* TagsNowText(TagsNow now) noexcept
{
    return now == TagsNow::Off ? L"off" : now == TagsNow::Depth ? L"depth" : L"all";
}

void LogSummary()
{
    std::map<std::uint32_t, ViewportSeen> viewports;
    std::vector<std::uint32_t>            backBuffers;
    std::vector<std::uint32_t>            options;
    std::uint64_t                         calls = 0;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        viewports   = g_viewports;
        backBuffers = std::move(g_backBufferOrder);
        options     = std::move(g_optionsOrder);
        calls       = g_windowCalls;
        g_backBufferOrder.clear();
        g_optionsOrder.clear();
        g_windowCalls = 0;
        for (auto& [id, seen] : g_viewports)
        {
            seen.options = seen.constants = seen.frees = 0;
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
    platform::Log(L"frame generation watch: {} call(s) in {:.0f} s over {} viewport(s); options order {}; back-buffer "
                  L"tag order {}; switches: viewport off {}, tags copied at tag time {}",
                  calls, kSummaryMs / 1000.0, viewports.size(), Ids(options), Ids(backBuffers),
                  off == 0 ? std::wstring(L"none") : off == kEveryViewport ? std::wstring(L"all") : std::to_wstring(off),
                  TagsNowText(g_tagsNow.load()));
    for (const auto& [id, seen] : viewports)
    {
        std::wstring line = std::format(
            L"  viewport {}: options {} (mode {}{}, frames {}, flags {:#x}, dynamic {}x{}, result {}), constants {} "
            L"(result {}), frees {}",
            id, seen.options, seen.modeIn,
            seen.modeOut != seen.modeIn ? std::format(L" sent as {}", seen.modeOut) : std::wstring{}, seen.frames,
            seen.flags, seen.dynamicW, seen.dynamicH, seen.optionsResult, seen.constants, seen.constantsResult,
            seen.frees);
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
    // The same D3D12 resource tagged as one kind for two viewports: by present time it holds the last one's.
    std::wstring shared;
    for (std::size_t k = 0; k < kKinds.size(); ++k)
    {
        std::map<std::uintptr_t, std::vector<std::uint32_t>> owners;
        for (const auto& [id, seen] : viewports)
        {
            if (seen.tags[k].seen && seen.tags[k].native != 0)
            {
                owners[seen.tags[k].native].push_back(id);
            }
        }
        for (const auto& [native, ids] : owners)
        {
            if (ids.size() > 1)
            {
                shared += std::format(L"{}{} {} for viewports {}", shared.empty() ? L"" : L"; ", kKinds[k].name,
                                      Hex(native), Ids(ids));
            }
        }
    }
    platform::Log(L"  shared resources: {}", shared.empty() ? std::wstring(L"none") : shared);
}

void RemoveWrappers() noexcept
{
    if (!g_installed.load())
    {
        return;
    }
    // In reverse; an entry another tool changed after us stays ours (the DLL is pinned, the wrapper forwards).
    const bool options   = g_setOptions.Restore();
    const bool frees     = g_free.Restore();
    const bool constants = g_setConstants.Restore();
    const bool tags      = g_setTag.Restore();
    g_installed.store(false);
    platform::Log(L"frame generation watch: off ({})", options && frees && constants && tags
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
                       .help  = L"test: tag depth (all: depth, motion vectors, HUD-less colour and UI) as only valid "
                                L"now, so Streamline copies it at tag time",
                       .set   = [](const Value& v) {
                           const auto&   word = std::get<std::wstring>(v);
                           const TagsNow now  = word == L"depth" ? TagsNow::Depth
                                                : word == L"all" ? TagsNow::All
                                                                 : TagsNow::Off;
                           g_tagsNow.store(now);
                           platform::Log(L"frame generation watch: tags copied at tag time: {}",
                                         now == TagsNow::Off     ? L"none (the game's lifecycles)"
                                         : now == TagsNow::Depth ? L"depth"
                                                                 : L"depth, motion vectors, HUD-less colour and UI");
                           return Outcome{};
                       },
                       .get = [] { return Value{std::wstring(TagsNowText(g_tagsNow.load()))}; }});
}

}  // namespace lss::renderer::framegen_watch
