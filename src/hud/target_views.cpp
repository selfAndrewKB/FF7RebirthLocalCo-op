#include "hud/target_views.h"

#include "hud/hud_rules.h"
#include "hud/widget_opacity.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "player_two/generation.h"
#include "player_two/lock_on.h"
#include "player_two/weak_object.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace lss::hud::target_views {

namespace {

using platform::CallGuarded;

// Facts read from the database (RE-p2-hud-targets.md §3, R3; this build).
constexpr std::uint8_t kCollectBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
                                          0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xF8,
                                          0xE7, 0xFF, 0xFF, 0xB8, 0xE0, 0x18, 0x00, 0x00, 0xE8, 0x6F, 0x2D, 0x2F, 0x01};
constexpr std::uint8_t kMainTargetBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                             0x41, 0x56, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xAC, 0xA0, 0xA9, 0x00};
constexpr std::uint8_t kReleaseBytes[]    = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x46, 0x74, 0x82, 0x01};
constexpr std::uint8_t kEnemyAiBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0x23, 0x2F, 0x01};

constexpr platform::FunctionFact kCollect{L"EndUI__CollectLockOnTargets", 0x140DBC198, kCollectBytes};
constexpr platform::FunctionFact kMainTarget{L"EndUI__GetMainTargetOfLockOnTarget", 0x141614E6C, kMainTargetBytes};
constexpr platform::FunctionFact kRelease{L"TWeakPtrTS__Release", 0x140887AE0, kReleaseBytes};
constexpr platform::FunctionFact kEnemyAi{L"EndCharacter_GetEnemyAIComponent", 0x140DBCBBC, kEnemyAiBytes};

// The red ability label's lookup of its enemy's gauge: the entry for the character whose source is
// the targeted key and whose widget is not Collapsed (EndUI__MF8_FindStatusScreenPointForCharacter;
// its one call, in the ability-name tick; RE-p2-battle-popups.md §2.2).
constexpr std::uint8_t kLookupBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x4C, 0x89, 0x48, 0x20, 0x4C,
                                         0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};
constexpr platform::FunctionFact kLookup{L"EndUI__MF8_FindStatusScreenPointForCharacter", 0x140A68C84, kLookupBytes};
constexpr std::uintptr_t         kLookupCall = 0x140A68826;
constexpr std::uint32_t          kTargetedKey = 0x140;  // the bar manager's targeted key, a TWeakPtr {object, controller}
// The gauge manager's two calls to the collection: creating entries for new sources, and keeping only
// collected ones (§3.1).
constexpr std::uintptr_t kCreateCall = 0x140A66B85;
constexpr std::uintptr_t kSyncCall   = 0x140B20F39;

// The battle system's displayed target and cursor (player 1's outside every scope).
constexpr std::uintptr_t kBattleSystem = 0x14939BC58;
constexpr std::uint32_t  kCursor       = 0x68;
constexpr std::uint32_t  kDisplayed    = 0x70;

// The gauge manager (EndUISystem[31]): its entries (a TSet, elements of 0x180 bytes: +0 source, +0x10
// the weak widget) and the sticky list (+0xB0 {object, controller} pairs, +0xB8 count).
constexpr std::uintptr_t kEndUISystem = 0x1493496E0;
constexpr std::uint32_t  kManager     = 0xF8;
constexpr std::uint32_t  kEntries     = 0x10;
constexpr std::uint32_t  kElement     = 0x180;
constexpr std::uint32_t  kEntryWidget = 0x10;
constexpr std::uint32_t  kSticky      = 0xB0;
constexpr std::uint32_t  kStickyCount = 0xB8;
constexpr std::int32_t   kMaxSticky   = 64;
// A lock-on target's owner actor (vt+0x28); an enemy AI component's "always show status" byte.
constexpr std::uint32_t  kOwnerSlot   = 0x28;
constexpr std::uint32_t  kShowStatus  = 0x773;
constexpr std::uint32_t  kSharedCount = 0x8;


