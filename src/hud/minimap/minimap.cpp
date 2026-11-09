#include "hud/minimap/minimap.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>

#include "facts/hud.h"
#include "facts/party.h"
#include "hud/hud_rules.h"
#include "hud/minimap/label_relayouts.h"
#include "hud/minimap/minimap_icons.h"
#include "hud/minimap/minimap_labels.h"
#include "hud/passes/entry_filter_code.h"
#include "hud/passes/menu_buffers.h"
#include "hud/passes/overlay_pass.h"
#include "hud/passes/shared_slot.h"
#include "hud/widget_visibility.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/code_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/module_pin.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "session/party_member.h"

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

namespace generation   = players::generation;
namespace party_member = session::party_member;

// RE-p2-own-minimap.md §1, §3 and §8 (this build). Every function's entry bytes are checked before use.

// The widget, through the navimap holder: g_EndUISystem (the object itself) +0x108 area-map manager, +0x340 holder,
// +0x20 the widget's native interface (UEndNaviMapCircle +0x108), the Slate widget at +0x138 (a TSharedPtr, object
// first), its paint interface (+0x2B0, the vtable says circle), its content SEndNaviMap (+0x3B0) and target (+0x490).
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
constexpr std::uint8_t   kHolderUpdateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54,
                                                 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0x90, 0x03, 0x00, 0x00, 0xE8};
constexpr platform::FunctionFact kHolderUpdate{L"MasterIconPaint_NaviMapHolder_FrameUpdate", 0x140843F10,
                                               kHolderUpdateBytes};
// Its one leader read: call UEndPartyAPI_GetPartyLeaderType (u8 in al) at 0x140844016; location, range, arrow yaw
// and the party-icon skip all follow that answer (no sub-function reads the leader itself, §8.5).
constexpr std::uintptr_t kLeaderTypeCall    = 0x140844016;
using facts::kGetPartyLeaderType;

// The holder update's map pieces (RE-p2-minimap-tiles.md §2, §4): a piece in state 2 (shown) is dropped when the run's
// centre is out of its rectangle + margin + 5 m, asked at 0x140844905 (`lea rdx,[rsp+90h]` = the centre; `mov rcx,rbx`
// = the piece; only al is read after). The pieces' states are shared, so with one centre per run each player's run
// dropped the other's pieces (CONFIRMED live: ~17 load/drop cycles a second, the flicker).
constexpr std::uintptr_t kPieceUnloadCall      = 0x140844905;
constexpr std::uintptr_t kPieceShouldUnload    = 0x141513F24;  // NaviMapPiece__ShouldUnload_CentreOutOfRectPlus500
constexpr std::uint8_t   kPieceUnloadArgsBytes[] = {0x48, 0x8D, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB};
constexpr platform::FunctionFact kPieceUnloadArgs{L"the piece-unload call's arguments", 0x1408448FA,
                                                  kPieceUnloadArgsBytes};
constexpr std::uint8_t   kPieceUnloadAfterBytes[] = {0x84, 0xC0};
constexpr platform::FunctionFact kPieceUnloadAfter{L"after the piece-unload call", 0x14084490A, kPieceUnloadAfterBytes};

// SEndNaviMapCircle__OnPaint_Pool0's pool-0 block (§8.1): the acquire (rcx renderer, dl 0; its call crosses a
// 16-byte block, so it is installed from the game thread while that thread is outside the paint), FindOrAdd (rcx
// slot, rdx the FName value from the static below, r8 rect), and the submit (rcx renderer, dl 0, r8 slot).
constexpr std::uintptr_t kFindOrAddCall    = 0x140FC6CE9;
constexpr std::uintptr_t kStockNameStatic  = 0x14939C2F8;  // FName "SEndNaviMapCircle", built once by the game

// Pool 0's consume: the vertex-buffer load before DrawLayerEntry (r13 = renderer, r9 = entry).
constexpr std::uintptr_t kConsumeLoadSite   = 0x140FC7C91;
constexpr std::uintptr_t kConsumeLoadResume = 0x140FC7C98;
// Game version 1.005 (RE-1005-hud-2.md): the load crosses a 16-byte block there, so the next instruction, the store
// of the vertex-buffer argument, is redirected instead (hud/passes/entry_filter_code.h); the call follows it.
constexpr std::uintptr_t kConsumeStoreSite   = 0x140FC7C98;
constexpr std::uintptr_t kConsumeStoreResume = 0x140FC7C9D;

// Pool 0's buffers (renderer +0x80 vertex, +0x98 index): the same kind and capacities as every pool's (§8.3; uploads
// grow past them on the render thread).
constexpr std::uint32_t  kPoolZeroVertexBuffer = 0x80;
constexpr std::uint32_t  kPoolZeroIndexBuffer  = 0x98;

