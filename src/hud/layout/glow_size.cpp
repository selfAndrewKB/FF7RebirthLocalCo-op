#include "hud/layout/glow_size.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UScriptStruct.hpp>

#include "platform/guarded.h"
#include "platform/log.h"
#include "players/object_slot.h"
#include "players/unreal_access.h"

#if LSS_DEV
#include "platform/command_registry.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lss::hud {

namespace {

using RC::Unreal::FProperty;
using RC::Unreal::UObject;
namespace UObjectGlobals = RC::Unreal::UObjectGlobals;

/// The aura's asset (research/RE-summon-gauge.md §7.5): spawned by name from the gauge widget's tick, loaded with the
/// battle HUD. The head glows are not scaled: each is one sprite the bar's own placement puts at its point.
constexpr const wchar_t* kAuraPath =
    L"/Game/Effect/Resident/Particle/Menu/FX_R_Menu_SummonGauge_Aura_01.FX_R_Menu_SummonGauge_Aura_01";

/// The size constants of the aura's emitters: each emitter's spawn script writes its particles' sprite size (the line
/// flash, a mesh, its scale) from literals of its VM, found by decoding the scripts and confirmed by writing them in
/// the game (research/RE-summon-gauge.md §7.6). Per emitter (the handle's name): the literal's index in the spawn
/// script's table and the asset's value there. A literal that does not hold its value is another build of the asset:
/// it is not written, and the mismatch is logged. `emt`, the GPU sparkle, has no literal table and keeps its size.
struct SizeLiteral
{
    const wchar_t* emitter;
    int            index;
    float          original;
};
constexpr SizeLiteral kSizeLiterals[] = {
    {L"Loop_Gage", 17, 0.75f},    {L"Loop_Gage", 18, 0.1f},   // the band behind the bar: width, height
    {L"Loop_Piz", 18, 0.75f},     {L"Loop_Piz", 19, 0.1f},
    {L"Loop_Magic", 20, 0.68f},   {L"Loop_Magic", 16, 0.1f},
    {L"Start_Gage", 11, 0.7f},    {L"Start_Gage", 12, 0.1f},  // the burst when the gauge appears
    {L"Line", 14, 0.95f},         {L"Line", 15, 0.05f},
    {L"LineFlash", 20, 0.008f},                               // a mesh: its uniform scale
    {L"Flash", 13, 1.5f},                                     // uniform sprites
    {L"FlareRing_01", 11, 0.5f},
};
/// The loaded literal holds the table's value: the same float, with room for a rounding of the asset's own (a relative
/// 1e-6, well under the gap to any neighbouring value of the tables).
bool SameLiteral(float value, float original) noexcept
{
    return std::fabs(value - original) <= 1e-6f * std::max(std::fabs(original), 1e-3f);
}

/// A discovery that found the system but not every script yet is tried again this many times (every
/// kFindRetryTicks), then kept as it is.
constexpr int kMaxPartialReads = 5;

/// The factor written is held within this range (a one-pixel band while a layout changes would ask for nearly nothing)
/// and only after it has stood this many ticks.
constexpr float         kMinFactor     = 0.05f;
constexpr float         kMaxFactor     = 4.0f;
constexpr std::uint32_t kSettledTicks  = 10;
constexpr int           kOffsetRetries = 10;

constexpr std::uint32_t kFindRetryTicks = 30;
constexpr std::size_t   kMaxEmitters    = 32;
constexpr std::size_t   kMaxVariables   = 256;
constexpr std::int32_t  kMaxStoreBytes  = 1 << 20;

/// A TArray's header as the engine lays it out.
struct RawArray
{
    void*        data = nullptr;
    std::int32_t num  = 0;
    std::int32_t max  = 0;
};

/// Every offset this unit reads, from the Niagara classes' reflection; -1 until resolved.
struct Offsets
{
    std::int32_t emitterHandles = -1;  // UNiagaraSystem.EmitterHandles: TArray<FNiagaraEmitterHandle>
    std::int32_t handleSize     = -1;  // FNiagaraEmitterHandle's size
    std::int32_t handleInstance = -1;  //   .Instance: UNiagaraEmitter*
    std::int32_t handleName     = -1;  //   .Name: FName
    std::int32_t spawnProps     = -1;  // UNiagaraEmitter.SpawnScriptProps: FNiagaraEmitterScriptProperties
    std::int32_t updateProps    = -1;  //   .UpdateScriptProps
    std::int32_t gpuScript      = -1;  //   .GPUComputeScript: UNiagaraScript*
    std::int32_t propsScript    = -1;  // FNiagaraEmitterScriptProperties.Script: UNiagaraScript*
    std::int32_t rapidStore     = -1;  // UNiagaraScript.RapidIterationParameters: FNiagaraParameterStore
    std::int32_t execStore      = -1;  //   .ScriptExecutionParamStore (derives from FNiagaraParameterStore)
    std::int32_t vmData         = -1;  //   .CachedScriptVM: FNiagaraVMExecutableData
    std::int32_t vmId           = -1;  //   .CachedScriptVMId: FNiagaraVMExecutableDataId
    std::int32_t usesRapidByte  = -1;  //   FNiagaraVMExecutableDataId.bUsesRapidIterationParams: byte and mask
    std::uint8_t usesRapidMask  = 0;
    std::int32_t storeOffsets   = -1;  // FNiagaraParameterStore.SortedParameterOffsets: TArray<FNiagaraVariableWithOffset>
    std::int32_t storeData      = -1;  //   .ParameterData: TArray<uint8>
    std::int32_t variableSize   = -1;  // FNiagaraVariableWithOffset's size
    std::int32_t variableName   = -1;  //   .Name: FName
    std::int32_t variableOffset = -1;  //   .Offset: int32
    std::int32_t literals       = -1;  // FNiagaraVMExecutableData.ScriptLiterals: TArray<uint8>
    // The probes' extras (not required): the VM's attribute list and byte codes, to read the scripts offline.
    std::int32_t vmAttributes   = -1;  // FNiagaraVMExecutableData.Attributes: TArray<FNiagaraVariable>
    std::int32_t niagaraVarSize = -1;  // FNiagaraVariable's size (Name first)
    std::int32_t vmByteCode     = -1;  //   .ByteCode: TArray<uint8>
    std::int32_t vmOptimized    = -1;  //   .OptimizedByteCode.Data: TArray<uint8>
};

/// One size constant found in the loaded aura: its table entry and the spawn script whose literal table holds it. The
/// table's address is read again at every write (the array could be reallocated), never kept.
struct Scaled
{
    const SizeLiteral* literal = nullptr;
    UObject*           script  = nullptr;
    float              value   = 0.0f;  // the asset's own float, as read (what factor 1 writes back)
};

struct Aura
{
    UObject*            system = nullptr;
    std::int32_t        index  = -1;
    std::vector<Scaled> scaled;
};

std::optional<Offsets> g_offsets;         // game thread; resolved once, kept even if some name failed (no retry)
bool                   g_offsetsTried = false;
std::optional<Aura>    g_aura;            // the loaded system and its scale parameters
std::uint32_t          g_findTicks    = 0;
int                    g_partialReads = 0;     // discoveries that found the system with scripts still unreadable
int                    g_offsetTries  = 0;
float                  g_written      = 1.0f;  // the factor last written (1: the asset's values stand)
float                  g_failedFactor = 0.0f;  // a factor whose write fell short, logged once
float                  g_pending      = 1.0f;  // the factor asked for, and for how many ticks in a row
std::uint32_t          g_pendingTicks = 0;
#if LSS_DEV
std::optional<float> g_override;  // `glow-size <factor>`: instead of the layout's

/// A literal written by `glow-literal`, with the value the asset had, set back by `glow-literal off`.
struct Literal
{
    float*       at       = nullptr;
    float        original = 0.0f;     // the value found there (the asset's, or one this unit had written)
    UObject*     system   = nullptr;  // whose script holds it: set back only while that system still lives
    std::int32_t index    = -1;
};
std::vector<Literal> g_literals;  // dropped with the aura's system (their stores go with it)
#endif

// ---- reflection ---------------------------------------------------------------------------------------------------

RC::Unreal::UStruct* FindStruct(const wchar_t* path) noexcept
{
    try
    {
        return UObjectGlobals::StaticFindObject<RC::Unreal::UStruct*>(nullptr, nullptr, path);
    }
    catch (...)
    {
        return nullptr;
    }
}

/// A property's offset in `owner`, when it has `size` bytes (0: any size); -1 otherwise.
std::int32_t PropertyOffset(RC::Unreal::UStruct* owner, const wchar_t* name, std::int32_t size) noexcept
{
    try
    {
        if (owner == nullptr)
        {
            return -1;
        }
        FProperty* property = owner->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));
        if (property == nullptr || property->GetOffset_Internal() < 0 || (size != 0 && property->GetSize() != size))
        {
            return -1;
        }
        return property->GetOffset_Internal();
    }
    catch (...)
    {
        return -1;
    }
}

