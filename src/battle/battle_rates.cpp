#include "battle/battle_rates.h"

#include "battle/battle_rates_rules.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"

#include "platform/command_registry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace lss::battle::battle_rates {

namespace {

namespace rules    = battle_rates_rules;
namespace settings = platform::settings;

using platform::FunctionFact;

// Facts read from the database (research/RE-battle-rate-sites.md; the research build's addresses).
// The enemy component's battle start (vtable slot 0xD8 of the two enemy component classes).
constexpr std::uint8_t kStartBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57};
constexpr FunctionFact kStart{L"BattleEnemyComp_VtD8_BattleStart_StatsAndExp", 0x14110472C, kStartBytes};
constexpr std::array<platform::SlotFact, 2> kStartSlots{{{L"the enemy component's battle start slot", 0x1457F0D98, kStart},
                                                          {L"the second enemy component's battle start slot", 0x1466B73D0, kStart}}};
// The stat computation and its two enemy calls; the phase change's stagger-share read just before its own.
constexpr std::uint8_t kComputeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57};
constexpr FunctionFact kCompute{L"EnemyParam_ComputeStats_FromSpecAndLevel", 0x14143C34C, kComputeBytes};
constexpr std::uint8_t kShareBytes[] = {0x48, 0x8B, 0x81, 0x90, 0x04, 0x00, 0x00};
constexpr FunctionFact kShare{L"BattleEnemyComp_GetStaggerGaugeShare", 0x14143C85C, kShareBytes};
constexpr std::uintptr_t kStartComputeCall = 0x14143BCC2;
constexpr std::uintptr_t kShareCall        = 0x14143BFE6;
constexpr std::uintptr_t kPhaseComputeCall = 0x14143C155;
// The party-side test, and the boss byte of the component's base row.
constexpr std::uint8_t kPartySideBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr FunctionFact  kPartySide{L"BattleCtrlComp_HasPlayerTypeOrFlags90A098", 0x140A5C154, kPartySideBytes};
constexpr std::uint32_t kComponentBaseRow = 0x3E0;
constexpr std::uint32_t kRowBossByte      = 0x19A;
// The hit's difficulty step: its difficulty read and its rate-row read.
constexpr std::uint8_t kDifficultyBytes[] = {0x48, 0x8B, 0x05};
constexpr FunctionFact kDifficulty{L"Difficulty_GetEffective_DynamicAsNormal", 0x14087F0A8, kDifficultyBytes};
constexpr std::uint8_t kGetFloatBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74};
constexpr FunctionFact kGetFloat{L"EndParam__GetFloatByKey", 0x140A59490, kGetFloatBytes};
constexpr std::uintptr_t kDifficultyCall  = 0x140FB42A3;
constexpr std::uintptr_t kDamageRateCall  = 0x140FB42D1;
constexpr std::uint8_t   kDifficultyEasy   = 0;
constexpr std::uint8_t   kDifficultyNormal = 1;
// An enemy's base EXP at its battle start.
constexpr std::uint8_t kExpValueBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x40, 0x00, 0x00, 0x00};
constexpr FunctionFact kExpValue{L"EnemyEXP_GetValueAtLevel_WithExpUpBonus", 0x1417F5780, kExpValueBytes};
constexpr std::uintptr_t kExpCall = 0x14143B9C8;
// The award's AP bonus: its gate and its row read; 1.005's second gate (that build only).
constexpr std::uint8_t kStoryFlagBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10};
constexpr FunctionFact kStoryFlag{L"StoryFlag_GetByName", 0x1408BED98, kStoryFlagBytes};
constexpr std::uint8_t kGetIntBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10};
constexpr FunctionFact kGetInt{L"EndParam__GetIntByKey", 0x140A59354, kGetIntBytes};
constexpr std::uintptr_t kApGateCall = 0x14143C9FB;
constexpr std::uintptr_t kApRowCall  = 0x14143CA10;
constexpr std::uintptr_t kBonusGate2V1005 = 0x1418F72B8;  // 1.005 only: the award's second bonus gate, u8()
constexpr std::uint8_t   kBonusGate2Bytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8};