// The widget's own target recipe (SEndNaviMapCircle__Construct_CreatesRT, §8.4): NewObject in GObjTransientPkg, SRGB
// (+0x91 bit 0), TargetGamma 1, ClearColor 0, InitCustomFormat(X, Y, PF_B8G8R8A8, linear), UpdateResourceImmediate.
constexpr std::uintptr_t kTransientPackage = 0x149115C28;
constexpr std::uint8_t   kNewRenderTargetBytes[] = {0x40, 0x53, 0xB8, 0x60, 0x00, 0x00, 0x00, 0xE8,
                                                    0x38, 0xDA, 0x23, 0xFF, 0x48, 0x2B, 0xE0, 0x48};
constexpr platform::FunctionFact kNewRenderTarget{L"the render target's NewObject", 0x142E714EC, kNewRenderTargetBytes};
constexpr std::uint8_t   kInitCustomFormatBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x86, 0xC0,
                                                     0xEF, 0xFC, 0x48, 0x2B, 0xE0, 0x8A, 0x44, 0x24};
constexpr platform::FunctionFact kInitCustomFormat{L"UTextureRenderTarget2D::InitCustomFormat", 0x1451B2EA0,
                                                   kInitCustomFormatBytes};
constexpr std::uint8_t   kUpdateResourceImmediateBytes[] = {0xB8, 0x58, 0x00, 0x00, 0x00, 0xE8, 0x5A, 0x58,
                                                            0xEF, 0xFC, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x81};
constexpr platform::FunctionFact kUpdateResourceImmediate{L"UTexture::UpdateResourceImmediate", 0x1451B96CC,
                                                          kUpdateResourceImmediateBytes};
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

constexpr ULONGLONG      kCountsPeriodMs     = 120'000;  // a safety submit is logged at once
constexpr ULONGLONG      kCentrePeriodMs     = 120'000;

using HolderUpdateFn = void (*)(void* holder, float deltaTime);
using LeaderTypeFn   = std::uint8_t (*)();
using FindOrAddFn    = void* (*)(void* slot, std::uintptr_t name, void* rect, std::uintptr_t unused);
using NewTargetFn    = void* (*)(void* outer);
using InitFormatFn   = void (*)(void* target, std::uint32_t sizeX, std::uint32_t sizeY, std::uint8_t format,
                              std::uint8_t forceLinearGamma);
using UpdateNowFn    = void (*)(void* target, std::uint8_t clear);
using ShouldUnloadFn = bool (*)(const void* piece, const float* centre);

struct Counts
{
    std::uint64_t runsSeats = 0, runsOne = 0;
};

constexpr int kPlayers = players::kMaxPlayers;

// Installed once, then read.
platform::CallRedirect g_acquireCall;
platform::CallRedirect g_findCall;
platform::CallRedirect g_submitCall;
platform::CallRedirect g_leaderCall;
platform::CallRedirect g_unloadCall;  // optional: without it, the pieces thrash
platform::CodeRedirect g_consumeCode;
bool                   g_installed          = false;
std::uintptr_t         g_system             = 0;
std::uintptr_t         g_acquireSite        = 0;
std::uintptr_t         g_acquireTarget      = 0;
std::uintptr_t         g_stockNameStatic    = 0;
std::uintptr_t         g_transientPackage   = 0;
std::uintptr_t         g_circleVTable       = 0;
HolderUpdateFn         g_holderUpdate       = nullptr;
NewTargetFn            g_newTarget          = nullptr;
InitFormatFn           g_initFormat         = nullptr;
UpdateNowFn            g_updateNow          = nullptr;

// The seats' entries in the circle's pool-0 slot (hud/passes/shared_slot.h): the hold, the names, the buffers.
SharedLayerSlot g_slot{{L"minimap", 0, kPoolZeroVertexBuffer, kPoolZeroIndexBuffer, L"SEndNaviMapCircle",
                        {L"SEndNaviMapCircle_Column2", L"SEndNaviMapCircle_Column3"}}};