std::int32_t StructSize(RC::Unreal::UStruct* owner) noexcept
{
    try
    {
        return owner != nullptr ? owner->GetStructureSize() : -1;
    }
    catch (...)
    {
        return -1;
    }
}

/// A bool property's byte offset and bit mask in `owner`.
bool BoolBit(RC::Unreal::UStruct* owner, const wchar_t* name, std::int32_t& byteOffset, std::uint8_t& mask) noexcept
{
    try
    {
        if (owner == nullptr)
        {
            return false;
        }
        FProperty* property = owner->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));
        if (property == nullptr || !property->IsA<RC::Unreal::FBoolProperty>())
        {
            return false;
        }
        auto* boolean = static_cast<RC::Unreal::FBoolProperty*>(property);
        byteOffset    = property->GetOffset_Internal() + boolean->GetByteOffset();
        mask          = boolean->GetFieldMask();
        return byteOffset >= 0 && mask != 0;
    }
    catch (...)
    {
        return false;
    }
}

Offsets ResolveOffsets() noexcept
{
    Offsets o;
    auto* system   = FindStruct(L"/Script/Niagara.NiagaraSystem");
    auto* handle   = FindStruct(L"/Script/Niagara.NiagaraEmitterHandle");
    auto* emitter  = FindStruct(L"/Script/Niagara.NiagaraEmitter");
    auto* props    = FindStruct(L"/Script/Niagara.NiagaraEmitterScriptProperties");
    auto* script   = FindStruct(L"/Script/Niagara.NiagaraScript");
    auto* store    = FindStruct(L"/Script/Niagara.NiagaraParameterStore");
    auto* variable = FindStruct(L"/Script/Niagara.NiagaraVariableWithOffset");
    auto* vm       = FindStruct(L"/Script/Niagara.NiagaraVMExecutableData");
    auto* vmId     = FindStruct(L"/Script/Niagara.NiagaraVMExecutableDataId");
    constexpr std::int32_t kArray = sizeof(RawArray);
    constexpr std::int32_t kPtr   = sizeof(void*);
    o.emitterHandles = PropertyOffset(system, L"EmitterHandles", kArray);
    o.handleSize     = StructSize(handle);
    o.handleInstance = PropertyOffset(handle, L"Instance", kPtr);
    o.handleName     = PropertyOffset(handle, L"Name", sizeof(std::uint64_t));
    o.spawnProps     = PropertyOffset(emitter, L"SpawnScriptProps", 0);
    o.updateProps    = PropertyOffset(emitter, L"UpdateScriptProps", 0);
    o.gpuScript      = PropertyOffset(emitter, L"GPUComputeScript", kPtr);
    o.propsScript    = PropertyOffset(props, L"Script", kPtr);
    o.rapidStore     = PropertyOffset(script, L"RapidIterationParameters", 0);
    o.execStore      = PropertyOffset(script, L"ScriptExecutionParamStore", 0);
    o.vmData         = PropertyOffset(script, L"CachedScriptVM", 0);
    o.vmId           = PropertyOffset(script, L"CachedScriptVMId", 0);
    if (!BoolBit(vmId, L"bUsesRapidIterationParams", o.usesRapidByte, o.usesRapidMask))
    {
        o.usesRapidByte = -1;
    }
    o.storeOffsets   = PropertyOffset(store, L"SortedParameterOffsets", kArray);
    o.storeData      = PropertyOffset(store, L"ParameterData", kArray);
    o.variableSize   = StructSize(variable);
    o.variableName   = PropertyOffset(variable, L"Name", sizeof(std::uint64_t));
    o.variableOffset = PropertyOffset(variable, L"Offset", sizeof(std::int32_t));
    o.literals       = PropertyOffset(vm, L"ScriptLiterals", kArray);
    o.vmAttributes   = PropertyOffset(vm, L"Attributes", kArray);
    o.niagaraVarSize = StructSize(FindStruct(L"/Script/Niagara.NiagaraVariable"));
    o.vmByteCode     = PropertyOffset(vm, L"ByteCode", kArray);
    const std::int32_t optimized = PropertyOffset(vm, L"OptimizedByteCode", 0);
    const std::int32_t data      = PropertyOffset(FindStruct(L"/Script/Niagara.NiagaraVMExecutableByteCode"), L"Data", kArray);
    o.vmOptimized                = optimized >= 0 && data >= 0 ? optimized + data : -1;
    return o;
}