using StartFn      = std::uint64_t (*)(void* component, std::uint8_t a, std::uint8_t b);
using ComputeFn    = std::uint64_t (*)(void* estimate, void* param, std::int32_t* hp, std::int32_t* bp, void* pa, void* ma,
                                    void* pd, void* md);
using ShareFn      = float (*)(void* component);
using PartySideFn  = bool (*)(void* component);
using DifficultyFn = std::uint8_t (*)();
using GetFloatFn   = float (*)(const void* key, float fallback);
using ExpValueFn   = std::int32_t (*)(std::uint64_t rowRef, std::int32_t level);
using StoryFlagFn  = bool (*)(const void* key);
using GetIntFn     = std::int32_t (*)(const void* key, std::int32_t fallback);
using GateFn       = std::uint8_t (*)();

std::array<platform::VTableEntry, 2> g_startSlots;
platform::CallRedirect g_startCompute, g_share, g_phaseCompute, g_difficulty, g_damageRate, g_exp, g_apGate, g_apRow;
PartySideFn            g_partySide = nullptr;
GateFn                 g_gate2     = nullptr;  // 1.005 only

/// The rates in tenths (10: the game's own). Set on the game thread, read on any.
struct Rates
{
    std::atomic<int> damage{rules::kOne}, hp{rules::kOne}, boss{rules::kOne}, stagger{rules::kOne}, exp{rules::kOne},
        ap{rules::kOne};
};
Rates g_rates;

thread_local void* t_starting    = nullptr;  // the enemy component whose battle start runs on this thread
thread_local void* t_overriding  = nullptr;  // the component whose phase change runs on this thread
thread_local bool  t_damageNormal = false;   // this hit's difficulty read was Normal, answered Easy for the mod's rate
thread_local bool  t_apOpen       = false;   // the award's real bonus state, its gate answered open for the rate

// --- HP and the stagger gauge --------------------------------------------------------------------------------------

/// The game's computed HP and gauge for `component`, scaled before the game sets them.
void ScaleStats(void* component, std::int32_t* hp, std::int32_t* bp, const wchar_t* when) noexcept
{
    const int hpRate = g_rates.hp.load(std::memory_order_relaxed), bossRate = g_rates.boss.load(std::memory_order_relaxed),
              bpRate = g_rates.stagger.load(std::memory_order_relaxed);
    bool party = true;
    if (component == nullptr || hp == nullptr || bp == nullptr ||
        (hpRate == rules::kOne && bossRate == rules::kOne && bpRate == rules::kOne) ||
        !platform::CallGuarded(g_partySide, party, component) || party)
    {
        return;  // a summon or an ally: as the game made it
    }
    const std::uint8_t* row  = nullptr;
    std::uint8_t        kind = 0;
    if (platform::CopyGuarded(&row, static_cast<const std::uint8_t*>(component) + kComponentBaseRow, sizeof(row)) &&
        row != nullptr)
    {
        platform::CopyGuarded(&kind, row + kRowBossByte, sizeof(kind));
    }
    const bool         boss  = kind == 1;
    const int          rate  = boss ? bossRate : hpRate;
    const std::int32_t hpWas = *hp, bpWas = *bp;
    *hp = rules::Scaled(hpWas, rate);
    *bp = rules::Scaled(bpWas, bpRate);
    // A fixed HP in the enemy's data replaces the computed one after this, as it does the game's own difficulty factor.
    platform::LogDebug(L"battle rates: {} {:p} at {}: computed max HP {} to {} (x{}), stagger gauge {} to {} (x{})",
                       boss ? L"the boss" : L"an enemy", component, when, hpWas, *hp, rules::WordOf(rate), bpWas, *bp,
                       rules::WordOf(bpRate));
}

std::uint64_t Start(void* component, std::uint8_t a, std::uint8_t b, StartFn original) noexcept
{
    void* const previous = t_starting;
    t_starting           = component;
    const std::uint64_t result = original(component, a, b);
    t_starting = previous;
    return result;
}
std::uint64_t StartA(void* component, std::uint8_t a, std::uint8_t b)
{
    return Start(component, a, b, reinterpret_cast<StartFn>(g_startSlots[0].Original()));
}
std::uint64_t StartB(void* component, std::uint8_t a, std::uint8_t b)
{
    return Start(component, a, b, reinterpret_cast<StartFn>(g_startSlots[1].Original()));
}

