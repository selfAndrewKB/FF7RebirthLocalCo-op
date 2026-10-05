#include "hud/minimap.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

#include "hud/entry_filter_code.h"
#include "hud/hud_addresses.h"
#include "hud/label_relayouts.h"
#include "hud/minimap_icons.h"
#include "hud/menu_buffers.h"
#include "hud/minimap_labels.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "player_two/generation.h"
#include "player_two/party_member.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace lss::hud::minimap {

namespace {

namespace generation   = player_two::generation;
namespace party_member = player_two::party_member;

// RE-p2-own-minimap.md §1, §3 and §8 (this build). Every function's entry bytes are checked before use.

// The widget, through the navimap holder: g_EndUISystem (the object itself) +0x108 area-map manager, +0x340 holder,
// +0x20 the widget's native interface (UEndNaviMapCircle +0x108), the Slate widget at +0x138 (a TSharedPtr, object
// first), its paint interface (+0x2B0, the vtable says circle), its content SEndNaviMap (+0x3B0) and target (+0x490).
constexpr std::uintptr_t kEndUISystem         = 0x1493496E0;
constexpr std::uint32_t  kAreaMapManager      = 0x108;
constexpr std::uint32_t  kNaviMapHolder       = 0x340;
constexpr std::uint32_t  kHolderInterface     = 0x20;
constexpr std::uint32_t  kInterfaceInObject   = 0x108;
constexpr std::uint32_t  kObjectSlateWidget   = 0x138;
constexpr std::uint32_t  kSlatePaintInterface = 0x2B0;
constexpr std::uint32_t  kSlateContent        = 0x3B0;
constexpr std::uint32_t  kSlateTarget         = 0x490;
constexpr std::uint32_t  kContentCentre       = 0x478;  // float x, y, z
constexpr std::uintptr_t kCirclePaintVTable   = 0x14676E5D8;

// MasterIconPaint_NaviMapHolder_FrameUpdate(holder, dt): the content's only writer (centre, rotations, icons).
constexpr std::uintptr_t kHolderUpdate        = 0x140843F10;
constexpr std::uint8_t   kHolderUpdateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54,
                                                 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x90, 0x03, 0x00, 0x00, 0xE8};
// Its one leader read: call UEndPartyAPI_GetPartyLeaderType (u8 in al) at 0x140844016; location, range, arrow yaw
// and the party-icon skip all follow that answer (no sub-function reads the leader itself, §8.5).
constexpr std::uintptr_t kLeaderTypeCall    = 0x140844016;
constexpr std::uintptr_t kLeaderType        = 0x1408070C4;
constexpr std::uint8_t   kLeaderTypeBytes[] = {0x48, 0x8B, 0x05, 0x15, 0x4C, 0xB9, 0x08, 0x48, 0x85, 0xC0, 0x74,
                                               0x07, 0x8A, 0x80, 0x50, 0x01, 0x00, 0x00, 0xC3, 0xB0, 0x10, 0xC3};

// The holder update's map pieces (RE-p2-minimap-tiles.md §2, §4): a piece in state 2 (shown) is dropped when the run's
// centre is out of its rectangle + margin + 5 m, asked at 0x140844905 (`lea rdx,[rsp+90h]` = the centre; `mov rcx,rbx`
// = the piece; only al is read after). The pieces' states are shared, so with one centre per run each player's run
// dropped the other's pieces (CONFIRMED live: ~17 load/drop cycles a second, the flicker).
constexpr std::uintptr_t kPieceUnloadCall      = 0x140844905;
constexpr std::uintptr_t kPieceShouldUnload    = 0x141513F24;  // NaviMapPiece__ShouldUnload_CentreOutOfRectPlus500
constexpr std::uintptr_t kPieceUnloadArgs      = 0x1408448FA;
constexpr std::uint8_t   kPieceUnloadArgsBytes[] = {0x48, 0x8D, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB};
constexpr std::uintptr_t kPieceUnloadAfter     = 0x14084490A;
constexpr std::uint8_t   kPieceUnloadAfterBytes[] = {0x84, 0xC0};

// SEndNaviMapCircle__OnPaint_Pool0's pool-0 block (§8.1): the acquire (rcx renderer, dl 0; its call crosses a
// 16-byte block, so it is installed from the game thread while that thread is outside the paint), FindOrAdd (rcx
// slot, rdx the FName value from the static below, r8 rect), and the submit (rcx renderer, dl 0, r8 slot).
constexpr std::uintptr_t kAcquireCall      = 0x140FC6CAE;
constexpr std::uintptr_t kAcquireLayerSlot = 0x140FC8694;
constexpr std::uint8_t   kAcquireLayerSlotPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kFindOrAddCall    = 0x140FC6CE9;
constexpr std::uintptr_t kSubmitCall       = 0x140FC6F13;
constexpr std::uintptr_t kStockNameStatic  = 0x14939C2F8;  // FName "SEndNaviMapCircle", built once by the game

// Pool 0's consume: the vertex-buffer load before DrawLayerEntry (r13 = renderer, r9 = entry).
constexpr std::uintptr_t kConsumeLoadSite   = 0x140FC7C91;
constexpr std::uintptr_t kConsumeLoadResume = 0x140FC7C98;