bool Complete(const Offsets& o) noexcept
{
    return o.emitterHandles >= 0 && o.handleSize > 0 && o.handleInstance >= 0 && o.handleName >= 0 && o.spawnProps >= 0 &&
           o.updateProps >= 0 && o.gpuScript >= 0 && o.propsScript >= 0 && o.rapidStore >= 0 && o.execStore >= 0 &&
           o.vmData >= 0 && o.storeOffsets >= 0 && o.storeData >= 0 && o.variableSize > 0 && o.variableName >= 0 &&
           o.variableOffset >= 0 && o.literals >= 0;
}

const Offsets* KnownOffsets() noexcept
{
    if (!g_offsetsTried)
    {
        // Resolved at the first ticks; a field the reflection does not have yet is tried again a few times before the
        // unit gives up for the session.
        g_offsets = ResolveOffsets();
        if (!Complete(*g_offsets) && ++g_offsetTries < kOffsetRetries)
        {
            return nullptr;
        }
        g_offsetsTried   = true;
        const Offsets& o = *g_offsets;
        if (!Complete(o))
        {
            platform::Log(L"glow size: UNAVAILABLE, Niagara's reflection lacks a field (EmitterHandles {}, handle {}/{}/{}, "
                          L"emitter props {}/{}/{}, script {}, store {}/{}/{}/{}, variable {}/{}/{}, VM {}/{})",
                          o.emitterHandles, o.handleSize, o.handleInstance, o.handleName, o.spawnProps, o.updateProps,
                          o.gpuScript, o.propsScript, o.rapidStore, o.execStore, o.storeOffsets, o.storeData,
                          o.variableSize, o.variableName, o.variableOffset, o.vmData, o.literals);
        }
        else
        {
            platform::Log(L"glow size: the aura's sprite scales follow the band (Niagara's stores through reflection: "
                          L"handles +0x{:X}, scripts +0x{:X}/+0x{:X}/+0x{:X}, rapid store +0x{:X}, offsets +0x{:X}, data "
                          L"+0x{:X}, literals +0x{:X})",
                          o.emitterHandles, o.spawnProps, o.updateProps, o.gpuScript, o.rapidStore, o.storeOffsets,
                          o.storeData, o.vmData + o.literals);
        }
    }
    return g_offsets && Complete(*g_offsets) ? &*g_offsets : nullptr;
}