std::uint64_t ComputeAtStart(void* estimate, void* param, std::int32_t* hp, std::int32_t* bp, void* pa, void* ma, void* pd, void* md)
{
    const std::uint64_t result =
        reinterpret_cast<ComputeFn>(g_startCompute.Original())(estimate, param, hp, bp, pa, ma, pd, md);
    ScaleStats(t_starting, hp, bp, L"its battle start");
    return result;
}

float ShareAtPhase(void* component)
{
    t_overriding = component;  // the phase change's own computation follows in the same call
    return reinterpret_cast<ShareFn>(g_share.Original())(component);
}

std::uint64_t ComputeAtPhase(void* estimate, void* param, std::int32_t* hp, std::int32_t* bp, void* pa, void* ma, void* pd, void* md)
{
    const std::uint64_t result =
        reinterpret_cast<ComputeFn>(g_phaseCompute.Original())(estimate, param, hp, bp, pa, ma, pd, md);
    ScaleStats(t_overriding, hp, bp, L"a phase change");
    t_overriding = nullptr;
    return result;
}

// --- Enemy damage --------------------------------------------------------------------------------------------------

/// The step's difficulty read: Normal answered Easy while the rate is not 1.0, so the step runs and reads its row.
std::uint8_t DifficultyAtHit()
{
    const std::uint8_t difficulty = reinterpret_cast<DifficultyFn>(g_difficulty.Original())();
    t_damageNormal = difficulty == kDifficultyNormal && g_rates.damage.load(std::memory_order_relaxed) != rules::kOne;
    return t_damageNormal ? kDifficultyEasy : difficulty;
}

/// The step's rate-row read: the game's row (1.0 for Normal answered Easy) times the rate.
float DamageRateAtHit(const void* key, float fallback)
{
    const bool  normal = std::exchange(t_damageNormal, false);
    const int   rate   = g_rates.damage.load(std::memory_order_relaxed);
    const float stock  = normal ? 1.0f : reinterpret_cast<GetFloatFn>(g_damageRate.Original())(key, fallback);
    if (rate == rules::kOne)
    {
        return stock;
    }
    const float scaled = stock * rules::Multiplier(rate);
    platform::LogDebug(L"battle rates: an enemy's hit on the party at {:.2f} ({} {:.2f} x{})", scaled,
                       normal ? L"Normal, the game's" : L"the game's row", stock, rules::WordOf(rate));
    return scaled;
}

// --- EXP and AP -----------------------------------------------------------------------------------------------------

std::int32_t ExpAtStart(std::uint64_t rowRef, std::int32_t level)
{
    const std::int32_t value = reinterpret_cast<ExpValueFn>(g_exp.Original())(rowRef, level);
    const int          rate  = g_rates.exp.load(std::memory_order_relaxed);
    if (rate == rules::kOne)
    {
        return value;
    }
    const std::int32_t scaled = rules::Scaled(value, rate);
    platform::LogDebug(L"battle rates: an enemy's EXP {} to {} (x{})", value, scaled, rules::WordOf(rate));
    return scaled;
}

/// The award's bonus gate: open while the rate is not 1.0, its real state kept for the row read.
bool ApGate(const void* key)
{
    const bool real = reinterpret_cast<StoryFlagFn>(g_apGate.Original())(key);
    if (g_rates.ap.load(std::memory_order_relaxed) == rules::kOne)
    {
        return real;
    }
    std::uint8_t second = 0;
    t_apOpen = real || (g_gate2 != nullptr && platform::CallGuarded(g_gate2, second) && second != 0);
    return true;
}

std::int32_t ApRow(const void* key, std::int32_t fallback)
{
    const bool         open  = std::exchange(t_apOpen, false);  // this award's gate, read once
    const std::int32_t stock = reinterpret_cast<GetIntFn>(g_apRow.Original())(key, fallback);
    const int          rate  = g_rates.ap.load(std::memory_order_relaxed);
    if (rate == rules::kOne)
    {
        return stock;
    }
    const std::int32_t percent = rules::AwardPercent(stock, open, rate);
    platform::LogDebug(L"battle rates: the award's AP at {} % (the game's {} %{}, x{})", percent, open ? stock : 100,
                       open ? L"" : L" with its bonus off", rules::WordOf(rate));
    return percent;
}