// Pool 0's buffers (renderer +0x80 vertex, +0x98 index): the same kind and capacities as every pool's (§8.3; uploads
// grow past them on the render thread).
constexpr std::uint32_t  kPoolZeroVertexBuffer = 0x80;
constexpr std::uint32_t  kPoolZeroIndexBuffer  = 0x98;
constexpr std::uintptr_t kVertexBufferVtable   = 0x146621CF8;
constexpr std::uintptr_t kIndexBufferVtable    = 0x146621D50;
constexpr std::uintptr_t kBeginInitResource    = 0x1409E7EF8;
constexpr std::uint8_t   kBeginInitResourcePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48,
                                                         0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0xB8, 0x90, 0x00, 0x00, 0x00};

// The widget's own target recipe (SEndNaviMapCircle__Construct_CreatesRT, §8.4): NewObject in GObjTransientPkg, SRGB
// (+0x91 bit 0), TargetGamma 1, ClearColor 0, InitCustomFormat(X, Y, PF_B8G8R8A8, linear), UpdateResourceImmediate.
constexpr std::uintptr_t kTransientPackage = 0x149115C28;
constexpr std::uintptr_t kNewRenderTarget  = 0x142E714EC;
constexpr std::uint8_t   kNewRenderTargetBytes[] = {0x40, 0x53, 0xB8, 0x60, 0x00, 0x00, 0x00, 0xE8,
                                                    0x38, 0xDA, 0x23, 0xFF, 0x48, 0x2B, 0xE0, 0x48};
constexpr std::uintptr_t kInitCustomFormat = 0x1451B2EA0;
constexpr std::uint8_t   kInitCustomFormatBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x86, 0xC0,
                                                     0xEF, 0xFC, 0x48, 0x2B, 0xE0, 0x8A, 0x44, 0x24};
constexpr std::uintptr_t kUpdateResourceImmediate = 0x1451B96CC;
constexpr std::uint8_t   kUpdateResourceImmediateBytes[] = {0xB8, 0x58, 0x00, 0x00, 0x00, 0xE8, 0x5A, 0x58,
                                                            0xEF, 0xFC, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x81};
constexpr std::uint32_t  kTargetFlags91    = 0x91;  // bit 0: SRGB
constexpr std::uint32_t  kTargetGamma      = 0xE0;
constexpr std::uint32_t  kTargetSizeX      = 0xE8;
constexpr std::uint32_t  kTargetSizeY      = 0xEC;
constexpr std::uint32_t  kTargetClearColor = 0xF0;  // FLinearColor
constexpr std::uint32_t  kTargetFormat     = 0x108;
constexpr std::uint32_t  kTargetResource   = 0xA8;
constexpr std::uint32_t  kResourceTexture  = 0x10;  // the texture RHI the HUD element samples
constexpr std::uint32_t  kResourceTarget   = 0x90;  // the render-target RHI the consume binds
constexpr std::uint8_t   kFormatB8G8R8A8   = 2;
constexpr std::uint32_t  kMaxTargetSize    = 4096;

constexpr const wchar_t* kStockEntryName     = L"SEndNaviMapCircle";
constexpr const wchar_t* kPlayerTwoEntryName = L"SEndNaviMapCircle_P2";
constexpr ULONGLONG      kCountsPeriodMs     = 120'000;  // a safety submit is logged at once
constexpr ULONGLONG      kCentrePeriodMs     = 120'000;
constexpr int            kMaxLines           = 300;

using HolderUpdateFn = void (*)(void* holder, float deltaTime);
using LeaderTypeFn   = std::uint8_t (*)();
using AcquireFn      = void* (*)(void* renderer, std::uintptr_t pool);
using FindOrAddFn    = void* (*)(void* slot, std::uintptr_t name, void* rect, std::uintptr_t unused);
using SubmitFn       = void (*)(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused);
using NewTargetFn    = void* (*)(void* outer);
using InitFormatFn   = void (*)(void* target, std::uint32_t sizeX, std::uint32_t sizeY, std::uint8_t format,
                              std::uint8_t forceLinearGamma);
using UpdateNowFn    = void (*)(void* target, std::uint8_t clear);
using ShouldUnloadFn = bool (*)(const void* piece, const float* centre);

enum class Pass
{
    None,
    PlayerTwo,
    PlayerOne,
};

struct Counts
{
    std::uint64_t held = 0, takenBack = 0, named = 0, submitSkipped = 0, safetySubmits = 0, runsTwo = 0, runsOne = 0;
};

// Installed once, then read.
platform::CallRedirect g_acquireCall;
platform::CallRedirect g_findCall;
platform::CallRedirect g_submitCall;
platform::CallRedirect g_leaderCall;
platform::CallRedirect g_unloadCall;  // optional: without it, the pieces thrash as before
platform::CodeRedirect g_consumeCode;
bool                   g_installed          = false;
std::uintptr_t         g_system             = 0;
std::uintptr_t         g_acquireSite        = 0;
std::uintptr_t         g_acquireTarget      = 0;
std::uintptr_t         g_stockNameStatic    = 0;
std::uintptr_t         g_transientPackage   = 0;
std::uintptr_t         g_circleVTable       = 0;
std::uintptr_t         g_vertexBufferVtable = 0;
std::uintptr_t         g_indexBufferVtable  = 0;
HolderUpdateFn         g_holderUpdate       = nullptr;
NewTargetFn            g_newTarget          = nullptr;
InitFormatFn           g_initFormat         = nullptr;
UpdateNowFn            g_updateNow          = nullptr;
BeginInitResourceFn    g_beginInit          = nullptr;