struct Pair  // a thread-safe TWeakPtr
{
    std::uintptr_t object     = 0;
    std::uintptr_t controller = 0;
    bool operator==(const Pair&) const = default;
};

struct Hidden
{
    std::uint64_t weak     = 0;     // the widget's FWeakObjectPtr
    float         previous = 1.0f;  // its own render opacity
};

using CollectFn    = void (*)(void* out);
using MainTargetFn = Pair* (*)(Pair* out, const Pair* in);
using ReleaseFn    = void (*)(Pair* weak);
using EnemyAiFn    = std::uintptr_t (*)(void* character);
using OwnerFn      = void* (*)(void* target);
using LookupFn     = std::uint8_t (*)(void* manager, float* point, float* scale, void* character);

// Set once at install.
platform::CallRedirect g_createCall;
platform::CallRedirect g_syncCall;
platform::CallRedirect g_lookupCall;
bool           g_installed    = false;
bool           g_unionOn      = false;
MainTargetFn   g_mainTarget   = nullptr;
ReleaseFn      g_release      = nullptr;
EnemyAiFn      g_enemyAi      = nullptr;
std::uintptr_t g_battleSystem = 0;
std::uintptr_t g_endUISystem  = 0;
// 1.005: the call sites are placed from the game thread (platform/deferred_sites.h), with what Install found kept
// for it.
platform::DeferredSites g_placement;
std::uintptr_t          g_collect = 0;  // the collection function

/// One main-target lookup, kept for the rest of the frame.
struct MainLookup
{
    Pair                          in;
    std::array<std::uintptr_t, 2> out{};
};

/// A player-2 label lookup, for the log.
struct LabelLog
{
    std::uintptr_t character = 0, key = 0;
    bool           found = false, ok = true;
    bool operator==(const LabelLog&) const = default;
};

/// What a pass took out, for the log.
struct PassHides
{
    bool           cursor = false;
    std::int32_t   gauges = 0;
    std::uintptr_t hiddenCursor = 0;  // the cursor this pass takes out (the other player's)
    std::uintptr_t ownCursor    = 0;  // this pass's own player's cursor
    std::int32_t   entries      = 0;  // the bar manager's gauges this frame
    bool           list         = false;  // player 2's command target list open
    bool operator==(const PassHides&) const = default;
};

// Game thread.
std::vector<Hidden>         g_hidden;
std::array<MainLookup, 4>   g_lookups{};      // this frame's main-target lookups (BeginFrame empties them)
std::size_t                 g_lookupCount = 0;
bool                        g_gaugesFound = false;  // this frame's walk done
std::array<std::vector<std::uint64_t>, 2> g_otherGauges;  // per pass (0 player 1's, 1 player 2's): the other player's
                                                          // gauges, as their widgets' FWeakObjectPtr (resolved at use)
int                         g_currentPass = -1;  // the view whose HUD updates and render run (0, 1), else -1
bool                        g_failed = false;
std::array<PassHides, 2>    g_loggedHides{};
std::array<LabelLog, 8>     g_loggedLabels{};  // per character, as last logged
std::uint32_t               g_nextLabel = 0;   // the slot a newly seen character takes
std::array<std::int32_t, 2> g_loggedUnion{-1, -1};
std::uint32_t               g_listOpenings = 0;  // player 2's target list openings seen: each logs the hides afresh
std::int32_t                g_entryCount   = 0;  // the bar manager's gauges, counted by this frame's walk (the log)

using platform::ReadAt;  // the guarded reads: platform/guarded.h

/// One line of `level`. Callers of Debug and
/// Trace lines check platform::Enabled before building `text`.
void Note(platform::Level level, const std::wstring& text)
{
    if (platform::Enabled(level))
    {
        platform::LogAt(level, L"target views: {}", text);
    }
}

void Note(const std::wstring& text)
{
    Note(platform::Level::Info, text);
}

std::int32_t Count(const void* list) noexcept
{
    return ReadAt<std::int32_t>(reinterpret_cast<std::uintptr_t>(list) + 8);
}