// Game thread.
platform::DeferredSites g_acquire;  // crosses a block on both builds: placed from the game thread, off the paint
bool          g_refused       = false;  // a check failed: the earlier behaviour until restart
// By seat ([0] unused).
void*          g_target[kPlayers]      = {};  // the seat's render target (rooted, kept for the process)
bool           g_seatOn[kPlayers]      = {};  // the seat's minimap is used this frame
bool           g_ranFor[kPlayers]      = {};  // the seat's holder run ran this frame
const wchar_t* g_lastWhy[kPlayers]     = {};
float          g_centre[kPlayers][2]   = {};  // the seat's run's map centre (logged)
bool           g_sizeDiffers[kPlayers] = {};
// One run or paint at a time.
int           g_answerType    = -1;  // the leader type the holder update gets in a seat's run
int           g_runSeat       = 0;   // the seat whose holder run runs (0: player 1's centre)
void*         g_swappedSlate  = nullptr;
void*         g_savedTarget   = nullptr;
Counts        g_counts, g_loggedCounts;
SlotCounts    g_loggedSlot;
ULONGLONG     g_countsAt      = 0;
ULONGLONG     g_centreAt      = 0;
std::uint64_t g_loggedOffThread = 0;

/// Any seat's minimap is used this frame.
bool AnySeatOn() noexcept
{
    for (int seat = 1; seat < kPlayers; ++seat)
    {
        if (g_seatOn[seat])
        {
            return true;
        }
    }
    return false;
}

// The map pieces' unload test (game thread): each player's last centre as the holder update passed it (12 bytes from
// its rdx), so a run can also ask about the other players'. Valid only while a seat's minimap is on.
struct PieceCounts
{
    std::uint64_t asked = 0, dropped = 0, keptForOther = 0;
};
float         g_pieceCentre[kPlayers][3] = {};  // [0] player 1's (the game's tick and player 1's run), then by seat
bool          g_pieceCentreKnown[kPlayers] = {};
PieceCounts   g_pieces[kPlayers], g_loggedPieces[kPlayers];  // by the run asking
ULONGLONG     g_piecesAt        = 0;
bool          g_loggedFirstKeep = false;