// ---- the aura's scripts ---------------------------------------------------------------------------------------------

template <class T>
bool ReadAt(const void* base, std::int32_t offset, T& out) noexcept
{
    return base != nullptr && offset >= 0 &&
           platform::CopyGuarded(&out, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
}

enum class ScriptKind : std::uint8_t
{
    Spawn,
    Update,
    Gpu,
};

struct Script
{
    std::wstring emitter;
    ScriptKind   kind   = ScriptKind::Spawn;
    UObject*     object = nullptr;

    const wchar_t* Label() const noexcept
    {
        return kind == ScriptKind::Spawn ? L"spawn" : kind == ScriptKind::Update ? L"update" : L"gpu";
    }
};

/// The aura's emitter scripts, in emitter order: spawn, update, then the GPU script when the emitter has one.
std::vector<Script> ScriptsOf(const Offsets& o, UObject* system)  // may throw (out of memory): the callers catch
{
    std::vector<Script> scripts;
    RawArray handles;
    if (!ReadAt(system, o.emitterHandles, handles) || handles.num <= 0 || handles.num > static_cast<int>(kMaxEmitters))
    {
        return scripts;
    }
    for (std::int32_t i = 0; i < handles.num; ++i)
    {
        const auto* handle = static_cast<const std::uint8_t*>(handles.data) + static_cast<std::ptrdiff_t>(i) * o.handleSize;
        UObject*      emitter = nullptr;
        std::uint64_t name    = 0;
        if (!ReadAt(handle, o.handleInstance, emitter) || emitter == nullptr || !ReadAt(handle, o.handleName, name))
        {
            continue;
        }
        const std::wstring text = players::unreal::NameText(name);
        UObject* spawn = nullptr;
        UObject* update = nullptr;
        UObject* gpu = nullptr;
        ReadAt(emitter, o.spawnProps + o.propsScript, spawn);
        ReadAt(emitter, o.updateProps + o.propsScript, update);
        ReadAt(emitter, o.gpuScript, gpu);
        for (const auto& [kind, object] :
             {std::pair{ScriptKind::Spawn, spawn}, std::pair{ScriptKind::Update, update}, std::pair{ScriptKind::Gpu, gpu}})
        {
            if (object != nullptr)
            {
                scripts.push_back({text, kind, object});
            }
        }
    }
    return scripts;
}

/// One variable of a parameter store: its name, offset and the floats there (as many as fit before the next variable,
/// at most four).
struct Variable
{
    std::wstring name;
    std::int32_t offset = 0;
    float*       at     = nullptr;
    int          count  = 0;
};

std::vector<Variable> VariablesOf(const Offsets& o, const void* store)  // may throw (out of memory): the callers catch
{
    std::vector<Variable> out;
    RawArray offsets, data;
    if (!ReadAt(store, o.storeOffsets, offsets) || !ReadAt(store, o.storeData, data) || offsets.num < 0 ||
        offsets.num > static_cast<int>(kMaxVariables) || data.num < 0 || data.num > kMaxStoreBytes)
    {
        return out;
    }
    out.reserve(static_cast<std::size_t>(offsets.num));
    for (std::int32_t i = 0; i < offsets.num; ++i)
    {
        const auto* entry = static_cast<const std::uint8_t*>(offsets.data) + static_cast<std::ptrdiff_t>(i) * o.variableSize;
        std::uint64_t name   = 0;
        std::int32_t  offset = -1;
        if (!ReadAt(entry, o.variableName, name) || !ReadAt(entry, o.variableOffset, offset) || offset < 0 ||
            offset > data.num - 4)
        {
            continue;
        }
        out.push_back({players::unreal::NameText(name), offset, reinterpret_cast<float*>(static_cast<std::uint8_t*>(data.data) + offset), 0});
    }
    for (auto& variable : out)
    {
        std::int32_t next = data.num;
        for (const auto& other : out)
        {
            if (other.offset > variable.offset && other.offset < next)
            {
                next = other.offset;
            }
        }
        variable.count = std::min(4, (next - variable.offset) / 4);
    }
    return out;
}

/// The literal table of a script's VM data as floats.
bool LiteralsOf(const Offsets& o, UObject* script, float*& at, int& count) noexcept
{
    RawArray literals;
    if (!ReadAt(script, o.vmData + o.literals, literals) || literals.num < 0 || literals.num > kMaxStoreBytes)
    {
        return false;
    }
    at    = static_cast<float*>(literals.data);
    count = literals.num / 4;
    return true;
}

bool AuraLive() noexcept
{
    return g_aura && players::LiveInSlot(g_aura->index, g_aura->system);
}

/// Looks the aura's system up (every kFindRetryTicks while it is not loaded) and reads its scale parameters and their
/// originals. Returns whether it is at hand.
bool FindAura(const Offsets& o)  // may throw (out of memory): the callers catch
{
    if (AuraLive())
    {
        return true;
    }
    if (g_aura)
    {
        platform::Log(L"glow size: the aura's system was unloaded; its scales are read again when it is back");
        g_aura.reset();
        g_written = 1.0f;  // whatever is loaded next has the asset's values
#if LSS_DEV
        g_literals.clear();
#endif
    }
    if (g_findTicks++ % kFindRetryTicks != 0)
    {
        return false;
    }
    UObject* system = nullptr;
    try
    {
        system = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kAuraPath);
    }
    catch (...)
    {
        system = nullptr;
    }
    if (system == nullptr)
    {
        return false;
    }
    Aura aura;
    aura.system = system;
    aura.index  = static_cast<std::int32_t>(system->GetInternalIndex());
    const auto   scripts = ScriptsOf(o, system);
    std::wstring found;
    std::wstring mismatched;
    int          unreadable = 0;
    for (const SizeLiteral& entry : kSizeLiterals)
    {
        const Script* spawn = nullptr;
        for (const Script& script : scripts)
        {
            if (script.kind == ScriptKind::Spawn && script.emitter == entry.emitter)
            {
                spawn = &script;
            }
        }
        float* at    = nullptr;
        int    count = 0;
        float  value = 0.0f;
        if (spawn == nullptr || !LiteralsOf(o, spawn->object, at, count) || entry.index >= count ||
            !platform::CopyGuarded(&value, at + entry.index, sizeof(value)))
        {
            ++unreadable;  // the emitter or its script not filled yet, or not this asset at all
            mismatched += std::format(L" {}[{}] (no such literal)", entry.emitter, entry.index);
            continue;
        }
        if (!SameLiteral(value, entry.original))
        {
            mismatched += std::format(L" {}[{}] holds {:g}, not {:g}", entry.emitter, entry.index, value, entry.original);
            continue;
        }
        aura.scaled.push_back({&entry, spawn->object, value});
        found += std::format(L" {}[{}]={:g}", entry.emitter, entry.index, value);
    }
    if ((scripts.empty() || unreadable > 0) && ++g_partialReads < kMaxPartialReads)
    {
        // The system is there but its emitters or scripts are not filled yet: looked at again, not kept (a few
        // times; a table that never fills is then kept as it is, with the mismatches logged below).
        g_findTicks = 1;
        platform::LogDebug(L"glow size: the aura's system is loaded with {} of its emitters' scripts unreadable yet; read "
                           L"again ({} of {})",
                           unreadable, g_partialReads, kMaxPartialReads);
        return false;
    }
    g_partialReads = 0;
    g_aura         = std::move(aura);
    g_written      = 1.0f;
    g_failedFactor = 0.0f;
    platform::Log(L"glow size: the aura's system loaded; {} of {} size constant(s) as known:{}{}{}", g_aura->scaled.size(),
                  std::size(kSizeLiterals), found.empty() ? L" none" : found.c_str(),
                  mismatched.empty() ? L"" : L"; not written (another build of the asset, or sizes this unit wrote before "
                                             L"its state was lost?):",
                  mismatched);
    return true;
}