// Read by the render-thread consume code through their addresses (hud/entry_filter_code.h).
EntryFilterFields g_fields;
MenuBufferSet     g_buffers;  // player 2's (hud/menu_buffers.h)

// Game thread.
bool          g_acquireTried  = false;
bool          g_refused       = false;  // a check failed: the earlier behaviour until restart
bool          g_namesReady    = false;
std::uint64_t g_playerTwoName = 0;
void*         g_renderer      = nullptr;
bool          g_buffersQueued = false;
void*         g_target        = nullptr;  // player 2's render target (rooted, kept for the process)
bool          g_frameActive   = false;
bool          g_ranForTwo     = false;
int           g_answerType    = -1;  // the leader type the holder update gets in player 2's run
Pass          g_pass          = Pass::None;
void*         g_held          = nullptr;
void*         g_swappedSlate  = nullptr;
void*         g_savedTarget   = nullptr;
Counts        g_counts, g_loggedCounts;
ULONGLONG     g_countsAt      = 0;
ULONGLONG     g_centreAt      = 0;
const wchar_t* g_lastWhy      = nullptr;
float         g_centreTwo[2]  = {};
int           g_lines         = 0;
std::atomic<std::uint64_t> g_offThread{0};
std::uint64_t g_loggedOffThread = 0;
bool          g_sizeDiffers     = false;

// The map pieces' unload test (game thread): each player's last centre as the holder update passed it (12 bytes from
// its rdx), so a run can also ask about the other player's. Valid only while player 2's minimap is on.
struct PieceCounts
{
    std::uint64_t asked = 0, dropped = 0, keptForOther = 0;
};
float         g_pieceCentre[2][3] = {};  // [0] player 1's (the game's tick and player 1's run), [1] player 2's
bool          g_pieceCentreKnown[2] = {};
PieceCounts   g_pieces[2], g_loggedPieces[2];  // by the run asking: [0] player 1's centre, [1] player 2's
ULONGLONG     g_piecesAt        = 0;
bool          g_loggedFirstKeep = false;

/// One "minimap:" line of `level`, within the line budget (a line whose level is off takes none of it).
template <class... A>
void LineAt(platform::Level level, std::wformat_string<A...> format, A&&... args) noexcept
{
    if (g_lines >= kMaxLines || !platform::Enabled(level))
    {
        return;
    }
    try
    {
        ++g_lines;
        platform::LogAt(level, L"minimap: {}", std::format(format, std::forward<A>(args)...));
    }
    catch (...)
    {
    }
}

template <class... A>
void Line(std::wformat_string<A...> format, A&&... args) noexcept
{
    LineAt(platform::Level::Info, format, std::forward<A>(args)...);
}

template <class T>
bool Read(const void* object, std::uint32_t offset, T& out) noexcept
{
    return object != nullptr && platform::CopyGuarded(&out, static_cast<const std::byte*>(object) + offset, sizeof(T));
}

void* Pointer(const void* object, std::uint32_t offset) noexcept
{
    void* value = nullptr;
    Read(object, offset, value);
    return value;
}

void Refuse(const wchar_t* why) noexcept
{
    g_refused     = true;
    g_frameActive = false;
    Line(L"{}; the minimap keeps player 1's map in both halves", why);
}

void* Holder() noexcept
{
    return Pointer(Pointer(reinterpret_cast<const void*>(g_system), kAreaMapManager), kNaviMapHolder);
}

/// The circle minimap's Slate widget, or null (no minimap, or the octagon).
void* CircleSlate() noexcept
{
    const auto* iface = static_cast<const std::byte*>(Pointer(Holder(), kHolderInterface));
    if (iface == nullptr)
    {
        return nullptr;
    }
    void*          slate  = Pointer(iface - kInterfaceInObject, kObjectSlateWidget);
    std::uintptr_t vtable = 0;
    return Read(slate, kSlatePaintInterface, vtable) && vtable == g_circleVTable ? slate : nullptr;
}

bool CentreOf(const void* slate, float (&centre)[2]) noexcept
{
    return Read(Pointer(slate, kSlateContent), kContentCentre, centre);
}

// --- The game's calls, redirected ---

/// The holder update's leader read: player 2's member's type in player 2's run.
std::uint8_t LeaderType() noexcept
{
    if (g_answerType >= 0 && generation::OnGameThread())
    {
        return static_cast<std::uint8_t>(g_answerType);
    }
    return reinterpret_cast<LeaderTypeFn>(g_leaderCall.Original())();
}