// --- Install --------------------------------------------------------------------------------------------------------

struct OptionRate
{
    const settings::Option* option;
    std::atomic<int>*       rate;
};
const std::array<OptionRate, 6> kOptionRates{{{&settings::kEnemyDamage, &g_rates.damage},
                                               {&settings::kEnemyHp, &g_rates.hp},
                                               {&settings::kBossHp, &g_rates.boss},
                                               {&settings::kEnemyStagger, &g_rates.stagger},
                                               {&settings::kExpGain, &g_rates.exp},
                                               {&settings::kApGain, &g_rates.ap}}};

void ReadSettings()
{
    for (const OptionRate& o : kOptionRates)
    {
        const int tenths = rules::TenthsOf(settings::Of(*o.option).word).value_or(rules::kOne);
        o.rate->store(tenths);
        if (tenths != rules::kOne)
        {
            platform::Log(L"battle rates: {} x{} ({})", o.option->key, rules::WordOf(tenths), settings::Why(*o.option));
        }
    }
}

/// The six options, for the Co-op page and the console. May throw (out of memory).
void RegisterOptions()
{
    for (const OptionRate& o : kOptionRates)
    {
        std::atomic<int>* const rate = o.rate;
        const wchar_t* const    key  = o.option->key;
        control::RegisterOption(
            *o.option,
            [rate, key](const std::wstring& word) {
                const auto tenths = rules::TenthsOf(word);
                if (tenths)
                {
                    rate->store(*tenths);
                    platform::Log(L"battle rates: {} x{} (the console)", key, word);
                }
                return control::Outcome{tenths.has_value(), tenths ? word : std::wstring(L"not a multiplier from 0.1 to 10.0")};
            },
            [rate] { return std::wstring(rules::WordOf(rate->load())); });
    }
}

/// One call: the game's function at `fact` found, its call at `idaSite` redirected to `replacement`. Every site fits
/// one 16-byte block in both builds, so all are placed at install.
template <class Fn>
bool PlaceCall(const platform::GameImage& image, platform::CallRedirect& call, std::uintptr_t idaSite,
               const FunctionFact& fact, Fn* replacement) noexcept
{
    const auto target = platform::FindFunction(image, fact);
    return target && call.Install(image, idaSite, *target, reinterpret_cast<std::uintptr_t>(replacement));
}

/// The 1.005 bonus gate the AP lever reads (none on the research build); false if its bytes are not the expected.
bool FindSecondGate(const platform::GameImage& image) noexcept
{
    if (platform::RunningBuild() != platform::BuildIndex::V1005)
    {
        return true;
    }
    const std::uintptr_t gate = image.Base() + (kBonusGate2V1005 - 0x140000000);
    std::uint8_t         bytes[sizeof(kBonusGate2Bytes)]{};
    if (!platform::CopyGuarded(bytes, reinterpret_cast<const void*>(gate), sizeof(bytes)) ||
        std::memcmp(bytes, kBonusGate2Bytes, sizeof(bytes)) != 0)
    {
        platform::Log(L"battle rates: the award's second bonus gate is not where expected");
        return false;
    }
    g_gate2 = reinterpret_cast<GateFn>(gate);
    return true;
}

/// One lever's placements, whole or none: when a step fails, the steps already placed are put back, so a lever is never
/// half the mod's (an AP row scaled without its gate would replace the game's own bonus percent).
class Lever
{
public:
    Lever(const platform::GameImage& image, bool ready) noexcept : image_(image), ok_(ready) {}

    template <class Fn>
    Lever& Call(platform::CallRedirect& call, std::uintptr_t idaSite, const FunctionFact& fact, Fn* replacement) noexcept
    {
        if (ok_ && nCalls_ < calls_.size())
        {
            ok_ = PlaceCall(image_, call, idaSite, fact, replacement);
            if (ok_)
            {
                calls_[nCalls_++] = &call;
            }
        }
        return *this;
    }

    template <class Fn>
    Lever& Slot(const platform::SlotFact& fact, Fn* replacement, platform::VTableEntry& entry) noexcept
    {
        if (ok_ && nSlots_ < slots_.size())
        {
            ok_ = platform::ReplaceSlot(image_, fact, reinterpret_cast<std::uintptr_t>(replacement), entry);
            if (ok_)
            {
                slots_[nSlots_++] = &entry;
            }
        }
        return *this;
    }

