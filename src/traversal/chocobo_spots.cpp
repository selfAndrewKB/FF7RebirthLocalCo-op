#include "traversal/chocobo_spots.h"

#include "facts/party.h"
#include "platform/every.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "rides/chocobo_ride.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace lss::traversal::chocobo_spots {

namespace {

// The spots' slots (research/RE-chocobo-exits-pads.md; 1.005 through replaced-chocobo-spots.json and
// replaced-chocobo-traversal.json). The try-begins take (actor, dt in xmm1, pawn in r8) from the notify pass; the jump
// box's update takes (actor, the pass's context), the pawn at the context's +0x8.
constexpr std::uint8_t kPadTryBytes[]    = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
                                            0x89, 0x78, 0x18, 0x4C, 0x89, 0x70, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0xB8};
constexpr std::uint8_t kBoxTryBytes[]    = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
                                            0x89, 0x78, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1};
constexpr std::uint8_t kSliderTryBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55, 0x48,
                                            0x8D, 0x6C, 0x24, 0xD0, 0xB8, 0x30, 0x01, 0x00, 0x00};
constexpr std::uint8_t kBoxUpdateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                            0x74, 0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};

struct Spot
{
    const wchar_t*     name;
    platform::SlotFact slot;
    bool               buttons;  // the try-begin reads the merged pads (the launch pad's held Accel): the seat's buttons too
    bool               update;   // the jump box's per-frame update, not a try-begin
};
constexpr Spot kSpots[] = {
    {L"launch pad", {L"the chocobo launch pad's try-begin slot", 0x1466F5508,
                     {L"ChocoboJumpStandActor_TryLaunchGlide", 0x1436663C0, kPadTryBytes}}, true, false},
    {L"jump box", {L"the chocobo jump box's try-begin slot", 0x1466F6B40,
                   {L"ChocoboJumpBoxActor_TryBegin", 0x143665FE8, kBoxTryBytes}}, false, false},
    {L"slider", {L"the chocobo slider's try-begin slot", 0x1466F6090,
                 {L"ChocoboSliderActor_TryBegin", 0x14366664C, kSliderTryBytes}}, false, false},
    {L"jump box", {L"the chocobo jump box's update slot", 0x1466F6B48,
                   {L"ChocoboJumpBoxActor_Update_ComboJumpFrame", 0x143662B60, kBoxUpdateBytes}}, false, true},
};
constexpr std::size_t kSpotCount = std::size(kSpots);
constexpr std::size_t kBoxTry = 1, kBoxUpdate = 3;  // the jump box's two slots go in together

using TryBeginFn = void (*)(void* actor, float dt, void* pawn);
using UpdateFn   = void (*)(void* actor, void* context);
constexpr std::uint32_t kContextPawn = 0x8;   // the notify pass's context: the character the pass runs for
constexpr std::uint32_t kHolderState = 0x68;  // the control holder's resolved state (26 the glide, 8 a field action)
using facts::kCharacterControlHolder;

constexpr auto kSummaryEvery = std::chrono::seconds(10);

std::array<platform::VTableEntry, kSpotCount> g_slots;

// Game thread, by seat and spot (seat 0 unused).
struct Counts
{
    unsigned scoped = 0, refused = 0, started = 0;
};
std::array<std::array<Counts, kSpotCount>, players::kMaxPlayers> g_counts{};
platform::Every                                                  g_summary{kSummaryEvery};

std::uint8_t ControlState(const void* chocobo) noexcept
{
    return ReadField<std::uint8_t>(ReadPointer(chocobo, kCharacterControlHolder), kHolderState);
}

struct TryCall
{
    TryBeginFn fn;
    void*      actor;
    float      dt;
    void*      pawn;
};

void RunTry(void* context) noexcept
{
    auto& call = *static_cast<TryCall*>(context);
    call.fn(call.actor, call.dt, call.pawn);
}

struct UpdateCall
{
    UpdateFn fn;
    void*    actor;
    void*    context;
};

void RunUpdate(void* context) noexcept
{
    auto& call = *static_cast<UpdateCall*>(context);
    call.fn(call.actor, call.context);
}

void LogStart(int seat, const Spot& spot, const void* actor, std::uint8_t before, std::uint8_t after) noexcept
{
    try
    {
        platform::LogDebug(L"chocobo spots: {}'s chocobo starts at the {} {} in {}'s scope (its control state {} -> {}; 26 "
                           L"the glide, 8 a field action)",
                           players::SeatName(seat), spot.name, players::NameOf(actor), players::SeatName(seat), before, after);
    }
    catch (...)
    {
    }
}