/// The holder update's unload test for a shown map piece: the game's own test, asked for this run's centre and, while
/// player 2's minimap is on, for the other player's last centre too. A piece is dropped only when it is out of range
/// of both, so neither player's run drops the pieces the other's map shows. Any other time: the game's answer.
bool ShouldUnload(const void* piece, const float* centre) noexcept
{
    const auto original = reinterpret_cast<ShouldUnloadFn>(g_unloadCall.Original());
    const bool drop     = original(piece, centre);
    if (!g_frameActive || centre == nullptr || !generation::OnGameThread())
    {
        return drop;
    }
    const int run   = g_answerType >= 0 ? 1 : 0;  // the leader answer is set only inside player 2's run
    const int other = 1 - run;
    std::memcpy(g_pieceCentre[run], centre, sizeof(g_pieceCentre[run]));
    g_pieceCentreKnown[run] = true;
    ++g_pieces[run].asked;
    if (!drop || !g_pieceCentreKnown[other])
    {
        g_pieces[run].dropped += drop ? 1 : 0;
        return drop;
    }
    if (!original(piece, g_pieceCentre[other]))
    {
        ++g_pieces[run].keptForOther;
        if (!g_loggedFirstKeep)
        {
            g_loggedFirstKeep = true;
            LineAt(platform::Level::Debug,
                   L"map pieces: {}'s run kept a piece the other player's map shows ({}; their centre ({:.0f}, {:.0f}))",
                   run == 1 ? L"player 2" : L"player 1", piece, g_pieceCentre[other][0], g_pieceCentre[other][1]);
        }
        return false;
    }
    ++g_pieces[run].dropped;
    return true;
}

void LogPieces() noexcept
{
    const ULONGLONG now = GetTickCount64();
    if (!g_unloadCall.IsInstalled() || now < g_piecesAt)
    {
        return;
    }
    g_piecesAt = now + kCountsPeriodMs;
    PieceCounts d[2];
    for (int i = 0; i < 2; ++i)
    {
        d[i] = {g_pieces[i].asked - g_loggedPieces[i].asked, g_pieces[i].dropped - g_loggedPieces[i].dropped,
                g_pieces[i].keptForOther - g_loggedPieces[i].keptForOther};
        g_loggedPieces[i] = g_pieces[i];
    }
    if (d[0].asked == 0 && d[1].asked == 0)
    {
        return;
    }
    LineAt(platform::Level::Debug,
           L"map pieces since the last line: player 1's centre asked {} times, dropped {}, kept for player 2 {}; player "
           L"2's run asked {}, dropped {}, kept for player 1 {}",
           d[0].asked, d[0].dropped, d[0].keptForOther, d[1].asked, d[1].dropped, d[1].keptForOther);
}

/// The circle's AcquireLayerSlot: player 2's pass holds its slot; player 1's pass gets it back instead of a second.
void* Acquire(void* renderer, std::uintptr_t pool) noexcept
{
    const auto original = reinterpret_cast<AcquireFn>(g_acquireCall.Original());
    if (!generation::OnGameThread())
    {
        g_offThread.fetch_add(1, std::memory_order_relaxed);
        return original(renderer, pool);
    }
    g_renderer = renderer;
    if (g_pass == Pass::PlayerOne && g_held != nullptr)
    {
        void* slot = g_held;
        g_held     = nullptr;
        ++g_counts.takenBack;
        return slot;
    }
    void* slot = original(renderer, pool);
    if (g_pass == Pass::PlayerTwo && g_held == nullptr)
    {
        g_held = slot;
        ++g_counts.held;
    }
    return slot;
}

/// The circle's FindOrAddNamedLayerEntry: player 2's entry in the held slot has its own name.
void* FindOrAdd(void* slot, std::uintptr_t name, void* rect, std::uintptr_t unused) noexcept
{
    if (g_pass == Pass::PlayerTwo && slot != nullptr && slot == g_held && generation::OnGameThread())
    {
        name = static_cast<std::uintptr_t>(g_playerTwoName);
        ++g_counts.named;
    }
    return reinterpret_cast<FindOrAddFn>(g_findCall.Original())(slot, name, rect, unused);
}

/// The circle's SubmitLayerSlot: player 2's pass leaves the held slot for player 1's pass to submit.
void Submit(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused) noexcept
{
    if (g_pass == Pass::PlayerTwo && slot != nullptr && slot == g_held && generation::OnGameThread())
    {
        ++g_counts.submitSkipped;
        return;
    }
    reinterpret_cast<SubmitFn>(g_submitCall.Original())(renderer, pool, slot, unused);
}

// --- Player 2's target and buffers ---

bool RootGuarded(void* object) noexcept
{
    __try
    {
        static_cast<RC::Unreal::UObject*>(object)->SetRootSet();
        return true;
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return false;
    }
}