/// Writes the asset's own value x factor into every size constant, each through its script's literal table as it is
/// now. Returns the count written.
int WriteScales(const Offsets& o, float factor) noexcept
{
    int written = 0;
    for (const Scaled& scaled : g_aura->scaled)
    {
        float* at    = nullptr;
        int    count = 0;
        if (!LiteralsOf(o, scaled.script, at, count) || scaled.literal->index >= count)
        {
            continue;
        }
        const float value = scaled.value * factor;
        if (platform::WriteGuarded(at + scaled.literal->index, &value, sizeof(value)))
        {
            ++written;
        }
    }
    return written;
}

#if LSS_DEV

/// The aura's system as it is now: the kept one, or the one at its path, whether or not its scale parameters were
/// found (the probes are for seeing why not, too). Null while it is not loaded.
UObject* SystemNow() noexcept
{
    if (AuraLive())
    {
        return g_aura->system;
    }
    try
    {
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, kAuraPath);
    }
    catch (...)
    {
        return nullptr;
    }
}

void DumpStore(const Offsets& o, const Script& script, const wchar_t* which, const void* store)
{
    const auto variables = VariablesOf(o, store);
    platform::Log(L"glow size dump: {} {} {}: {} variable(s)", script.emitter, script.Label(), which, variables.size());
    for (const Variable& v : variables)
    {
        float values[4]{};
        const bool ok = platform::CopyGuarded(values, v.at, sizeof(float) * static_cast<std::size_t>(v.count));
        std::wstring text;
        for (int i = 0; ok && i < v.count; ++i)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &values[i], sizeof(bits));
            text += std::format(L" {:.6g} (0x{:08X})", values[i], bits);
        }
        platform::Log(L"glow size dump:   +{:<4} {}:{}", v.offset, v.name, ok ? text.c_str() : L" unreadable");
    }
}

