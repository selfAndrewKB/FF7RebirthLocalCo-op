#include "renderer/frame_generation.h"

#include "facts/streamline.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/instruction_patch.h"
#include "platform/log.h"
#include "platform/module_folder.h"
#include "platform/vtable_entry.h"
#include "views/player_view_point.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace lss::renderer::frame_generation {

namespace {

constexpr std::size_t kMaxTags = 8;  // the game tags one at a time

using SetTagFn = std::int32_t (*)(const void*, const void*, std::uint32_t, void*);

platform::InstructionPatch g_constantsReset;
platform::InstructionPatch g_velocityReset;
platform::VTableEntry      g_setTag;

std::atomic<bool>        g_placed{false};
std::atomic<TagsNow>     g_tagsNow{TagsNow::Depth};
std::atomic<TagObserver> g_observer{nullptr};

// Game thread: the tag wrapper's pending placement.
std::uintptr_t g_tagGlobal  = 0;                     // the live address of the game's slSetTag pointer
std::uintptr_t g_lastSeen   = ~std::uintptr_t{0};    // the pointer value last looked at (a lookup only on a change)
bool           g_waitLogged = false;
bool           g_wrapperOff = false;  // installing it failed for good, or removed: logged, never retried

// Any thread: the viewports whose depth was copied (each one's first copy logged), and the tags that came without a
// command list (the first at Warning, each at Debug with its count).
std::mutex                 g_seenLock;
std::vector<std::uint32_t> g_seenViewports;
std::atomic<std::uint64_t> g_noCommand{0};

template <class T>
T At(const std::uint8_t* bytes, std::size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

bool CopiedNow(std::uint32_t type, TagsNow tags) noexcept
{
    // The frame inputs DLSS-G reads at present.
    const bool input = type == facts::kSlDepth || type == facts::kSlMotionVectors || type == facts::kSlHudLessColor ||
                       type == facts::kSlUiColorAlpha;
    return input && (tags == TagsNow::All || (tags == TagsNow::Depth && type == facts::kSlDepth));
}

/// The first time a viewport's depth is copied (a view comes with a join, a mirror or a world change): Info.
void NoteFirstCopy(std::uint32_t viewport) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(g_seenLock);
        if (std::find(g_seenViewports.begin(), g_seenViewports.end(), viewport) != g_seenViewports.end())
        {
            return;
        }
        g_seenViewports.push_back(viewport);
    }
    catch (...)
    {
        return;
    }
    platform::Log(L"frame generation: viewport {}'s depth copied at tag time (Streamline's own copy), so its generated "
                  L"frames read its own depth",
                  viewport);
}

std::int32_t SetTag(const void* viewport, const void* tags, std::uint32_t count, void* command)
{
    const TagsNow                                         now = g_tagsNow.load(std::memory_order_relaxed);
    std::array<std::uint8_t, facts::kSlTagSize * kMaxTags> copy{};
    std::array<std::uint32_t, kMaxTags>                   lifeIn{};
    const bool copied = tags != nullptr && count > 0 && count <= kMaxTags &&
                        platform::CopyGuarded(copy.data(), tags, facts::kSlTagSize * count);
    const void* forward  = tags;
    bool        depthNow = false;
    if (copied)
    {
        const bool split   = now != TagsNow::Off && views::ViewsSplit();
        bool       changed = false;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint8_t*       tag  = copy.data() + i * facts::kSlTagSize;
            const std::uint32_t type = At<std::uint32_t>(tag, facts::kSlTagType);
            lifeIn[i]                = At<std::uint32_t>(tag, facts::kSlTagLifecycle);
            if (!split || lifeIn[i] == facts::kSlOnlyValidNow || !CopiedNow(type, now))
            {
                continue;
            }
            // Streamline records the copy on the command list the tag comes with; without one it cannot copy.
            if (command == nullptr)
            {
                const std::uint64_t times = g_noCommand.fetch_add(1) + 1;
                platform::LogAt(times == 1 ? platform::Level::Warning : platform::Level::Debug,
                                L"frame generation: a tag of type {} came without a command list; sent as the game asked "
                                L"(valid until present; {} time(s))",
                                type, times);
                continue;
            }
            std::memcpy(tag + facts::kSlTagLifecycle, &facts::kSlOnlyValidNow, sizeof(facts::kSlOnlyValidNow));
            changed  = true;
            depthNow = depthNow || type == facts::kSlDepth;
        }
        forward = changed ? copy.data() : tags;
    }
    const std::int32_t result = reinterpret_cast<SetTagFn>(g_setTag.Original())(viewport, forward, count, command);
    if (depthNow && result == 0)  // sl::Result::eOk
    {
        std::uint32_t id = 0xFFFFFFFF;
        platform::Read(viewport, facts::kSlViewportId, id);
        NoteFirstCopy(id);
    }
    if (TagObserver observer = g_observer.load(std::memory_order_acquire); observer != nullptr && copied)
    {
        observer({viewport, copy.data(), lifeIn.data(), count, command, result});  // the copy holds what was sent
    }
    return result;
}