/// Player 2's target, made with the widget's own recipe at the live target's size.
void MakeTarget(const void* slate) noexcept
{
    const void*   live  = Pointer(slate, kSlateTarget);
    std::uint32_t sizeX = 0, sizeY = 0;
    std::uint8_t  flags = 0, format = 0;
    float         gamma = 0.0f;
    if (!Read(live, kTargetSizeX, sizeX) || !Read(live, kTargetSizeY, sizeY) || sizeX == 0 || sizeY == 0 ||
        sizeX > kMaxTargetSize || sizeY > kMaxTargetSize)
    {
        return;  // tried again next tick
    }
    Read(live, kTargetFlags91, flags);
    Read(live, kTargetFormat, format);
    Read(live, kTargetGamma, gamma);
    void* outer  = Pointer(reinterpret_cast<const void*>(g_transientPackage), 0);
    void* target = nullptr;
    if (outer == nullptr || !platform::CallGuarded(g_newTarget, target, outer) || target == nullptr)
    {
        Refuse(L"player 2's render target could not be made");
        return;
    }
    if (!RootGuarded(target))
    {
        Refuse(L"player 2's render target could not be rooted");
        return;
    }
    auto*                  bytes = static_cast<std::byte*>(target);
    std::uint8_t           srgb  = 0;
    const float            one   = 1.0f;
    const float            clear[4] = {};
    Read(target, kTargetFlags91, srgb);
    srgb |= 1;
    std::memcpy(bytes + kTargetFlags91, &srgb, sizeof(srgb));
    std::memcpy(bytes + kTargetGamma, &one, sizeof(one));
    std::memcpy(bytes + kTargetClearColor, clear, sizeof(clear));
    if (!platform::CallGuarded(g_initFormat, target, sizeX, sizeY, kFormatB8G8R8A8, static_cast<std::uint8_t>(1)) ||
        !platform::CallGuarded(g_updateNow, target, static_cast<std::uint8_t>(1)))
    {
        Refuse(L"player 2's render target could not be initialised");
        return;
    }
    g_target = target;
    Line(L"player 2's render target made with the widget's recipe: {}x{} B8G8R8A8, SRGB, gamma 1 (the live target: "
         L"flags91 {:#x}, format {}, gamma {:.2f}) at {}",
         sizeX, sizeY, flags, format, gamma, target);
}

bool TargetReady() noexcept
{
    const void* resource = Pointer(g_target, kTargetResource);
    return resource != nullptr && Pointer(resource, kResourceTarget) != nullptr &&
           Pointer(resource, kResourceTexture) != nullptr;
}

/// Player 2's buffer set, built as the renderer's ctor builds pool 0's, once the renderer is known.
void QueueBuffers() noexcept
{
    if (g_buffersQueued || g_renderer == nullptr)
    {
        return;
    }
    if (!BuildMenuBuffers(g_renderer, kPoolZeroVertexBuffer, kPoolZeroIndexBuffer, g_vertexBufferVtable,
                          g_indexBufferVtable, g_beginInit, g_buffers))
    {
        Refuse(L"pool 0's buffers are not the renderer's buffer kind the research read");
        return;
    }
    g_buffersQueued = true;
    Line(L"player 2's minimap buffers created (capacity {} / {}); waiting for the render thread", kMenuVertexCapacity,
         kMenuIndexCapacity);
}

/// True once the render thread has initialised both buffers (hud/menu_buffers.h, checked every frame); then
/// publishes them to the consume code once.
bool BuffersUsable() noexcept
{
    if (!g_buffersQueued)
    {
        QueueBuffers();
        return false;
    }
    if (!MenuBuffersInitialised(g_buffers))
    {
        return false;
    }
    if (g_fields.playerTwoName == 0)
    {
        g_fields.vertexBuffer = reinterpret_cast<std::uint64_t>(&g_buffers.vertex);
        g_fields.indexBuffer  = reinterpret_cast<std::uint64_t>(&g_buffers.index);
        g_fields.srmSlot      = reinterpret_cast<std::uint64_t>(&g_buffers.srm);
        std::atomic_thread_fence(std::memory_order_release);
        g_fields.playerTwoName = g_playerTwoName;  // last: the consume code switches on this
        Line(L"player 2's minimap buffers are initialised");
    }
    return true;
}

/// The game's entry name, checked against the engine's, and player 2's made once.
void MakeNames() noexcept
{
    std::uint64_t stock = 0;
    if (g_namesReady || !Read(reinterpret_cast<const void*>(g_stockNameStatic), 0, stock) || stock == 0)
    {
        return;  // the game builds its static on the first paint
    }
    try
    {
        g_playerTwoName = EngineNameValue(kPlayerTwoEntryName, RC::Unreal::FNAME_Add);
        if (EngineNameValue(kStockEntryName, RC::Unreal::FNAME_Find) != stock || g_playerTwoName == 0 ||
            g_playerTwoName == stock)
        {
            Refuse(L"an FName built by the mod does not match the engine's layout");
            return;
        }
        g_namesReady = true;
    }
    catch (...)
    {
        Refuse(L"the player-2 entry name could not be made");
    }
}

/// Why player 2's minimap is not used this frame (null: it is).
const wchar_t* WhyNot(bool splitShown, const void* slate) noexcept
{
    if (g_refused)
    {
        return L"refused";
    }
    if (!g_acquireCall.IsInstalled())
    {
        return L"the acquire redirect is not installed";
    }
    if (!splitShown || !generation::Active() || generation::Member() == nullptr)
    {
        return L"no split view with player 2 playing";
    }
    if (slate == nullptr)
    {
        return L"no circle minimap on screen";
    }
    if (!g_namesReady)
    {
        return L"waiting for the game's entry name";
    }
    if (g_target == nullptr || !TargetReady())
    {
        return L"waiting for player 2's render target";
    }
    if (!BuffersUsable())
    {
        return L"waiting for player 2's buffers";
    }
    return nullptr;
}