/// A spot's try-begin: for the chocobo a seat rides, inside the seat's scope (with its pad's buttons where the gates
/// read the pads); otherwise the game's call.
template <std::size_t Index>
void TryBegin(void* actor, float dt, void* pawn) noexcept
{
    const auto original = reinterpret_cast<TryBeginFn>(g_slots[Index].Original());
    const int  seat     = pawn != nullptr && players::generation::OnGameThread() ? rides::chocobo_ride::SeatRiding(pawn) : -1;
    if (seat >= 1)
    {
        Counts&            counts = g_counts[static_cast<std::size_t>(seat)][Index];
        const std::uint8_t before = ControlState(pawn);
        TryCall            call{original, actor, dt, pawn};
        const bool scoped = kSpots[Index].buttons ? rides::chocobo_ride::RunAsRiderWithButtons(pawn, &RunTry, &call)
                                                  : rides::chocobo_ride::RunAsRider(pawn, &RunTry, &call);
        if (scoped)
        {
            ++counts.scoped;
            if (const std::uint8_t after = ControlState(pawn); after != before)
            {
                ++counts.started;
                LogStart(seat, kSpots[Index], actor, before, after);
            }
            return;
        }
        ++counts.refused;
        platform::LogTrace(L"chocobo spots: {}'s chocobo's {} check ran outside {}'s scope (refused)", players::SeatName(seat),
                           kSpots[Index].name, players::SeatName(seat));
    }
    original(actor, dt, pawn);
}

/// The jump box's update: for the chocobo a seat rides, inside the seat's scope (the combo window resolves the
/// chocobo through the primary controller's coordinator); otherwise the game's call.
template <std::size_t Index>
void Update(void* actor, void* context) noexcept
{
    const auto original = reinterpret_cast<UpdateFn>(g_slots[Index].Original());
    void*      pawn     = context != nullptr && players::generation::OnGameThread() ? ReadPointer(context, kContextPawn) : nullptr;
    const int  seat     = rides::chocobo_ride::SeatRiding(pawn);
    if (seat >= 1)
    {
        Counts&    counts = g_counts[static_cast<std::size_t>(seat)][Index];
        UpdateCall call{original, actor, context};
        if (rides::chocobo_ride::RunAsRider(pawn, &RunUpdate, &call))
        {
            ++counts.scoped;
            return;
        }
        ++counts.refused;
    }
    original(actor, context);
}

bool Place(const platform::GameImage& image, std::size_t index, std::uintptr_t replacement) noexcept
{
    return platform::ReplaceSlot(image, kSpots[index].slot, replacement, g_slots[index]);
}

void LogSummary() noexcept
{
    if (!platform::Enabled(platform::Level::Debug) || !g_summary.Due(std::chrono::steady_clock::now()))
    {
        return;
    }
    const auto all = std::exchange(g_counts, {});
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        for (std::size_t i = 0; i < kSpotCount; ++i)
        {
            const Counts& counts = all[static_cast<std::size_t>(seat)][i];
            if (counts.scoped + counts.refused == 0)
            {
                continue;
            }
            try
            {
                platform::LogDebug(L"chocobo spots ({} s): {}'s chocobo's {} {} ran in {}'s scope {} time(s) (refused {}, "
                                   L"started {})",
                                   kSummaryEvery.count(), players::SeatName(seat), kSpots[i].name,
                                   kSpots[i].update ? L"update" : L"check", players::SeatName(seat), counts.scoped,
                                   counts.refused, counts.started);
            }
            catch (...)
            {
            }
        }
    }
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const bool pad    = Place(image, 0, reinterpret_cast<std::uintptr_t>(&TryBegin<0>));
    const bool slider = Place(image, 2, reinterpret_cast<std::uintptr_t>(&TryBegin<2>));
    // The jump box's two slots together or neither: a combo jump decided outside the seat's scope would never come.
    bool box = Place(image, kBoxTry, reinterpret_cast<std::uintptr_t>(&TryBegin<kBoxTry>)) &&
               Place(image, kBoxUpdate, reinterpret_cast<std::uintptr_t>(&Update<kBoxUpdate>));
    if (!box)
    {
        (void)g_slots[kBoxTry].Restore();
    }
    platform::Log(L"chocobo spots: player 2's or 3's chocobo takes the launch pads {}, the jump boxes {} and the sliders {} in "
                  L"that player's scope (each spot's start check; the pad's with their pad's buttons)",
                  pad ? L"yes" : L"NO (a slot differs)", box ? L"yes" : L"NO (a slot differs)",
                  slider ? L"yes" : L"NO (a slot differs)");
}

void Remove() noexcept
{
    for (platform::VTableEntry& slot : g_slots)
    {
        if (slot.Original() != 0 && !slot.Restore())
        {
            platform::Log(L"chocobo spots: a slot was changed after us and is left in place (it passes through)");
        }
    }
}

void Tick() noexcept
{
    if (!players::generation::OnGameThread())
    {
        return;
    }
    LogSummary();
}

}  // namespace lss::traversal::chocobo_spots