/// Replaces both collection calls: the game's collection, then again with player 2's target fields.
void Collect(void* out) noexcept
{
    const auto original = reinterpret_cast<CollectFn>(g_createCall.Original());
    original(out);
    if (!g_unionOn || !player_two::generation::Active() || player_two::generation::Member() == nullptr ||
        player_two::lock_on::DisplayedObject() == 0)
    {
        return;
    }
    const std::int32_t              before = Count(out);
    const player_two::lock_on::TargetScope scope;
    if (!scope.Held())
    {
        return;
    }
    original(out);
    const std::int32_t after = Count(out);
    if (!platform::Enabled(platform::Level::Debug) || (before == g_loggedUnion[0] && after == g_loggedUnion[1]))
    {
        return;
    }
    g_loggedUnion = {before, after};
    try
    {
        Note(platform::Level::Debug, std::format(L"collected {} for player 1, {} with player 2's target", before, after));
    }
    catch (...)
    {
    }
}

/// The target and, if it is a part, its main target; looked up once per frame per target.
std::array<std::uintptr_t, 2> MainOf(const Pair& target)
{
    for (std::size_t i = 0; i < g_lookupCount; ++i)
    {
        if (g_lookups[i].in == target)
        {
            return g_lookups[i].out;
        }
    }
    std::array<std::uintptr_t, 2> objects{target.object, 0};
    if (target.object == 0 || target.controller == 0 || ReadAt<std::int32_t>(target.controller + kSharedCount) <= 0)
    {
        return {};
    }
    Pair  main;
    Pair* result = nullptr;
    if (!CallGuarded(g_mainTarget, result, &main, &target))
    {
        g_failed = true;
        Note(L"the main-target lookup raised an exception; marks are no longer hidden per view");
        return objects;
    }
    objects[1] = main.object;
    CallGuarded(g_release, &main);
    if (g_lookupCount < g_lookups.size())
    {
        g_lookups[g_lookupCount++] = {target, objects};
    }
    return objects;
}

bool Contains(const std::array<std::uintptr_t, 2>& set, std::uintptr_t object) noexcept
{
    return object != 0 && (set[0] == object || set[1] == object);
}

/// In both players' games: its enemy always shows status, or it is on the sticky list.
bool Shared(std::uintptr_t source, std::uintptr_t manager)
{
    void*          owner = nullptr;
    std::uintptr_t ai    = 0;
    const auto     slot  =
        reinterpret_cast<OwnerFn>(ReadAt<std::uintptr_t>(ReadAt<std::uintptr_t>(source) + kOwnerSlot));
    if (CallGuarded(slot, owner, reinterpret_cast<void*>(source)) && owner != nullptr && CallGuarded(g_enemyAi, ai, owner) &&
        ai != 0 && ReadAt<std::uint8_t>(ai + kShowStatus) != 0)
    {
        return true;
    }
    const auto         sticky = ReadAt<std::uintptr_t>(manager + kSticky);
    const std::int32_t count  = ReadAt<std::int32_t>(manager + kStickyCount);
    for (std::int32_t i = 0; sticky != 0 && i < count && i < kMaxSticky; ++i)
    {
        if (ReadAt<std::uintptr_t>(sticky + sizeof(Pair) * static_cast<std::uint32_t>(i)) == source)
        {
            return true;
        }
    }
    return false;
}

/// A widget from its FWeakObjectPtr value, or null if it is gone.
void* WidgetOf(std::uint64_t weak) noexcept
{
    return player_two::weak_object::ObjectOf(weak);
}

/// Takes `widget` out of this pass's image (render opacity 0, so it stays painted and ticked),
/// remembering its own opacity. True if it did.
bool Hide(void* widget) noexcept
{
    const float previous = widget_opacity::Get(widget);
    if (widget == nullptr || previous == 0.0f)
    {
        return false;  // not drawn anyway
    }
    std::uint64_t weak = 0;
    if (!player_two::weak_object::Assign(&weak, widget) || !widget_opacity::Set(widget, 0.0f))
    {
        return false;
    }
    try
    {
        g_hidden.push_back({weak, previous});
    }
    catch (...)
    {
        widget_opacity::Set(widget, previous);  // not recorded: not left transparent
        return false;
    }
    return true;
}