void LogCounts() noexcept
{
    const ULONGLONG     now       = GetTickCount64();
    const std::uint64_t offThread = g_offThread.load(std::memory_order_relaxed);
    // A safety submit or an acquire off the game thread is logged at once; the steady hand-over every 2 min.
    const bool urgent  = g_counts.safetySubmits != g_loggedCounts.safetySubmits || offThread != g_loggedOffThread;
    const bool changed = g_counts.held != g_loggedCounts.held || urgent;
    if (!changed || (!urgent && now < g_countsAt))
    {
        return;
    }
    g_loggedOffThread = offThread;
    g_countsAt     = now + kCountsPeriodMs;
    const Counts d = {g_counts.held - g_loggedCounts.held,           g_counts.takenBack - g_loggedCounts.takenBack,
                      g_counts.named - g_loggedCounts.named,         g_counts.submitSkipped - g_loggedCounts.submitSkipped,
                      g_counts.safetySubmits - g_loggedCounts.safetySubmits, g_counts.runsTwo - g_loggedCounts.runsTwo,
                      g_counts.runsOne - g_loggedCounts.runsOne};
    g_loggedCounts = g_counts;
    // The steady hand-over is a periodic status line (Debug); a safety submit or an acquire off the game thread is Info.
    LineAt(urgent ? platform::Level::Info : platform::Level::Debug,
           L"since the last line: holder runs {} for player 2, {} for player 1; slots held {}, taken back by player 1's "
           L"pass {}, submitted alone {}; player 2's entries named {}, submits left to player 1 {}; acquires off the game "
           L"thread {}",
           d.runsTwo, d.runsOne, d.held, d.takenBack, d.safetySubmits, d.named, d.submitSkipped,
           g_offThread.load(std::memory_order_relaxed));
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const bool bytes = image.BytesMatch(kHolderUpdate, kHolderUpdateBytes) && image.BytesMatch(kLeaderType, kLeaderTypeBytes) &&
                       image.BytesMatch(kAcquireLayerSlot, kAcquireLayerSlotPrologue) &&
                       image.BytesMatch(engine::kFindOrAddNamedLayerEntry, engine::kFindOrAddNamedLayerEntryPrologue) &&
                       image.BytesMatch(engine::kSubmitLayerSlot, engine::kSubmitLayerSlotPrologue) &&
                       image.BytesMatch(kBeginInitResource, kBeginInitResourcePrologue) &&
                       image.BytesMatch(kNewRenderTarget, kNewRenderTargetBytes) &&
                       image.BytesMatch(kInitCustomFormat, kInitCustomFormatBytes) &&
                       image.BytesMatch(kUpdateResourceImmediate, kUpdateResourceImmediateBytes);
    const auto system    = image.Rebase(kEndUISystem, kAreaMapManager + sizeof(void*));
    const auto holder    = image.Rebase(kHolderUpdate);
    const auto leader    = image.Rebase(kLeaderType);
    const auto leaderAt  = image.Rebase(kLeaderTypeCall, 5);
    const auto acquireAt = image.Rebase(kAcquireCall, 5);
    const auto acquire   = image.Rebase(kAcquireLayerSlot);
    const auto findAt    = image.Rebase(kFindOrAddCall, 5);
    const auto find      = image.Rebase(engine::kFindOrAddNamedLayerEntry);
    const auto submitAt  = image.Rebase(kSubmitCall, 5);
    const auto submit    = image.Rebase(engine::kSubmitLayerSlot);
    const auto name      = image.Rebase(kStockNameStatic, sizeof(std::uint64_t));
    const auto consume   = image.Rebase(kConsumeLoadSite, sizeof(kLoadPoolZeroVertexBufferBytes));
    const auto resume    = image.Rebase(kConsumeLoadResume);
    const auto package   = image.Rebase(kTransientPackage, sizeof(void*));
    const auto circle    = image.Rebase(kCirclePaintVTable, sizeof(void*));
    const auto vbVtable  = image.Rebase(kVertexBufferVtable, sizeof(void*));
    const auto ibVtable  = image.Rebase(kIndexBufferVtable, sizeof(void*));
    const auto init      = image.Rebase(kBeginInitResource);
    const auto newTarget = image.Rebase(kNewRenderTarget);
    const auto format    = image.Rebase(kInitCustomFormat);
    const auto update    = image.Rebase(kUpdateResourceImmediate);
    if (!bytes || !system || !holder || !leader || !leaderAt || !acquireAt || !acquire || !findAt || !find || !submitAt ||
        !submit || !name || !consume || !resume || !package || !circle || !vbVtable || !ibVtable || !init || !newTarget ||
        !format || !update || !platform::PinThisModule())
    {
        platform::Log(L"minimap: the minimap's code is not what the research read; player 1's map in both halves");
        return false;
    }
    g_system             = *system;
    g_holderUpdate       = reinterpret_cast<HolderUpdateFn>(*holder);
    g_acquireSite        = *acquireAt;
    g_acquireTarget      = *acquire;
    g_stockNameStatic    = *name;
    g_transientPackage   = *package;
    g_circleVTable       = *circle;
    g_vertexBufferVtable = *vbVtable;
    g_indexBufferVtable  = *ibVtable;
    g_beginInit          = reinterpret_cast<BeginInitResourceFn>(*init);
    g_newTarget          = reinterpret_cast<NewTargetFn>(*newTarget);
    g_initFormat         = reinterpret_cast<InitFormatFn>(*format);
    g_updateNow          = reinterpret_cast<UpdateNowFn>(*update);

    // The consume code first: it does nothing until player 2's entry name is published with its buffers.
    const auto code = EntryBuffersCode(g_fields, kLoadPoolZeroVertexBufferBytes);
    const bool installed =
        g_consumeCode.Install(*consume, kLoadPoolZeroVertexBufferBytes, code, *resume) &&
        g_leaderCall.Install(*leaderAt, *leader, reinterpret_cast<std::uintptr_t>(&LeaderType)) &&
        g_findCall.Install(*findAt, *find, reinterpret_cast<std::uintptr_t>(&FindOrAdd)) &&
        g_submitCall.Install(*submitAt, *submit, reinterpret_cast<std::uintptr_t>(&Submit));
    if (!installed)
    {
        Remove();
        platform::Log(L"minimap: a site is not what the research read; player 1's map in both halves");
        return false;
    }
    g_installed = true;
    platform::Log(L"minimap: installed (the acquire redirect follows on the game thread)");

    const auto unloadAt = image.Rebase(kPieceUnloadCall, 5);
    const auto unload   = image.Rebase(kPieceShouldUnload);
    const bool pieces   = unloadAt && unload && image.BytesMatch(kPieceUnloadArgs, kPieceUnloadArgsBytes) &&
                        image.BytesMatch(kPieceUnloadAfter, kPieceUnloadAfterBytes) &&
                        g_unloadCall.Install(*unloadAt, *unload, reinterpret_cast<std::uintptr_t>(&ShouldUnload));
    if (pieces)
    {
        platform::Log(L"minimap: map pieces are dropped only when out of range of both players' centres");
    }
    else
    {
        platform::Log(L"minimap: the map pieces' unload test is not what the research read; each run drops the other's "
                      L"pieces (the maps flicker when the players are apart)");
    }
    minimap_labels::Install(image);  // optional: without it, each pass lays the labels out again (logged)
    label_relayouts::Install(image);  // logging: which text blocks relayout, where and why
    minimap_icons::Prepare(image);    // optional: placed from the game thread (Tick); without it the icons churn
    return true;
}