    /// Whole, or every step put back (the latest first).
    bool Done() noexcept
    {
        if (ok_)
        {
            return true;
        }
        for (std::size_t i = nSlots_; i-- > 0;)
        {
            if (!slots_[i]->Restore())
            {
                platform::Log(L"battle rates: a battle start slot could not be put back (it passes through)");
            }
        }
        for (std::size_t i = nCalls_; i-- > 0;)
        {
            if (!calls_[i]->Restore())
            {
                platform::Log(L"battle rates: a call could not be put back (it passes through)");
            }
        }
        return false;
    }

private:
    const platform::GameImage&            image_;
    bool                                  ok_;
    std::array<platform::CallRedirect*, 2> calls_{};
    std::array<platform::VTableEntry*, 2>  slots_{};
    std::size_t                           nCalls_ = 0, nSlots_ = 0;
};

/// Each lever on its own: one whose check fails stays the game's, whole. The row reads are placed before the reads
/// that lead to them, so a difficulty answered Easy or a gate answered open always meets the mod's row read; the
/// computations before the slots and the share read that name the enemy for them.
bool PlaceSites(const platform::GameImage& image) noexcept
{
    const auto party = platform::FindFunction(image, kPartySide);
    g_partySide      = party ? reinterpret_cast<PartySideFn>(*party) : nullptr;
    const bool start = Lever(image, g_partySide != nullptr)
                           .Call(g_startCompute, kStartComputeCall, kCompute, &ComputeAtStart)
                           .Slot(kStartSlots[0], &StartA, g_startSlots[0])
                           .Slot(kStartSlots[1], &StartB, g_startSlots[1])
                           .Done();
    const bool phase = Lever(image, g_partySide != nullptr)
                           .Call(g_phaseCompute, kPhaseComputeCall, kCompute, &ComputeAtPhase)
                           .Call(g_share, kShareCall, kShare, &ShareAtPhase)
                           .Done();
    const bool damage = Lever(image, true)
                            .Call(g_damageRate, kDamageRateCall, kGetFloat, &DamageRateAtHit)
                            .Call(g_difficulty, kDifficultyCall, kDifficulty, &DifficultyAtHit)
                            .Done();
    const bool exp = Lever(image, true).Call(g_exp, kExpCall, kExpValue, &ExpAtStart).Done();
    const bool ap  = Lever(image, FindSecondGate(image))
                        .Call(g_apRow, kApRowCall, kGetInt, &ApRow)
                        .Call(g_apGate, kApGateCall, kStoryFlag, &ApGate)
                        .Done();
    constexpr const wchar_t* kOff = L"UNAVAILABLE (a check failed): the game's";
    platform::Log(L"battle rates: enemy and boss HP and the stagger gauge at a battle's start {}, at a phase change {}; "
                  L"enemy damage {}; EXP {}; AP {}",
                  start ? L"scaled" : kOff, phase ? L"scaled" : kOff, damage ? L"through the game's difficulty step" : kOff,
                  exp ? L"at each enemy's battle start" : kOff, ap ? L"through the game's bonus at the award" : kOff);
    return start || phase || damage || exp || ap;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    try
    {
        ReadSettings();
    }
    catch (...)
    {
        platform::Log(L"battle rates: the settings could not be read; the rates are the game's");
    }
    try
    {
        RegisterOptions();
    }
    catch (...)
    {
        platform::LogWarning(L"battle rates: the options could not be registered (out of memory?); the page cannot set them");
    }
    return PlaceSites(image);
}

void Remove() noexcept
{
    for (platform::CallRedirect* call : {&g_apGate, &g_apRow, &g_exp, &g_difficulty, &g_damageRate, &g_phaseCompute, &g_share,
                                         &g_startCompute})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"battle rates: a call was changed after us; left in place (it passes through)");
        }
    }
    for (platform::VTableEntry& slot : g_startSlots)
    {
        if (slot.Original() != 0 && !slot.Restore())
        {
            platform::Log(L"battle rates: a battle start slot was changed after us; left in place (it passes through)");
        }
    }
}

}  // namespace lss::battle::battle_rates