/// One reset gate: the compare and its jump checked whole, then the compare replaced.
bool PlaceGate(platform::InstructionPatch& patch, const platform::GameImage& image, std::uintptr_t ida,
               std::span<const std::uint8_t> whole) noexcept
{
    const auto site = image.Rebase(ida, whole.size());
    std::array<std::uint8_t, 8> live{};
    if (!site || whole.size() > live.size() || !platform::CopyGuarded(live.data(), reinterpret_cast<const void*>(*site), whole.size()) ||
        std::memcmp(live.data(), whole.data(), whole.size()) != 0)
    {
        return false;
    }
    return patch.Apply(*site, whole.first(facts::kFgResetGateCompareSize), facts::kFgResetGateAlwaysEqual);
}

void InstallResetGates(const platform::GameImage& image) noexcept
{
    const bool constants = PlaceGate(g_constantsReset, image, facts::kFgConstantsResetGate, facts::kFgConstantsResetBytes);
    const bool velocity  = constants && PlaceGate(g_velocityReset, image, facts::kFgVelocityResetGate, facts::kFgVelocityResetBytes);
    if (constants && velocity)
    {
        platform::Log(L"frame generation: each view resets its generated frames on its own camera cut (the two "
                      L"first-family gates opened)");
        return;
    }
    // Both or neither: a half reset would leave the velocity history unreset.
    const bool undone = !constants || g_constantsReset.Restore();
    platform::LogWarning(L"frame generation: the reset gates are NOT opened (the game's code is not the expected "
                         L"instructions){}; players 2 and 3's generated frames do not reset on their camera cuts",
                         undone ? L"" : L", and the first gate could not be put back: it stays open");
}

/// Places the tag wrapper if the game's pointer now leads into Streamline. Game thread.
void TryPlaceWrapper() noexcept
{
    std::uintptr_t held = 0;
    if (g_tagGlobal == 0 || g_wrapperOff || !platform::Read(g_tagGlobal, held) || held == g_lastSeen)
    {
        return;
    }
    g_lastSeen               = held;
    const std::wstring owner = held != 0 ? platform::ModuleNameAt(held) : std::wstring{};
    if (owner.rfind(L"sl.", 0) != 0)
    {
        if (held != 0)
        {
            g_wrapperOff = true;
            platform::LogWarning(L"frame generation: slSetTag points into '{}', not Streamline (another tool?); the "
                                 L"depth rule stands aside: with frame generation on, every view reads the last view's depth",
                                 owner);
        }
        else if (!g_waitLogged)
        {
            g_waitLogged = true;
            platform::Log(L"frame generation: Streamline is not loaded yet; the depth rule waits for it");
        }
        return;
    }
    if (!g_setTag.Replace(g_tagGlobal, held, reinterpret_cast<std::uintptr_t>(&SetTag)))
    {
        g_wrapperOff = true;
        platform::LogWarning(L"frame generation: slSetTag's pointer could not be swapped; the depth rule stands aside");
        return;
    }
    g_placed.store(true);
    platform::Log(L"frame generation: while views are split, each view's depth goes to Streamline as only valid now "
                  L"(slSetTag in {} wrapped), so every view's generated frames read its own depth",
                  owner);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    InstallResetGates(image);
    g_tagGlobal = image.Rebase(facts::kSlSetTag, sizeof(void*)).value_or(0);
    if (g_tagGlobal == 0)
    {
        platform::LogWarning(L"frame generation: the game's slSetTag pointer is outside the image; the depth rule stands aside");
        return;
    }
    TryPlaceWrapper();
}

void Poll() noexcept
{
    if (!g_placed.load(std::memory_order_relaxed))
    {
        TryPlaceWrapper();
    }
}

void Remove() noexcept
{
    g_wrapperOff = true;  // no Poll places it again
    if (g_placed.load() && !g_setTag.Restore())
    {
        platform::Log(L"frame generation: slSetTag's pointer changed after us; left wrapped (it forwards)");
    }
    g_placed.store(false);
    const bool velocity  = g_velocityReset.Restore();
    const bool constants = g_constantsReset.Restore();
    if (!velocity || !constants)
    {
        platform::Log(L"frame generation: a reset gate changed after us; left as it is");
    }
}

void SetTagsNow(TagsNow tags) noexcept
{
    g_tagsNow.store(tags);
}

TagsNow GetTagsNow() noexcept
{
    return g_tagsNow.load();
}

bool TagWrapperPlaced() noexcept
{
    return g_placed.load();
}

void SetTagObserver(TagObserver observer) noexcept
{
    g_observer.store(observer, std::memory_order_release);
}

}  // namespace lss::renderer::frame_generation