void Remove() noexcept
{
    minimap_labels::Remove();  // player 1's label states back first (on the game thread)
    label_relayouts::Remove();
    minimap_icons::Remove();
    g_frameActive          = false;
    g_fields.playerTwoName = 0;
    bool all = true;
    for (platform::CallRedirect* call : {&g_leaderCall, &g_findCall, &g_submitCall, &g_unloadCall})
    {
        all = (!call->IsInstalled() || call->Restore()) && all;
    }
    all = (!g_consumeCode.IsInstalled() || g_consumeCode.Restore()) && all;
    // The acquire crosses a block: it may only be restored from the game thread outside the paint, so it stays (it
    // passes through while nothing is held; this DLL is pinned).
    if (!all)
    {
        platform::Log(L"minimap: a site was changed after us; left in place (it passes through)");
    }
    g_installed = false;
}

void Tick(bool splitShown) noexcept
{
    if (!g_installed || !generation::OnGameThread())
    {
        return;
    }
    if (!g_acquireTried)
    {
        g_acquireTried = true;  // here the game thread is outside the paint, as InstallFromOwnThread requires
        const bool ok  = g_acquireCall.InstallFromOwnThread(g_acquireSite, g_acquireTarget, reinterpret_cast<std::uintptr_t>(&Acquire));
        Line(L"the circle's acquire redirect {}", ok ? L"installed from the game thread" : L"FAILED (player 1's map in both halves)");
    }
    minimap_icons::InstallFromGameThread();  // once; here the game thread is outside the holder update
    MakeNames();
    void* const slate = CircleSlate();
    if (g_target == nullptr && !g_refused && slate != nullptr && splitShown && generation::Active() && g_namesReady)
    {
        MakeTarget(slate);
    }
    if (g_target != nullptr && slate != nullptr)
    {
        // Player 2's target keeps the size it was made at; a widget with another NaviMapSize would stretch it.
        std::uint32_t live[2] = {}, own[2] = {};
        const bool    differs = Read(Pointer(slate, kSlateTarget), kTargetSizeX, live) && Read(g_target, kTargetSizeX, own) &&
                             (live[0] != own[0] || live[1] != own[1]);
        if (differs != g_sizeDiffers)
        {
            g_sizeDiffers = differs;
            Line(L"the live minimap target is {}x{}, player 2's {}x{}{}", live[0], live[1], own[0], own[1],
                 differs ? L" (player 2's map is scaled to fit)" : L" (sizes match again)");
        }
    }
    const wchar_t* why = WhyNot(splitShown, slate);
    g_frameActive      = why == nullptr;
    if (!g_frameActive)
    {
        g_pieceCentreKnown[0] = g_pieceCentreKnown[1] = false;  // a later split starts from fresh centres
    }
    minimap_labels::Tick(g_frameActive);  // with player 2's minimap off, every label keeps player 1's state only
    label_relayouts::Tick(g_frameActive);
    minimap_icons::Tick(g_frameActive);  // off: the icons only player 2 kept are removed by the next run, as the game does
    if (why != g_lastWhy)
    {
        g_lastWhy = why;
        Line(L"{}", why == nullptr ? L"on" : why);
    }
    LogCounts();
    LogPieces();
}