void Dump()
{
    const Offsets* o = KnownOffsets();
    if (o == nullptr)
    {
        platform::Log(L"glow size dump: no reflection");
        return;
    }
    UObject* system = SystemNow();
    if (system == nullptr)
    {
        platform::Log(L"glow size dump: the aura's system is not loaded (it loads with the battle HUD)");
        return;
    }
    RawArray handles;
    ReadAt(system, o->emitterHandles, handles);
    platform::Log(L"glow size dump: system 0x{:X}, {} emitter handle(s) of {} bytes, kept: {}",
                  reinterpret_cast<std::uintptr_t>(system), handles.num, o->handleSize, g_aura ? L"yes" : L"no");
    for (std::int32_t i = 0; i < handles.num && i < static_cast<std::int32_t>(kMaxEmitters); ++i)
    {
        const auto* handle = static_cast<const std::uint8_t*>(handles.data) + static_cast<std::ptrdiff_t>(i) * o->handleSize;
        UObject*      emitter = nullptr;
        std::uint64_t name    = 0;
        UObject*      spawn   = nullptr;
        UObject*      update  = nullptr;
        UObject*      gpu     = nullptr;
        ReadAt(handle, o->handleInstance, emitter);
        ReadAt(handle, o->handleName, name);
        ReadAt(emitter, o->spawnProps + o->propsScript, spawn);
        ReadAt(emitter, o->updateProps + o->propsScript, update);
        ReadAt(emitter, o->gpuScript, gpu);
        platform::Log(L"glow size dump: handle {}: {} emitter 0x{:X}, spawn 0x{:X}, update 0x{:X}, gpu 0x{:X}", i,
                      players::unreal::NameText(name), reinterpret_cast<std::uintptr_t>(emitter),
                      reinterpret_cast<std::uintptr_t>(spawn), reinterpret_cast<std::uintptr_t>(update),
                      reinterpret_cast<std::uintptr_t>(gpu));
    }
    int n = 0;
    for (const Script& script : ScriptsOf(*o, system))
    {
        auto* base = static_cast<std::uint8_t*>(static_cast<void*>(script.object));
        std::uint8_t usesRapid = 0;
        const bool   hasFlag   = o->usesRapidByte >= 0 && o->vmId >= 0 && ReadAt(base, o->vmId + o->usesRapidByte, usesRapid);
        platform::Log(L"glow size dump: script {}: emitter {} {} (0x{:X}), uses rapid-iteration parameters: {}", n,
                      script.emitter, script.Label(), reinterpret_cast<std::uintptr_t>(script.object),
                      hasFlag ? ((usesRapid & o->usesRapidMask) != 0 ? L"yes" : L"no") : L"?");
        DumpStore(*o, script, L"rapid-iteration store", base + o->rapidStore);
        DumpStore(*o, script, L"execution store", base + o->execStore);
        float* at    = nullptr;
        int    count = 0;
        if (LiteralsOf(*o, script.object, at, count))
        {
            platform::Log(L"glow size dump: {} {} literals: {} float(s)", script.emitter, script.Label(), count);
            for (int i = 0; i < count; i += 8)
            {
                std::wstring line;
                for (int j = i; j < std::min(count, i + 8); ++j)
                {
                    float value = 0.0f;
                    platform::CopyGuarded(&value, at + j, sizeof(value));
                    line += std::format(L" [{}] {:.6g}", j, value);
                }
                platform::Log(L"glow size dump:  {}", line);
            }
        }
        // The VM's attribute list (the data set's variables in order) and its byte codes, as hex, for an offline read
        // of which constants the size writes come from.
        RawArray attributes;
        if (o->vmAttributes >= 0 && o->niagaraVarSize > 0 && ReadAt(base, o->vmData + o->vmAttributes, attributes) &&
            attributes.num > 0 && attributes.num <= static_cast<int>(kMaxVariables))
        {
            std::wstring names;
            for (std::int32_t i = 0; i < attributes.num; ++i)
            {
                std::uint64_t name = 0;
                ReadAt(static_cast<const std::uint8_t*>(attributes.data) + static_cast<std::ptrdiff_t>(i) * o->niagaraVarSize, 0, name);
                names += std::format(L" [{}] {}", i, players::unreal::NameText(name));
            }
            platform::Log(L"glow size dump: {} {} attributes ({}):{}", script.emitter, script.Label(), attributes.num, names);
        }
        for (const auto& [label, offset] : {std::pair{L"bytecode", o->vmByteCode}, std::pair{L"optimized", o->vmOptimized}})
        {
            RawArray code;
            if (offset < 0 || !ReadAt(base, o->vmData + offset, code) || code.num <= 0 || code.num > kMaxStoreBytes)
            {
                continue;
            }
            platform::Log(L"glow size dump: {} {} {}: {} byte(s)", script.emitter, script.Label(), label, code.num);
            constexpr std::int32_t kPerLine = 96;
            for (std::int32_t i = 0; i < code.num; i += kPerLine)
            {
                std::uint8_t bytes[kPerLine]{};
                const auto   count = std::min(kPerLine, code.num - i);
                if (!platform::CopyGuarded(bytes, static_cast<const std::uint8_t*>(code.data) + i, static_cast<std::size_t>(count)))
                {
                    break;
                }
                std::wstring hex;
                for (std::int32_t j = 0; j < count; ++j)
                {
                    hex += std::format(L"{:02x}", bytes[j]);
                }
                platform::Log(L"glow size dump: {} {} {} @{:<5} {}", script.emitter, script.Label(), label, i, hex);
            }
        }
        ++n;
    }
    platform::Log(L"glow size dump: {} script(s); the factor last written {:.3f}", n, g_written);
}