/// Replaces the red ability label's gauge lookup. The lookup considers only the gauge of the bar
/// manager's targeted key (+0x140/+0x148, the locked-on target, player 1's; read locally at
/// 0x140A68CC1 and 0x140A68E01): the label makes way only for that gauge. During player 2's
/// updates the key holds player 2's displayed target for the length of this call, and player 1's
/// is put back after, so player 2's label makes way for player 2's locked-on gauge only.
std::uint8_t LabelLookup(void* manager, float* point, float* scale, void* character) noexcept
{
    const auto original = reinterpret_cast<LookupFn>(g_lookupCall.Original());
    if (g_currentPass != 1 || manager == nullptr)
    {
        return original(manager, point, scale, character);  // player 1's view and the game's tick: its own key
    }
    Pair two;
    player_two::lock_on::Displayed(two.object, two.controller);
    const auto key   = reinterpret_cast<std::uintptr_t>(manager) + kTargetedKey;
    const Pair theirs{ReadAt<std::uintptr_t>(key), ReadAt<std::uintptr_t>(key + 8)};
    // A borrowed swap: the lookup pins the key with its own reference and releases it again.
    if (!platform::WriteGuarded(reinterpret_cast<void*>(key), &two, sizeof(two)))
    {
        if (!platform::WriteGuarded(reinterpret_cast<void*>(key), &theirs, sizeof(theirs)))  // put back what was written
        {
            g_failed = true;
            Note(L"putting player 1's targeted key back raised an exception; per-view marks are off this session");
        }
        return original(manager, point, scale, character);
    }
    std::uint8_t found = 0;
    const bool   ok    = CallGuarded(original, found, manager, point, scale, character);
    if (Pair{ReadAt<std::uintptr_t>(key), ReadAt<std::uintptr_t>(key + 8)} == two &&
        !platform::WriteGuarded(reinterpret_cast<void*>(key), &theirs, sizeof(theirs)))
    {
        g_failed = true;  // the per-view marks stop; the game rewrites its key next frame
        Note(L"putting player 1's targeted key back raised an exception; per-view marks are off this session");
    }
    // Each character's label is logged on its own change: one lookup runs per label each frame. A Debug line; one
    // whose lookup raised an exception is Info.
    const platform::Level level = ok ? platform::Level::Debug : platform::Level::Info;
    if (!platform::Enabled(level))
    {
        return found;
    }
    const LabelLog now{reinterpret_cast<std::uintptr_t>(character), two.object, found != 0, ok};
    auto logged = std::find_if(g_loggedLabels.begin(), g_loggedLabels.end(),
                               [&now](const LabelLog& label) { return label.character == now.character; });
    if (logged == g_loggedLabels.end())
    {
        logged = g_loggedLabels.begin() + static_cast<std::ptrdiff_t>(g_nextLabel++ % g_loggedLabels.size());
        *logged = {};
    }
    if (!(now == *logged))
    {
        *logged = now;
        try
        {
            Note(level, std::format(L"player 2's view: a red label for {:#x} {} player 2's locked-on gauge ({:#x}){}", now.character,
                                    now.found ? L"makes way for" : L"has no", now.key, now.ok ? L"" : L"; the lookup RAISED an exception"));
        }
        catch (...)
        {
        }
    }
    return found;
}

