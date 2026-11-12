#include "hud/battle/target_views.h"

#include "battle/lock_on.h"
#include "facts/engine.h"
#include "facts/hud.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "hud/hud_rules.h"
#include "hud/passes/pass_opacity.h"
#include "platform/build_value.h"
#include "platform/call_redirect.h"
#include "platform/deferred_sites.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/sparse_array.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "players/widget_opacity.h"

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

// Facts read from the database (research/RE-p2-hud-targets.md; this build).
constexpr std::uint8_t kCollectBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
                                          0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xF8,
                                          0xE7, 0xFF, 0xFF, 0xB8, 0xE0, 0x18, 0x00, 0x00, 0xE8, 0x6F, 0x2D, 0x2F, 0x01};
constexpr std::uint8_t kMainTargetBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                             0x41, 0x56, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0xAC, 0xA0, 0xA9, 0x00};
constexpr std::uint8_t kEnemyAiBytes[]    = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8, 0x68, 0x23, 0x2F, 0x01};

constexpr platform::FunctionFact kCollect{L"EndUI__CollectLockOnTargets", 0x140DBC198, kCollectBytes};
constexpr platform::FunctionFact kMainTarget{L"EndUI__GetMainTargetOfLockOnTarget", 0x141614E6C, kMainTargetBytes};
constexpr platform::FunctionFact kEnemyAi{L"EndCharacter_GetEnemyAIComponent", 0x140DBCBBC, kEnemyAiBytes};

// The red ability label's lookup of its enemy's gauge: the entry for the character whose source is
// the targeted key and whose widget is not Collapsed (EndUI__MF8_FindStatusScreenPointForCharacter;
// its one call, in the ability-name tick; research/RE-p2-battle-popups.md).
constexpr std::uint8_t kLookupBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x4C, 0x89, 0x48, 0x20, 0x4C,
                                         0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};
constexpr platform::FunctionFact kLookup{L"EndUI__MF8_FindStatusScreenPointForCharacter", 0x140A68C84, kLookupBytes};
constexpr std::uintptr_t         kLookupCall = 0x140A68826;
constexpr std::uint32_t          kTargetedKey = 0x140;  // the bar manager's targeted key, a TWeakPtr {object, controller}
// The gauge manager's two calls to the collection: creating entries for new sources, and keeping only
// collected ones.
constexpr std::uintptr_t kCreateCall = 0x140A66B85;
constexpr std::uintptr_t kSyncCall   = 0x140B20F39;

// The battle system's displayed target and cursor (player 1's outside every scope).
using facts::kBattleStateGlobal;
constexpr std::uint32_t  kCursor       = 0x68;
constexpr std::uint32_t  kDisplayed    = 0x70;

// The gauge manager (EndUISystem[31]): its entries (a TSet, elements of 0x180 bytes: +0 source, +0x10
// the weak widget) and the sticky list (+0xB0 {object, controller} pairs, +0xB8 count).
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

/// A seat's label lookup, for the log.
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
    std::int32_t   cursors      = 0;  // the other players' cursors this pass takes out
    std::uintptr_t ownCursor    = 0;  // this pass's own player's cursor
    std::int32_t   entries      = 0;  // the bar manager's gauges this frame
    bool           list         = false;  // a seat's command target list open
    bool operator==(const PassHides&) const = default;
};

// Game thread.
using Seats = std::array<int, players::kMaxPlayers>;
static_assert(players::kMaxPlayers == 3, "the logged counts below start with one -1 per player");

HiddenSet                   g_hidden;  // the other players' marks, transparent for a pass
std::array<MainLookup, 8>   g_lookups{};      // this frame's main-target lookups (BeginFrame empties them)
std::size_t                 g_lookupCount = 0;
bool                        g_gaugesFound = false;  // this frame's walk done
// Per pass, by its seat ([0] player 1's): the gauges another player targets and this pass's does not, as their
// widgets' FWeakObjectPtr (resolved at use).
std::array<std::vector<std::uint64_t>, players::kMaxPlayers> g_otherGauges;
int                         g_currentPass = -1;  // the seat whose view's HUD updates and render run, else -1
bool                        g_failed = false;
std::array<PassHides, players::kMaxPlayers> g_loggedHides{};
std::array<LabelLog, 8>     g_loggedLabels{};  // per character, as last logged
std::uint32_t               g_nextLabel = 0;   // the slot a newly seen character takes
Seats                       g_loggedUnion{-1, -1, -1};  // the collection's count before the seats' runs, then after each
std::uint32_t               g_listOpenings = 0;  // the seats' target list openings seen: each logs the hides afresh
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