/// `glow-literal <script> <index> <value>`: literal `index` of script `script` (the dump's numbering) set.
control::Outcome SetLiteral(const std::wstring& text)
{
    const Offsets* o      = KnownOffsets();
    UObject*       system = SystemNow();
    if (o == nullptr || system == nullptr)
    {
        return {false, L"the aura's system is not loaded (it loads with the battle HUD)"};
    }
    std::vector<std::wstring> words;
    for (std::size_t at = 0; at < text.size();)
    {
        const std::size_t end = text.find(L' ', at);
        const auto        word = text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        at                    = end == std::wstring::npos ? text.size() : end + 1;
        if (!word.empty())
        {
            words.push_back(word);
        }
    }
    if (words.size() != 3)
    {
        return {false, L"three words wanted: <script> <index> <value>, or off"};
    }
    const auto script = control::ReadWhole(words[0], 10);
    const auto index  = control::ReadWhole(words[1], 10);
    const auto value  = control::ReadReal(words[2]);
    if (!script || !index || !value || *script < 0 || *index < 0)
    {
        return {false, L"<script> and <index> are the dump's numbers, <value> a number"};
    }
    const auto scripts = ScriptsOf(*o, system);
    if (static_cast<std::size_t>(*script) >= scripts.size())
    {
        return {false, std::format(L"{} script(s) only", scripts.size())};
    }
    float* at    = nullptr;
    int    count = 0;
    if (!LiteralsOf(*o, scripts[static_cast<std::size_t>(*script)].object, at, count) || *index >= count)
    {
        return {false, std::format(L"{} literal(s) in that script", count)};
    }
    float* target = at + *index;
    Literal literal{target, 0.0f, system, static_cast<std::int32_t>(system->GetInternalIndex())};
    if (!platform::CopyGuarded(&literal.original, target, sizeof(float)))
    {
        return {false, L"unreadable"};
    }
    const float wanted = static_cast<float>(*value);
    if (!platform::WriteGuarded(target, &wanted, sizeof(wanted)))
    {
        return {false, L"not writable"};
    }
    bool known = false;
    for (const Literal& kept : g_literals)
    {
        known = known || kept.at == target;
    }
    if (!known)
    {
        g_literals.push_back(literal);
    }
    platform::Log(L"glow size: literal {} of script {} ({} {}) set {:.6g} -> {:.6g}", *index, *script,
                  scripts[static_cast<std::size_t>(*script)].emitter, scripts[static_cast<std::size_t>(*script)].Label(),
                  literal.original, wanted);
    return {true, std::format(L"was {:.6g}", literal.original)};
}

void RestoreLiterals() noexcept
{
    for (const Literal& literal : g_literals)
    {
        if (players::LiveInSlot(literal.index, literal.system))  // its system gone: the table went with it
        {
            platform::WriteGuarded(literal.at, &literal.original, sizeof(float));
        }
    }
    if (!g_literals.empty())
    {
        platform::Log(L"glow size: {} literal(s) set back", g_literals.size());
    }
    g_literals.clear();
}

#endif

}  // namespace