/// One "minimap:" line of `level`.
template <class... A>
void LineAt(platform::Level level, std::wformat_string<A...> format, A&&... args) noexcept
{
    if (!platform::Enabled(level))
    {
        return;
    }
    try
    {
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

using platform::Read;  // the guarded reads: platform/guarded.h
using platform::ReadPointer;

void Refuse(const wchar_t* why) noexcept
{
    g_refused = true;
    for (bool& on : g_seatOn)
    {
        on = false;
    }
    Line(L"{}; the minimap keeps player 1's map in every view", why);
}

void* Holder() noexcept
{
    return ReadPointer(ReadPointer(reinterpret_cast<const void*>(g_system), facts::kUIAreaMap), kNaviMapHolder);
}

/// The circle minimap's Slate widget, or null (no minimap, or the octagon).
void* CircleSlate() noexcept
{
    const auto* iface = static_cast<const std::byte*>(ReadPointer(Holder(), kHolderInterface));
    if (iface == nullptr)
    {
        return nullptr;
    }
    void*          slate  = ReadPointer(iface - kInterfaceInObject, kObjectSlateWidget);
    std::uintptr_t vtable = 0;
    return Read(slate, kSlatePaintInterface, vtable) && vtable == g_circleVTable ? slate : nullptr;
}

bool CentreOf(const void* slate, float (&centre)[2]) noexcept
{
    return Read(ReadPointer(slate, kSlateContent), kContentCentre, centre);
}

// --- The game's calls, redirected ---

/// The holder update's leader read: the seat's member's type in the seat's run.
std::uint8_t LeaderType() noexcept
{
    if (g_answerType >= 0 && generation::OnGameThread())
    {
        return static_cast<std::uint8_t>(g_answerType);
    }
    return reinterpret_cast<LeaderTypeFn>(g_leaderCall.Original())();
}

/// The holder update's unload test for a shown map piece: the game's own test, asked for this run's centre and, while
/// a seat's minimap is on, for every other player's last centre too. A piece is dropped only when it is out of range
/// of all of them, so no player's run drops the pieces another's map shows. Any other time: the game's answer.
bool ShouldUnload(const void* piece, const float* centre) noexcept
{
    const auto original = reinterpret_cast<ShouldUnloadFn>(g_unloadCall.Original());
    const bool drop     = original(piece, centre);
    if (!AnySeatOn() || centre == nullptr || !generation::OnGameThread())
    {
        return drop;
    }
    const int run = g_answerType >= 0 ? g_runSeat : 0;  // the leader answer is set only inside a seat's run
    std::memcpy(g_pieceCentre[run], centre, sizeof(g_pieceCentre[run]));
    g_pieceCentreKnown[run] = true;
    ++g_pieces[run].asked;
    // The game's answer stands unless some other player's map shows the piece (hud_rules::UnloadVerdict per player).
    auto verdict = hud_rules::Unload::GamesOwn;
    int  keeper  = -1;
    for (int other = 0; other < kPlayers && drop && keeper < 0; ++other)
    {
        if (other == run)
        {
            continue;
        }
        const auto answer = hud_rules::UnloadVerdict(drop, g_pieceCentreKnown[other],
                                                     [&] { return original(piece, g_pieceCentre[other]); });
        keeper  = answer == hud_rules::Unload::KeptForOther ? other : keeper;
        verdict = answer == hud_rules::Unload::GamesOwn ? verdict : answer;
    }
    if (verdict == hud_rules::Unload::GamesOwn)
    {
        g_pieces[run].dropped += drop ? 1 : 0;
        return drop;
    }
    if (keeper >= 0)
    {
        ++g_pieces[run].keptForOther;
        if (!g_loggedFirstKeep)
        {
            g_loggedFirstKeep = true;
            LineAt(platform::Level::Debug,
                   L"map pieces: {}'s run kept a piece {}'s map shows ({}; their centre ({:.0f}, {:.0f}))",
                   players::SeatName(run), players::SeatName(keeper), piece, g_pieceCentre[keeper][0],
                   g_pieceCentre[keeper][1]);
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
    PieceCounts d[kPlayers];
    bool        any = false;
    for (int i = 0; i < kPlayers; ++i)
    {
        d[i] = {g_pieces[i].asked - g_loggedPieces[i].asked, g_pieces[i].dropped - g_loggedPieces[i].dropped,
                g_pieces[i].keptForOther - g_loggedPieces[i].keptForOther};
        g_loggedPieces[i] = g_pieces[i];
        any               = any || d[i].asked != 0;
    }
    if (!any || !platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        std::wstring runs;
        for (int i = 0; i < kPlayers; ++i)
        {
            if (i == 0 || d[i].asked != 0)
            {
                runs += std::format(L"{}{}'s {} asked {} times, dropped {}, kept for another player {}",
                                    i == 0 ? L"" : L"; ", players::SeatName(i), i == 0 ? L"centre" : L"run",
                                    d[i].asked, d[i].dropped, d[i].keptForOther);
            }
        }
        LineAt(platform::Level::Debug, L"map pieces since the last line: {}", runs);
    }
    catch (...)
    {
    }
}

/// The circle's AcquireLayerSlot: the first seat pass holds its slot, later ones take it; player 1's pass gets it
/// back instead of a second.
void* Acquire(void* renderer, std::uintptr_t pool) noexcept
{
    return g_slot.Acquire(renderer, pool, reinterpret_cast<AcquireLayerSlotFn>(g_acquireCall.Original()));
}

/// The circle's FindOrAddNamedLayerEntry: a seat's entry in the held slot has its seat's name.
void* FindOrAdd(void* slot, std::uintptr_t name, void* rect, std::uintptr_t unused) noexcept
{
    if (generation::OnGameThread())
    {
        name = g_slot.NameFor(slot, name);
    }
    return reinterpret_cast<FindOrAddFn>(g_findCall.Original())(slot, name, rect, unused);
}

/// The circle's SubmitLayerSlot: a seat's pass leaves the held slot for player 1's pass to submit.
void Submit(void* renderer, std::uintptr_t pool, void* slot, std::uintptr_t unused) noexcept
{
    if (generation::OnGameThread() && g_slot.SkipSubmit(slot))
    {
        return;
    }
    reinterpret_cast<SubmitLayerSlotFn>(g_submitCall.Original())(renderer, pool, slot, unused);
}

// --- Each seat's target and buffers ---

/// The seat's target, made with the widget's own recipe at the live target's size.
void MakeTarget(int seat, const void* slate) noexcept
{
    const void*   live  = ReadPointer(slate, kSlateTarget);
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
    void* outer  = ReadPointer(reinterpret_cast<const void*>(g_transientPackage), 0);
    void* target = nullptr;
    if (outer == nullptr || !platform::CallGuarded(g_newTarget, target, outer) || target == nullptr)
    {
        Refuse(players::BySeat(seat, L"player 2's render target could not be made",
                               L"player 3's render target could not be made"));
        return;
    }
    if (!players::unreal::SetRootFlag(target, true))
    {
        Refuse(players::BySeat(seat, L"player 2's render target could not be rooted",
                               L"player 3's render target could not be rooted"));
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
        Refuse(players::BySeat(seat, L"player 2's render target could not be initialised",
                               L"player 3's render target could not be initialised"));
        return;
    }
    g_target[seat] = target;
    Line(L"{}'s render target made with the widget's recipe: {}x{} B8G8R8A8, SRGB, gamma 1 (the live target: "
         L"flags91 {:#x}, format {}, gamma {:.2f}) at {}",
         players::SeatName(seat), sizeX, sizeY, flags, format, gamma, target);
}

bool TargetReady(int seat) noexcept
{
    const void* resource = ReadPointer(g_target[seat], kTargetResource);
    return resource != nullptr && ReadPointer(resource, kResourceTarget) != nullptr &&
           ReadPointer(resource, kResourceTexture) != nullptr;
}

/// The buffers of the column the seat's pass draws (its own in the engine's list), built and initialised
/// (hud/passes/shared_slot.h); a wrong buffer kind refuses once.
bool BuffersUsable(int seat) noexcept
{
    const auto& columnOf = overlay_pass::Map().columnOf;
    const int   column   = seat >= 0 && seat < static_cast<int>(columnOf.size()) ? columnOf[static_cast<std::size_t>(seat)] : -1;
    const auto  buffers  = g_slot.BuffersUsable(column);
    if (buffers == SharedLayerSlot::Buffers::WrongKind && !g_refused)
    {
        Refuse(L"pool 0's buffers are not the renderer's buffer kind the research read");
    }
    return buffers == SharedLayerSlot::Buffers::Usable;
}

/// The game's entry name, checked against the engine's, and the seats' made once.
void MakeNames() noexcept
{
    std::uint64_t stock = 0;
    if (g_slot.NamesReady() || !Read(reinterpret_cast<const void*>(g_stockNameStatic), 0, stock) || stock == 0)
    {
        return;  // the game builds its static on the first paint
    }
    if (!g_slot.MakeNames(stock))
    {
        Refuse(L"an FName built by the mod does not match the engine's layout, or could not be made");
    }
}

/// Why the seat's minimap is not used this frame (null: it is).
const wchar_t* WhyNot(int seat, bool splitShown, const void* slate) noexcept
{
    if (g_refused)
    {
        return L"refused";
    }
    if (!g_acquireCall.IsInstalled())
    {
        return L"the acquire redirect is not installed";
    }
    if (!splitShown || !generation::Active(seat) || generation::MemberOf(seat) == nullptr)
    {
        return L"no split view, or the player is not playing";
    }
    if (slate == nullptr)
    {
        return L"no circle minimap on screen";
    }
    if (!g_slot.NamesReady())
    {
        return L"waiting for the game's entry name";
    }
    if (g_target[seat] == nullptr || !TargetReady(seat))
    {
        return L"waiting for the player's render target";
    }
    if (!BuffersUsable(seat))
    {
        return L"waiting for the player's buffers";
    }
    return nullptr;
}

void LogCounts() noexcept
{
    const ULONGLONG     now       = GetTickCount64();
    const SlotCounts&   slot      = g_slot.Counts();
    const std::uint64_t offThread = g_slot.OffThreadAcquires();
    // A safety submit or an acquire off the game thread is logged at once; the steady hand-over every 2 min.
    const bool urgent  = slot.safetySubmits != g_loggedSlot.safetySubmits || offThread != g_loggedOffThread;
    const bool changed = slot.held != g_loggedSlot.held || urgent;
    if (!changed || (!urgent && now < g_countsAt))
    {
        return;
    }
    g_loggedOffThread = offThread;
    g_countsAt        = now + kCountsPeriodMs;
    const Counts     d = {g_counts.runsSeats - g_loggedCounts.runsSeats, g_counts.runsOne - g_loggedCounts.runsOne};
    const SlotCounts s = {.held          = slot.held - g_loggedSlot.held,
                          .takenHeld     = slot.takenHeld - g_loggedSlot.takenHeld,
                          .takenBack     = slot.takenBack - g_loggedSlot.takenBack,
                          .named         = slot.named - g_loggedSlot.named,
                          .submitSkipped = slot.submitSkipped - g_loggedSlot.submitSkipped,
                          .safetySubmits = slot.safetySubmits - g_loggedSlot.safetySubmits};
    g_loggedCounts = g_counts;
    g_loggedSlot   = slot;
    // The steady hand-over is a periodic status line (Debug); a safety submit or an acquire off the game thread is Info.
    LineAt(urgent ? platform::Level::Info : platform::Level::Debug,
           L"since the last line: holder runs {} for players 2 and 3, {} for player 1; slots held {}, taken by a "
           L"later pass {}, taken back by player 1's pass {}, submitted alone {}; players 2 and 3's entries named {}, "
           L"submits left to player 1 {}; acquires off the game thread {}",
           d.runsSeats, d.runsOne, s.held, s.takenHeld, s.takenBack, s.safetySubmits, s.named, s.submitSkipped,
           offThread);
}

/// The circle's acquire call, from the game thread outside the paint (its call crosses a block on both builds).
bool PlaceAcquire(const platform::GameImage&) noexcept
{
    const bool ok =
        g_acquire.Place(g_acquireCall, g_acquireSite, g_acquireTarget, reinterpret_cast<std::uintptr_t>(&Acquire));
    Line(L"the circle's acquire redirect {}",
         ok ? L"installed from the game thread" : L"FAILED (player 1's map in every view)");
    return ok;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto system    = image.Rebase(facts::kEndUISystem, facts::kUIAreaMap + sizeof(void*));
    const auto holder    = platform::FindFunction(image, kHolderUpdate);
    const auto leader    = platform::FindFunction(image, kGetPartyLeaderType);
    const auto leaderAt  = image.Rebase(kLeaderTypeCall, 5);
    const auto acquireAt = platform::FindFunction(image, facts::kNaviMapCircleAcquireCall);
    const auto acquire   = platform::FindFunction(image, facts::kAcquireLayerSlot);
    const auto findAt    = image.Rebase(kFindOrAddCall, 5);
    const auto find      = platform::FindFunction(image, facts::kFindOrAddNamedLayerEntry);
    const auto submitAt  = platform::FindFunction(image, facts::kNaviMapCircleSubmitCall);
    const auto submit    = platform::FindFunction(image, facts::kSubmitLayerSlot);
    const auto name      = image.Rebase(kStockNameStatic, sizeof(std::uint64_t));
    const bool v1005     = platform::RunningBuild() == platform::BuildIndex::V1005;  // its consume site (above)
    const auto consume   = v1005 ? image.Rebase(kConsumeStoreSite, sizeof(kStoreVertexBufferArgBytes1005))
                                 : image.Rebase(kConsumeLoadSite, sizeof(kLoadPoolZeroVertexBufferBytes));
    const auto resume    = image.Rebase(v1005 ? kConsumeStoreResume : kConsumeLoadResume);
    const auto package   = image.Rebase(kTransientPackage, sizeof(void*));
    const auto circle    = image.Rebase(kCirclePaintVTable, sizeof(void*));
    const auto vbVtable  = image.Rebase(facts::kVertexBufferVtable, sizeof(void*));
    const auto ibVtable  = image.Rebase(facts::kIndexBufferVtable, sizeof(void*));
    const auto init      = platform::FindFunction(image, facts::kBeginInitResource);
    const auto newTarget = platform::FindFunction(image, kNewRenderTarget);
    const auto format    = platform::FindFunction(image, kInitCustomFormat);
    const auto update    = platform::FindFunction(image, kUpdateResourceImmediate);
    if (!system || !holder || !leader || !leaderAt || !acquireAt || !acquire || !findAt || !find || !submitAt ||
        !submit || !name || !consume || !resume || !package || !circle || !vbVtable || !ibVtable || !init || !newTarget ||
        !format || !update || !platform::PinThisModule())
    {
        platform::Log(L"minimap: the minimap's code is not what the research read; player 1's map in every view");
        return false;
    }
    g_system             = *system;
    g_holderUpdate       = reinterpret_cast<HolderUpdateFn>(*holder);
    g_acquireSite        = *acquireAt;
    g_acquireTarget      = *acquire;
    g_stockNameStatic    = *name;
    g_transientPackage   = *package;
    g_circleVTable       = *circle;
    g_newTarget          = reinterpret_cast<NewTargetFn>(*newTarget);
    g_initFormat         = reinterpret_cast<InitFormatFn>(*format);
    g_updateNow          = reinterpret_cast<UpdateNowFn>(*update);
    g_slot.Install(*vbVtable, *ibVtable, reinterpret_cast<BeginInitResourceFn>(*init),
                   reinterpret_cast<SubmitLayerSlotFn>(*submit));

    // The consume code first: it does nothing until a seat's entry name is published with its buffers.
    const auto code     = EntryBuffersCode(g_slot.Fields(), kLoadPoolZeroVertexBufferBytes);
    const auto code1005 = EntryBuffersStoreCode1005(g_slot.Fields());
    const bool consumed = v1005 ? g_consumeCode.Install(*consume, kStoreVertexBufferArgBytes1005, code1005, *resume)
                                : g_consumeCode.Install(*consume, kLoadPoolZeroVertexBufferBytes, code, *resume);
    const bool installed =
        consumed &&
        g_leaderCall.Install(*leaderAt, *leader, reinterpret_cast<std::uintptr_t>(&LeaderType)) &&
        g_findCall.Install(*findAt, *find, reinterpret_cast<std::uintptr_t>(&FindOrAdd)) &&
        g_submitCall.Install(*submitAt, *submit, reinterpret_cast<std::uintptr_t>(&Submit));
    if (!installed)
    {
        Remove();
        platform::Log(L"minimap: a site is not what the research read; player 1's map in every view");
        return false;
    }
    g_installed       = true;
    const bool queued = g_acquire.Install(image, &PlaceAcquire, true);  // outside the paint: the session's tick
    platform::Log(L"minimap: installed ({})", queued ? L"the acquire redirect follows on the game thread"
                                                    : L"no room to place the acquire redirect from the game thread");

    const auto unloadAt = image.Rebase(kPieceUnloadCall, 5);
    const auto unload   = image.Rebase(kPieceShouldUnload);
    const bool pieces   = unloadAt && unload && platform::FindFunction(image, kPieceUnloadArgs) &&
                        platform::FindFunction(image, kPieceUnloadAfter) &&
                        g_unloadCall.Install(*unloadAt, *unload, reinterpret_cast<std::uintptr_t>(&ShouldUnload));
    if (pieces)
    {
        platform::Log(L"minimap: map pieces are dropped only when out of range of every player's centre");
    }
    else
    {
        platform::Log(L"minimap: the map pieces' unload test is not what the research read; each run drops the other's "
                      L"pieces (the maps flicker when the players are apart)");
    }
    minimap_labels::Install(image);  // optional: without it, each pass lays the labels out again (logged)
    minimap_icons::Prepare(image);    // optional: its calls placed from the game thread; without it the icons churn
    return true;
}

void Remove() noexcept
{
    minimap_labels::Remove();  // player 1's label states back first (on the game thread)
    minimap_icons::Remove();
    g_acquire.Cancel();  // a placement still queued does nothing
    for (bool& on : g_seatOn)
    {
        on = false;
    }
    g_slot.Unpublish();
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
    MakeNames();
    void* const slate = CircleSlate();
    unsigned    on    = 0;  // bit `seat`: the seat's minimap is used this frame
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (g_target[seat] == nullptr && !g_refused && slate != nullptr && splitShown && generation::Active(seat) &&
            g_slot.NamesReady())
        {
            MakeTarget(seat, slate);
        }
        if (g_target[seat] != nullptr && slate != nullptr)
        {
            // The seat's target keeps the size it was made at; a widget with another NaviMapSize would stretch it.
            std::uint32_t live[2] = {}, own[2] = {};
            const bool    differs = Read(ReadPointer(slate, kSlateTarget), kTargetSizeX, live) &&
                                    Read(g_target[seat], kTargetSizeX, own) && (live[0] != own[0] || live[1] != own[1]);
            if (differs != g_sizeDiffers[seat])
            {
                g_sizeDiffers[seat] = differs;
                Line(L"the live minimap target is {}x{}, {}'s {}x{}{}", live[0], live[1], players::SeatName(seat),
                     own[0], own[1], differs ? L" (their map is scaled to fit)" : L" (sizes match again)");
            }
        }
        const wchar_t* why = WhyNot(seat, splitShown, slate);
        g_seatOn[seat]     = why == nullptr;
        on |= g_seatOn[seat] ? 1u << seat : 0u;
        if (!g_seatOn[seat])
        {
            g_pieceCentreKnown[seat] = false;  // its pieces are no longer kept for it
        }
        if (why != g_lastWhy[seat])
        {
            g_lastWhy[seat] = why;
            Line(L"{} ({})", why == nullptr ? L"on" : why, players::SeatName(seat));
        }
    }
    if (on == 0)
    {
        for (bool& known : g_pieceCentreKnown)
        {
            known = false;  // a later split starts from fresh centres
        }
    }
    minimap_labels::Tick(on);  // a seat whose minimap is off keeps player 1's label state only
    label_relayouts::Tick(on != 0);  // the relayout watch (dev tools; empty in a player's build)
    minimap_icons::Tick(on);  // off: the icons only the seats kept are removed by the next run, as the game does
    LogCounts();
    LogPieces();
}

void RunHolder(int seat) noexcept
{
    const bool isSeat = players::SeatIndex(seat);
    if (!isSeat)
    {
        minimap_labels::SwapBack();  // normally done after the seat's paint already (EndPaint)
    }
    void* holder = Holder();
    if (isSeat)
    {
        g_ranFor[seat] = false;
        if (!g_seatOn[seat] || holder == nullptr)
        {
            return;
        }
        const auto type = party_member::PlayerTypeOf(generation::MemberOf(seat));
        if (!type)
        {
            return;
        }
        g_answerType = *type;
        g_runSeat    = seat;
        minimap_labels::BeginRun(seat);  // its label texts set into the seat's own label states
        minimap_icons::BeginRun(seat);   // the icons the other players' last runs kept stay (hud/minimap/minimap_icons.h)
        g_ranFor[seat] = platform::CallGuarded(g_holderUpdate, holder, 0.0f);
        minimap_icons::EndRun(holder);
        minimap_labels::EndRun();
        g_answerType = -1;
        g_runSeat    = 0;
        ++g_counts.runsSeats;
        if (!g_ranFor[seat])
        {
            Refuse(players::BySeat(seat, L"the navimap holder update raised an exception in player 2's run",
                                   L"the navimap holder update raised an exception in player 3's run"));
            return;
        }
        CentreOf(CircleSlate(), g_centre[seat]);
        return;
    }
    bool anyRan = false;
    for (int s = 1; s < kPlayers; ++s)
    {
        anyRan = anyRan || g_ranFor[s];
    }
    if (holder == nullptr || !anyRan)
    {
        return;  // player 1's own map is the game tick's: only a seat's run changed it
    }
    label_relayouts::SetScope(label_relayouts::Scope::Run, 0);
    minimap_icons::BeginRun(0);  // the icons the seats' runs kept this frame stay
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
    if (now >= g_centreAt && CentreOf(CircleSlate(), centreOne) && platform::Enabled(platform::Level::Debug))
    {
        g_centreAt = now + kCentrePeriodMs;
        try
        {
            std::wstring seats;
            for (int s = 1; s <= players::kSeatsBuilt; ++s)
            {
                seats += std::format(L"{}'s run ({:.0f}, {:.0f}), ", players::SeatName(s), g_centre[s][0], g_centre[s][1]);
            }
            LineAt(platform::Level::Debug, L"map centres: {}player 1's run ({:.0f}, {:.0f})", seats, centreOne[0],
                   centreOne[1]);
        }
        catch (...)
        {
        }
    }
}

bool HidePin(std::uint64_t key) noexcept
{
    void* holder = Holder();
    bool  hidden = false;
    if (holder == nullptr || key == 0)
    {
        return false;
    }
    try
    {
        platform::ForEachSparseElement(reinterpret_cast<std::uintptr_t>(holder) + facts::kHolderEntries, facts::kHolderEntryElement,
                                       [&](std::uintptr_t element) {
                                           std::uint64_t  name = 0;
                                           std::uintptr_t icon = 0;
                                           if (!platform::Read(element, name) || name != key)
                                           {
                                               return true;
                                           }
                                           if (platform::Read(element + facts::kHolderEntryIcon, icon) && icon != 0)
                                           {
                                               hidden = widget_visibility::Set(icon, facts::kVisibilityHidden);
                                           }
                                           return false;
                                       });
    }
    catch (...)
    {
        return false;
    }
    return hidden;
}

bool BeginPaint(int seat) noexcept
{
    label_relayouts::SetScope(label_relayouts::Scope::Paint, seat);  // a seat's overlay render follows
    g_slot.SetActive(false);
    if (!players::SeatIndex(seat) || !g_seatOn[seat] || !g_ranFor[seat])
    {
        return false;
    }
    void* slate = CircleSlate();
    void* live  = ReadPointer(slate, kSlateTarget);
    if (live == nullptr ||
        !platform::WriteGuarded(static_cast<std::byte*>(slate) + kSlateTarget, &g_target[seat], sizeof(void*)))
    {
        return false;
    }
    g_swappedSlate = slate;
    g_savedTarget  = live;
    g_slot.SetActive(true);  // the circle paints for the seat in this pass: its acquire is held
    return true;
}

void EndPaint() noexcept
{
    label_relayouts::SetScope(label_relayouts::Scope::Other);
    g_slot.SetActive(false);
    minimap_labels::SwapBack();  // the seat's paint is done: every label holds player 1's state again
    if (g_swappedSlate != nullptr)
    {
        // Put the widget's own target back, unless the game replaced the field during the render.
        void* const swappedIn = ReadPointer(g_swappedSlate, kSlateTarget);
        bool        ours      = false;
        for (void* const target : g_target)
        {
            ours = ours || (target != nullptr && target == swappedIn);
        }
        if (ours && !platform::WriteGuarded(static_cast<std::byte*>(g_swappedSlate) + kSlateTarget, &g_savedTarget,
                                            sizeof(g_savedTarget)))
        {
            Refuse(L"the minimap's own render target could not be put back");
        }
        g_swappedSlate = nullptr;
        g_savedTarget  = nullptr;
    }
}
void BeginOverlayRender() noexcept
{
    g_slot.BeginFrame();
}

void EndOverlayRender() noexcept
{
    // Player 1's pass did not paint the minimap: the held slot is submitted as the game's pass would have, with
    // the seats' entries alone (a slot left acquired would hang its next acquire). Counted; the status line says so.
    g_slot.SubmitIfLeft();
    for (bool& ran : g_ranFor)
    {
        ran = false;
    }
}

}  // namespace lss::hud::minimap