/// Replaces both collection calls: the game's collection, then again with each seat's target fields (in turn, one
/// TargetScope at a time). Each run only appends what is missing.
void Collect(void* out) noexcept
{
    const auto original = reinterpret_cast<CollectFn>(g_createCall.Original());
    original(out);
    if (!g_unionOn)
    {
        return;
    }
    Seats counts{Count(out), -1, -1};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat) || players::generation::MemberOf(seat) == nullptr ||
            battle::lock_on::DisplayedObject(seat) == 0)
        {
            continue;
        }
        const battle::lock_on::TargetScope scope(seat);
        if (scope.Held())
        {
            original(out);
            counts[static_cast<std::size_t>(seat)] = Count(out);
        }
    }
    if (counts == Seats{counts[0], -1, -1} || !platform::Enabled(platform::Level::Debug) || counts == g_loggedUnion)
    {
        return;
    }
    g_loggedUnion = counts;
    try
    {
        std::wstring seats;
        for (int seat = 1; seat < players::kMaxPlayers; ++seat)
        {
            const int count = counts[static_cast<std::size_t>(seat)];
            seats += count < 0 ? L"" : std::format(L", {} with {}'s target", count, players::SeatName(seat));
        }
        Note(platform::Level::Debug, std::format(L"collected {} for player 1{}", counts[0], seats));
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
    return players::weak_object::ObjectOf(weak);
}

/// Takes `widget` out of this pass's image (render opacity 0, so it stays painted and ticked),
/// remembering its own opacity. True if it did.
bool Hide(void* widget) noexcept
{
    return g_hidden.Hide(widget);
}

/// Replaces the red ability label's gauge lookup. The lookup considers only the gauge of the bar
/// manager's targeted key (+0x140/+0x148, the locked-on target, player 1's; read locally
/// twice): the label makes way only for that gauge. During a seat's
/// updates the key holds the seat's displayed target for the length of this call, and player 1's
/// is put back after, so the seat's label makes way for the seat's locked-on gauge only.
std::uint8_t LabelLookup(void* manager, float* point, float* scale, void* character) noexcept
{
    const auto original = reinterpret_cast<LookupFn>(g_lookupCall.Original());
    const int  seat     = g_currentPass;
    if (!players::SeatIndex(seat) || manager == nullptr)
    {
        return original(manager, point, scale, character);  // player 1's view and the game's tick: its own key
    }
    Pair displayed;
    battle::lock_on::Displayed(seat, displayed.object, displayed.controller);
    const auto key   = reinterpret_cast<std::uintptr_t>(manager) + kTargetedKey;
    const Pair theirs{ReadAt<std::uintptr_t>(key), ReadAt<std::uintptr_t>(key + 8)};
    // A borrowed swap: the lookup pins the key with its own reference and releases it again.
    if (!platform::WriteGuarded(reinterpret_cast<void*>(key), &displayed, sizeof(displayed)))
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
    if (Pair{ReadAt<std::uintptr_t>(key), ReadAt<std::uintptr_t>(key + 8)} == displayed &&
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
    const LabelLog now{reinterpret_cast<std::uintptr_t>(character), displayed.object, found != 0, ok};
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
            Note(level, std::format(L"{}'s view: a red label for {:#x} {} their locked-on gauge ({:#x}){}",
                                    players::SeatName(seat), now.character, now.found ? L"makes way for" : L"has no",
                                    now.key, now.ok ? L"" : L"; the lookup RAISED an exception"));
        }
        catch (...)
        {
        }
    }
    return found;
}