/// Once per frame: the gauges each pass takes out (the other player's target's only, and not in
/// both players' games).
void FindOtherGauges(std::uintptr_t system)
{
    g_gaugesFound = true;
    g_entryCount  = 0;
    g_otherGauges[0].clear();
    g_otherGauges[1].clear();
    const std::uintptr_t manager = ReadAt<std::uintptr_t>(g_endUISystem + kManager);
    if (manager == 0 || system == 0)
    {
        return;
    }
    Pair two;
    player_two::lock_on::Displayed(two.object, two.controller);
    const auto one  =
        MainOf(Pair{ReadAt<std::uintptr_t>(system + kDisplayed), ReadAt<std::uintptr_t>(system + kDisplayed + 8)});
    const auto mine = MainOf(two);
    if (g_failed)
    {
        return;
    }
    platform::ForEachSparseElement(manager + kEntries, kElement, [&](std::uintptr_t entry) {
        ++g_entryCount;
        const auto source = ReadAt<std::uintptr_t>(entry);
        const int  pass   = hud_rules::OtherGaugePass(Contains(one, source), Contains(mine, source));
        if (pass >= 0 && !Shared(source, manager))
        {
            // Player 2's pass takes out player 1's only; player 1's pass player 2's only.
            g_otherGauges[pass].push_back(ReadAt<std::uint64_t>(entry + kEntryWidget));
        }
        return true;
    });
}

bool InstallSites(const platform::GameImage& image) noexcept;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    const std::uintptr_t collect = find(kCollect);
    g_mainTarget = reinterpret_cast<MainTargetFn>(find(kMainTarget));
    g_release    = reinterpret_cast<ReleaseFn>(find(kRelease));
    g_enemyAi    = reinterpret_cast<EnemyAiFn>(find(kEnemyAi));
    const auto system  = image.Rebase(kBattleSystem, sizeof(void*));
    const auto ui      = image.Rebase(kEndUISystem, kManager + sizeof(void*));
    g_installed =
        g_mainTarget && g_release && g_enemyAi && player_two::weak_object::Known() && system && ui &&
        widget_opacity::Install(image);
    g_battleSystem = system ? *system : 0;
    g_endUISystem  = ui ? *ui : 0;
    g_collect = collect;
    // The label lookup's call and both collection calls cross a 16-byte block in 1.005: they are placed from the
    // game thread, between its ticks (platform/deferred_sites.h).
    const bool placed = g_placement.Install(image, &InstallSites);
    if (g_placement.FromGameThread())
    {
        platform::Log(L"target views: {}; the label lookup and collection calls {}",
                      g_installed ? L"per-view marks ready" : L"UNAVAILABLE",
                      placed ? L"are placed from the game thread (1.005: they cross a 16-byte block)"
                             : L"are NOT placed (no room to place them from the game thread)");
    }
    return g_installed;
}

namespace {

/// The label lookup's call and both collection calls, then the install line. At install on the research build; on
/// 1.005 from the game thread.
bool InstallSites(const platform::GameImage& image) noexcept
{
    const std::uintptr_t collect = g_collect;
    const auto find = [&image](const platform::FunctionFact& fact) {
        const auto live = platform::FindFunction(image, fact);
        return live ? *live : std::uintptr_t{0};
    };
    const auto place = [](platform::CallRedirect& call, std::uintptr_t site, std::uintptr_t target, auto* with) {
        return g_placement.Place(call, site, target, reinterpret_cast<std::uintptr_t>(with));
    };
    const auto create  = image.Rebase(kCreateCall, 5);
    const auto sync    = image.Rebase(kSyncCall, 5);
    const std::uintptr_t lookup     = find(kLookup);
    const auto           lookupSite = image.Rebase(kLookupCall, 5);
    const bool           labels     = g_installed && lookup != 0 && lookupSite &&
                            place(g_lookupCall, *lookupSite, lookup, &LabelLookup);
    // Both collection sites or neither: the keeping step removes what only the creating step names.
    g_unionOn = g_installed && collect != 0 && create && sync && place(g_createCall, *create, collect, &Collect);
    if (g_unionOn && !place(g_syncCall, *sync, collect, &Collect))
    {
        if (!g_createCall.Restore())
        {
            platform::Log(L"target views: the creating collection call could not be put back; it stays (it passes through)");
        }
        g_unionOn = false;
    }
    platform::Log(L"target views: {}; player 2's enemy gauges {}; red ability labels {}",
                  g_installed ? L"per-view marks ready" : L"UNAVAILABLE",
                  g_unionOn ? L"collected with player 1's" : L"NOT collected (a check failed)",
                  labels ? L"make way for each player's own locked-on gauge" : L"follow player 1's lock-on (a check failed)");
    return g_installed;
}

}  // namespace

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    g_hidden.clear();  // no widget calls here: this may run off the game thread
    g_unionOn = false;
    for (auto* call : {&g_lookupCall, &g_syncCall, &g_createCall})
    {
        if (call->IsInstalled() && !call->Restore())
        {
            platform::Log(L"target views: a collection call was changed after us; left in place (it passes through)");
        }
    }
    g_installed = false;
}