void FollowGlowSize(float factor) noexcept
{
    try
    {
#if LSS_DEV
        if (g_override)
        {
            factor = *g_override;
        }
#endif
        if (!std::isfinite(factor) || factor <= 0.0f)
        {
            factor = 1.0f;
        }
        factor = std::clamp(factor, kMinFactor, kMaxFactor);
        // A factor counts once it has stood kSettledTicks ticks: a layout in the middle of a change (a band of a few
        // pixels, an empty layout read as 1) is not written.
        if (std::fabs(factor - g_pending) > 1e-4f)
        {
            g_pending      = factor;
            g_pendingTicks = 0;
        }
        if (++g_pendingTicks < kSettledTicks)
        {
            return;
        }
        g_pendingTicks = kSettledTicks;  // no overflow over a long hold
        const Offsets* o = KnownOffsets();  // resolved at the first ticks, so the startup log states the unit's state
        const bool     wanted = std::fabs(factor - 1.0f) > 1e-4f;
        if (o == nullptr || (!wanted && !g_aura))
        {
            return;  // no reflection; or nothing loaded and nothing to set back
        }
        if (!FindAura(*o) || std::fabs(factor - g_written) <= 1e-4f)
        {
            return;
        }
        if (g_aura->scaled.empty())
        {
            g_written = factor;  // nothing known to write: quiet (the discovery logged why)
            return;
        }
        const int written = WriteScales(*o, factor);
        if (written == static_cast<int>(g_aura->scaled.size()))
        {
            g_written      = factor;
            g_failedFactor = 0.0f;
            platform::Log(L"glow size: the aura's {} size constant(s) written at {:.3f} of the asset's{}; the next aura "
                          L"spawns at that size",
                          written, factor, wanted ? L"" : L" (set back)");
        }
        else if (std::fabs(factor - g_failedFactor) > 1e-4f)
        {
            // Tried again every tick until every write lands; said once per factor.
            g_failedFactor = factor;
            platform::LogDebug(L"glow size: {} of {} size constant(s) written at {:.3f}; the rest tried again",
                               written, g_aura->scaled.size(), factor);
        }
    }
    catch (...)
    {
        platform::Log(L"glow size: failed (an exception); left as it is");
    }
}

void ResetGlowSize() noexcept
{
    try
    {
        const Offsets* o = g_offsets && Complete(*g_offsets) ? &*g_offsets : nullptr;
        if (o == nullptr || !AuraLive() || std::fabs(g_written - 1.0f) <= 1e-4f)
        {
            return;
        }
        const int written = WriteScales(*o, 1.0f);
        g_written         = 1.0f;
        platform::Log(L"glow size: {} of {} size constant(s) set back to the asset's at the mod's uninstall", written,
                      g_aura->scaled.size());
    }
    catch (...)
    {
        platform::Log(L"glow size: the set-back at uninstall failed (an exception)");
    }
}

#if LSS_DEV

void RegisterGlowSizeCommands()
{
    using control::Outcome;
    using control::Value;
    control::Register({.name  = L"glow-size-dump",
                       .shape = control::Shape::None,
                       .help  = L"every parameter store and literal table of the summon gauge aura's Niagara scripts, "
                                L"to the log (the aura loads with the battle HUD)",
                       .set   = [](const Value&) {
                           try
                           {
                               Dump();
                           }
                           catch (...)
                           {
                               return Outcome{false, L"failed (an exception)"};
                           }
                           return Outcome{true, L"in the log"};
                       }});
    control::Register({.name     = L"glow-size",
                       .shape    = control::Shape::Float,
                       .minFloat = 0.01,
                       .maxFloat = 10.0,
                       .words    = {L"auto"},
                       .help     = L"the factor the aura's sprite scales are written at instead of the layout's "
                                   L"(0.01-10), or `auto` for the layout's",
                       .set      = [](const Value& v) {
                           if (std::holds_alternative<std::wstring>(v))
                           {
                               g_override.reset();
                               return Outcome{true, L"the layout's factor again at the next tick"};
                           }
                           g_override = static_cast<float>(std::get<double>(v));
                           return Outcome{true, L"written at the next tick"};
                       },
                       .get = [] {
                           return g_override ? Value{static_cast<double>(*g_override)} : Value{std::wstring(L"auto")};
                       }});
    control::Register({.name  = L"glow-literal",
                       .shape = control::Shape::Text,
                       .help  = L"one literal of one of the aura's scripts set, `<script> <index> <value>` (the dump's "
                                L"numbers), or `off` to set every touched literal back",
                       .set   = [](const Value& v) {
                           try
                           {
                               const std::wstring text = std::get<std::wstring>(v);
                               if (text == L"off")
                               {
                                   RestoreLiterals();
                                   return Outcome{true, L"set back"};
                               }
                               return SetLiteral(text);
                           }
                           catch (...)
                           {
                               return Outcome{false, L"failed (an exception)"};
                           }
                       }});
}

#endif

}  // namespace lss::hud