void RunHolder(bool playerTwo) noexcept
{
    if (!playerTwo)
    {
        minimap_labels::SwapBack();  // normally done after player 2's paint already (EndPlayerTwoPaint)
    }
    if (!g_frameActive)
    {
        g_ranForTwo = false;
        return;
    }
    void* holder = Holder();
    if (holder == nullptr || (!playerTwo && !g_ranForTwo))
    {
        return;
    }
    if (playerTwo)
    {
        const auto type = party_member::PlayerTypeOf(generation::Member());
        if (!type)
        {
            g_ranForTwo = false;
            return;
        }
        g_answerType = *type;
        minimap_labels::BeginPlayerTwoRun();  // its label texts set into player 2's own label states
        minimap_icons::BeginRun(1);           // the icons player 1's last run kept stay (hud/minimap_icons.h)
        g_ranForTwo  = platform::CallGuarded(g_holderUpdate, holder, 0.0f);
        minimap_icons::EndRun(holder);
        minimap_labels::EndPlayerTwoRun();
        g_answerType = -1;
        ++g_counts.runsTwo;
        if (!g_ranForTwo)
        {
            Refuse(L"the navimap holder update raised an exception in player 2's run");
            return;
        }
        CentreOf(CircleSlate(), g_centreTwo);
        return;
    }
    label_relayouts::SetScope(label_relayouts::Scope::PlayerOneRun);
    minimap_icons::BeginRun(0);  // the icons player 2's run kept this frame stay
    const bool ranForOne = platform::CallGuarded(g_holderUpdate, holder, 0.0f);
    minimap_icons::EndRun(holder);
    label_relayouts::SetScope(label_relayouts::Scope::Other);
    if (!ranForOne)
    {
        Refuse(L"the navimap holder update raised an exception in player 1's run");
    }
    ++g_counts.runsOne;
    float centreOne[2] = {};
    const ULONGLONG now = GetTickCount64();
    if (now >= g_centreAt && CentreOf(CircleSlate(), centreOne))
    {
        g_centreAt = now + kCentrePeriodMs;
        LineAt(platform::Level::Debug, L"map centres: player 2's run ({:.0f}, {:.0f}), player 1's run ({:.0f}, {:.0f})",
               g_centreTwo[0], g_centreTwo[1], centreOne[0], centreOne[1]);
    }
}

bool BeginPlayerTwoPaint() noexcept
{
    label_relayouts::SetScope(label_relayouts::Scope::PlayerTwoPaint);  // player 2's overlay render follows
    g_pass = Pass::None;
    g_held = nullptr;
    if (!g_frameActive || !g_ranForTwo)
    {
        return false;
    }
    void* slate = CircleSlate();
    void* live  = Pointer(slate, kSlateTarget);
    if (live == nullptr || !platform::WriteGuarded(static_cast<std::byte*>(slate) + kSlateTarget, &g_target, sizeof(g_target)))
    {
        return false;
    }
    g_swappedSlate = slate;
    g_savedTarget  = live;
    g_pass         = Pass::PlayerTwo;
    return true;
}

void EndPlayerTwoPaint() noexcept
{
    label_relayouts::SetScope(label_relayouts::Scope::Other);
    minimap_labels::SwapBack();  // player 2's paint is done: every label holds player 1's state again
    const bool painted = g_pass == Pass::PlayerTwo;
    if (g_swappedSlate != nullptr)
    {
        // Put the widget's own target back, unless the game replaced the field during the render.
        if (Pointer(g_swappedSlate, kSlateTarget) == g_target &&
            !platform::WriteGuarded(static_cast<std::byte*>(g_swappedSlate) + kSlateTarget, &g_savedTarget,
                                    sizeof(g_savedTarget)))
        {
            Refuse(L"the minimap's own render target could not be put back");
        }
        g_swappedSlate = nullptr;
        g_savedTarget  = nullptr;
    }
    g_pass = painted ? Pass::PlayerOne : Pass::None;
}

void EndOverlayRender() noexcept
{
    if (g_held != nullptr && g_renderer != nullptr)
    {
        // Player 1's pass did not paint the minimap: the held slot is submitted as the game's pass would have, with
        // player 2's entry alone (a slot left acquired would hang its next acquire).
        reinterpret_cast<SubmitFn>(g_submitCall.Original())(g_renderer, 0, g_held, 0);
        ++g_counts.safetySubmits;
    }
    g_held      = nullptr;
    g_pass      = Pass::None;
    g_ranForTwo = false;
}

}  // namespace lss::hud::minimap