void BeginFrame() noexcept
{
    g_lookupCount = 0;
    g_gaugesFound = false;
}

void BeforePass(int player) noexcept
{
    AfterPass();
    if (!g_installed || g_failed || !player_two::generation::OnGameThread() || !player_two::generation::Active())
    {
        return;
    }
    try
    {
        if (const auto openings = player_two::lock_on::TargetListOpenings(); openings != g_listOpenings)
        {
            g_listOpenings = openings;  // player 2's target list opened: the hides are logged afresh
            g_loggedHides  = {};
        }
        const std::uintptr_t system  = ReadAt<std::uintptr_t>(g_battleSystem);
        const auto           oneCur  = ReadAt<std::uintptr_t>(system != 0 ? system + kCursor : 0);
        const auto           twoCur  = reinterpret_cast<std::uintptr_t>(player_two::lock_on::Cursor());
        PassHides            hides;
        hides.hiddenCursor = player != 0 ? oneCur : twoCur;
        hides.ownCursor    = player != 0 ? twoCur : oneCur;
        hides.list         = player_two::lock_on::TargetListOpen();
        hides.cursor       = Hide(reinterpret_cast<void*>(hides.hiddenCursor));
        if (!g_gaugesFound)
        {
            FindOtherGauges(system);
        }
        hides.entries = g_entryCount;
        for (const std::uint64_t weak : g_otherGauges[player != 0 ? 1 : 0])
        {
            hides.gauges += Hide(WidgetOf(weak)) ? 1 : 0;
        }
        g_currentPass = player != 0 ? 1 : 0;  // the label lookup in this view's updates skips those gauges
        const std::size_t pass = player != 0 ? 1 : 0;
        if (hides != g_loggedHides[pass] && platform::Enabled(platform::Level::Trace))
        {
            g_loggedHides[pass] = hides;
            Note(platform::Level::Trace, std::format(L"{}'s pass takes out {} ({:#x}; its own {:#x}) and {} of {} gauge(s){}",
                             player != 0 ? L"player 2" : L"player 1",
                             hides.cursor ? L"the other player's cursor" : L"no cursor", hides.hiddenCursor, hides.ownCursor,
                             hides.gauges, hides.entries, hides.list ? L" | player 2's target list open" : L""));
        }
    }
    catch (...)
    {
    }
}
void AfterPass() noexcept
{
    g_currentPass = -1;
    if (g_hidden.empty())
    {
        return;
    }
    for (const Hidden& hidden : g_hidden)
    {
        // Only while it is still transparent: an opacity the game set since is its own.
        void* widget = player_two::weak_object::ObjectOf(hidden.weak);
        if (widget != nullptr && widget_opacity::Get(widget) == 0.0f)
        {
            widget_opacity::Set(widget, hidden.previous);
        }
    }
    g_hidden.clear();
}

std::array<std::uintptr_t, 2> TargetAndMain(std::uintptr_t object, std::uintptr_t controller) noexcept
{
    if (!g_installed)
    {
        return {};
    }
    try
    {
        return MainOf(Pair{object, controller});
    }
    catch (...)
    {
        return {};
    }
}

}  // namespace lss::hud::target_views