/// Once per frame: the gauges each pass takes out (another player's target's, not its own player's, and not in
/// every player's game).
void FindOtherGauges(std::uintptr_t system)
{
    g_gaugesFound = true;
    g_entryCount  = 0;
    for (auto& gauges : g_otherGauges)
    {
        gauges.clear();
    }
    const std::uintptr_t manager = ReadAt<std::uintptr_t>(g_endUISystem + facts::kUIEnemyStatus);
    if (manager == 0 || system == 0)
    {
        return;
    }
    // Each player's displayed target and its main target ([0] player 1's, the battle system's own).
    std::array<std::array<std::uintptr_t, 2>, players::kMaxPlayers> targets{};
    targets[0] = MainOf(Pair{ReadAt<std::uintptr_t>(system + kDisplayed), ReadAt<std::uintptr_t>(system + kDisplayed + 8)});
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        Pair displayed;
        battle::lock_on::Displayed(seat, displayed.object, displayed.controller);
        targets[static_cast<std::size_t>(seat)] = MainOf(displayed);
    }
    if (g_failed)
    {
        return;
    }
    platform::ForEachSparseElement(manager + kEntries, kElement, [&](std::uintptr_t entry) {
        ++g_entryCount;
        const auto source  = ReadAt<std::uintptr_t>(entry);
        unsigned   holders = 0;
        for (std::size_t s = 0; s < targets.size(); ++s)
        {
            holders |= Contains(targets[s], source) ? 1u << s : 0u;
        }
        bool hiddenSomewhere = false;
        for (int pass = 0; pass < players::kMaxPlayers; ++pass)
        {
            hiddenSomewhere = hiddenSomewhere || hud_rules::GaugeHiddenIn(holders, pass);
        }
        if (!hiddenSomewhere || Shared(source, manager))  // the game's calls only for a gauge some view would lose
        {
            return true;
        }
        for (int pass = 0; pass < players::kMaxPlayers; ++pass)
        {
            if (hud_rules::GaugeHiddenIn(holders, pass))  // another player's target, not this view's player's
            {
                g_otherGauges[static_cast<std::size_t>(pass)].push_back(ReadAt<std::uint64_t>(entry + kEntryWidget));
            }
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
    g_release    = reinterpret_cast<ReleaseFn>(find(facts::kWeakPtrRelease));
    g_enemyAi    = reinterpret_cast<EnemyAiFn>(find(kEnemyAi));
    const auto system  = image.Rebase(kBattleStateGlobal, sizeof(void*));
    const auto ui      = image.Rebase(facts::kEndUISystem, facts::kUIEnemyStatus + sizeof(void*));
    g_installed =
        g_mainTarget && g_release && g_enemyAi && players::weak_object::Known() && system &&
        facts::InBattleKnown() && ui &&
        players::widget_opacity::Installed();  // a row of the module table before this one
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
    platform::Log(L"target views: {}; players 2 and 3's enemy gauges {}; red ability labels {}",
                  g_installed ? L"per-view marks ready" : L"UNAVAILABLE",
                  g_unionOn ? L"collected with player 1's" : L"NOT collected (a check failed)",
                  labels ? L"make way for each player's own locked-on gauge" : L"follow player 1's lock-on (a check failed)");
    return g_installed;
}

}  // namespace

void Remove() noexcept
{
    g_placement.Cancel();  // a placement still queued does nothing
    g_hidden.Forget();  // no widget calls here: this may run off the game thread
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

void BeforePass(int seat) noexcept
{
    AfterPass();
    if (!g_installed || g_failed || !players::generation::OnGameThread() || !players::generation::AnyActive() ||
        seat < 0 || seat >= players::kMaxPlayers)
    {
        return;
    }
    try
    {
        std::uint32_t openings = 0;
        bool          list     = false;
        for (int s = 1; s <= players::kSeatsBuilt; ++s)
        {
            openings += battle::lock_on::TargetListOpenings(s);
            list = list || battle::lock_on::TargetListOpen(s);
        }
        if (openings != g_listOpenings)
        {
            g_listOpenings = openings;  // a seat's target list opened: the hides are logged afresh
            g_loggedHides  = {};
        }
        const std::uintptr_t system = ReadAt<std::uintptr_t>(g_battleSystem);
        // Each player's cursor ([0] player 1's, the battle system's own): every one but this view's player's goes.
        std::array<std::uintptr_t, players::kMaxPlayers> cursors{};
        cursors[0] = ReadAt<std::uintptr_t>(system != 0 ? system + kCursor : 0);
        for (int s = 1; s <= players::kSeatsBuilt; ++s)
        {
            cursors[static_cast<std::size_t>(s)] = reinterpret_cast<std::uintptr_t>(battle::lock_on::Cursor(s));
        }
        PassHides hides;
        hides.ownCursor = cursors[static_cast<std::size_t>(seat)];
        hides.list      = list;
        for (std::size_t s = 0; s < cursors.size(); ++s)
        {
            if (static_cast<int>(s) != seat)
            {
                hides.cursors += Hide(reinterpret_cast<void*>(cursors[s])) ? 1 : 0;
            }
        }
        if (!g_gaugesFound)
        {
            FindOtherGauges(system);
        }
        hides.entries = g_entryCount;
        for (const std::uint64_t weak : g_otherGauges[static_cast<std::size_t>(seat)])
        {
            hides.gauges += Hide(WidgetOf(weak)) ? 1 : 0;
        }
        g_currentPass = seat;  // the label lookup in this view's updates skips those gauges
        auto& logged  = g_loggedHides[static_cast<std::size_t>(seat)];
        if (hides != logged && platform::Enabled(platform::Level::Trace))
        {
            logged = hides;
            Note(platform::Level::Trace,
                 std::format(L"{}'s pass takes out {} other player's cursor(s) (its own {:#x}) and {} of {} gauge(s){}",
                             players::SeatName(seat), hides.cursors, hides.ownCursor, hides.gauges, hides.entries,
                             hides.list ? L" | player 2's or 3's target list open" : L""));
        }
    }
    catch (...)
    {
    }
}
void AfterPass() noexcept
{
    g_currentPass = -1;
    g_hidden.RestoreAll();  // each mark while it is still transparent (an opacity the game set since is its own)
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
