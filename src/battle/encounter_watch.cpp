#include "battle/encounter_watch.h"

#include "facts/party.h"
#include "facts/world.h"
#include "platform/call_redirect.h"
#include "platform/command_registry.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/describe.h"
#include "players/game_globals.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include <Windows.h>

namespace lss::battle::encounter_watch {

namespace {

using LeaderFn      = void* (*)();
using FieldLeaderFn = void* (*)(bool fieldLeader);
using EnterBattleFn = void (*)(void* manager, const void* engaged, void* character);
using ContainsFn    = bool (*)(const void* territory, const float* point, float flatDistanceSquared);
using CharacterFn   = bool (*)(const void* character);

constexpr int           kPlayers    = players::kMaxPlayers;
constexpr std::int32_t  kMaxEntries = 512;    // more than any field holds; a larger count is a misread
constexpr std::uint64_t kPolicyMs   = 250;    // the watch rule asked at most this often
constexpr std::uint64_t kStickMs    = 2000;   // a rule's answer kept this long before another player takes it
constexpr float         kCloserM    = 3.0f;   // and only by being this much nearer an enemy
constexpr float         kNearM      = 15.0f;  // a player 2 or 3 this close to a field enemy is logged
constexpr float         kFarM       = 25.0f;  // and again only after going this far from it
constexpr std::uint8_t  kUnread     = 0xFF;

// Set at install, then only read.
ContainsFn            g_contains = nullptr;
CharacterFn           g_inside   = nullptr;  // inside any fight's area (null: not found, not logged)

platform::CallRedirect g_subjectCall, g_enterBattleCall, g_anchorCall;

/// A player's character this tick and where it stands (placed: its location read).
struct Player
{
    const void* character = nullptr;
    float       at[3]     = {};
    bool        placed    = false;
};

/// One player against one field enemy: the flat distance (metres) and the game's territory test.
struct Reach
{
    float metres = 0.0f;
    bool  inside = false;
    bool  read   = false;  // a territory to test against
};

/// A field enemy's record as last seen at the tick's read: its bytes, and which players 2 and 3 were logged near it.
struct Entry
{
    const void*   entry = nullptr;
    std::uint64_t name  = 0;
    std::uint8_t  force[3]{kUnread, kUnread, kUnread};
    std::uint8_t  state[2]{kUnread, kUnread};
    unsigned      closeBy = 0;  // bit p: player p was logged near it and has not gone kFarM away since
};

// Game thread (the encounter tick and the console's reading, which runs there too).
std::vector<Entry>       g_entries, g_next;
std::vector<const void*> g_list;  // this tick's field enemies, the vector reused
std::uint8_t             g_state = kUnread, g_prompt = kUnread;
int                      g_rulePlayer = 0;  // the player the watch rule would hold (0: player 1)
std::uint64_t            g_ruleEntry = 0;   // the field enemy (its key) the rule's player stands nearest
std::uint64_t            g_ruleAt = 0, g_ruleTook = 0;
std::uint32_t            g_reads = 0, g_starts = 0;
int                      g_fightBy = 0;  // the player 2 or 3 the starting fight noticed (0: player 1), until the field again

// The option: field enemies notice players 2 and 3 (the console and the page set it; the tick reads it).
std::atomic<bool> g_noticeOn{false};

const void* Manager() noexcept
{
    return players::actor_fields::RootEncounter(players::game_globals::Object(players::game_globals::Global::GameRoot));
}

const wchar_t* StateName(std::uint8_t state) noexcept
{
    switch (state)
    {
        case 1: return L"field";
        case 2: return L"caution";
        case 3: return L"battle";
        case 4: return L"escaping";
        case kUnread: return L"unread";
        default: return L"?";
    }
}

std::wstring EntryName(std::uint64_t name)
{
    return name != 0 ? players::unreal::NameText(name) : std::wstring(L"(unnamed)");
}

/// Every player's character: player 1's the game's watched one (`one`), players 2 and 3's while they play.
std::array<Player, kPlayers> Players(const void* one) noexcept
{
    std::array<Player, kPlayers> all{};
    for (int p = 0; p < kPlayers; ++p)
    {
        const void* character = p == 0                                              ? one
                                : players::SeatBuilt(p) && players::generation::Active(p) ? players::generation::MemberOf(p)
                                                                                         : nullptr;
        all[p].character = character;
        all[p].placed    = character != nullptr && players::unreal::ActorLocation(character, all[p].at);
    }
    return all;
}

/// The field enemies' records as the tick holds them (last tick's list at the watched character's read), in g_list.
const std::vector<const void*>& Entries(const void* manager)
{
    std::vector<const void*>& out = g_list;
    out.clear();
    const auto count = ReadField<std::int32_t>(manager, facts::kEncounterEntryCount);
    const auto* heap = static_cast<const std::uint8_t*>(ReadPointer(manager, facts::kEncounterEntriesHeap));
    if (manager == nullptr || count <= 0 || count > kMaxEntries || (heap == nullptr && count > facts::kEncounterEntriesInlineMax))
    {
        return out;
    }
    const auto* data = heap != nullptr ? heap : static_cast<const std::uint8_t*>(manager) + facts::kEncounterEntriesInline;
    out.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i)
    {
        if (const void* entry = ReadPointer(data, 8 * static_cast<std::uint32_t>(i)); entry != nullptr)
        {
            out.push_back(entry);
        }
    }
    return out;
}

/// `player` against `entry`: the flat distance to its enemy, and with `territory` the game's territory test as the tick
/// makes it (the point against the territory, with its flat distance from the territory's root).
Reach ReachOf(const void* entry, const Player& player, bool territory = true) noexcept
{
    Reach reach;
    float enemyAt[3] = {};
    const void* enemy = players::weak_object::ObjectIn(entry, facts::kEncounterEntryEnemy);
    if (!player.placed || enemy == nullptr || !players::unreal::ActorLocation(enemy, enemyAt))
    {
        return reach;
    }
    reach.metres = std::hypot(player.at[0] - enemyAt[0], player.at[1] - enemyAt[1]) / 100.0f;
    float       rootAt[3] = {};
    const void* area      = territory ? players::weak_object::ObjectIn(entry, facts::kEncounterEntryTerritory) : nullptr;
    if (area == nullptr || g_contains == nullptr || !players::unreal::ActorLocation(area, rootAt))
    {
        return reach;
    }
    alignas(16) const float point[4] = {player.at[0], player.at[1], player.at[2], 0.0f};  // padded for any vector load
    const float             dx = rootAt[0] - player.at[0], dy = rootAt[1] - player.at[1];
    reach.read = platform::CallGuarded(g_contains, reach.inside, area, static_cast<const float*>(point), dx * dx + dy * dy);
    return reach;
}

std::wstring ReachText(const Reach& reach)
{
    return reach.metres == 0.0f ? std::wstring(L"unplaced")
                                : std::format(L"{:.1f} m{}", reach.metres,
                                              !reach.read ? L"" : reach.inside ? L", inside its territory" : L", outside its territory");
}

/// Each player's nearest field enemy, for a state line.
std::wstring NearestText(const std::vector<const void*>& entries, const std::array<Player, kPlayers>& all)
{
    std::wstring text;
    for (int p = 0; p < kPlayers; ++p)
    {
        if (all[p].character == nullptr)
        {
            continue;
        }
        Reach         best;
        std::uint64_t bestName = 0;
        for (const void* entry : entries)
        {
            const Reach reach = ReachOf(entry, all[p]);
            if (reach.metres > 0.0f && (best.metres == 0.0f || reach.metres < best.metres))
            {
                best     = reach;
                bestName = ReadField<std::uint64_t>(entry, facts::kEncounterEntryName);
            }
        }
        text += std::format(L"{}{}: {}", text.empty() ? L"" : L"; ", players::SeatName(p),
                            best.metres == 0.0f ? std::wstring(L"no field enemy placed")
                                                : std::format(L"{} {}", EntryName(bestName), ReachText(best)));
    }
    return text;
}

/// The watch rule (what enemies noticing players 2 and 3 would do): in the field state, the player who stands inside a
/// field enemy's territory nearest that enemy, kept until another is clearly nearer; nobody inside, player 1. Logged
/// at Debug when its answer changes.
void Rule(const std::vector<const void*>& entries, const std::array<Player, kPlayers>& all, std::uint64_t now)
{
    if (now - g_ruleAt < kPolicyMs)
    {
        return;
    }
    g_ruleAt          = now;
    int           pick = 0;
    float         best = 0.0f;
    std::uint64_t name = 0;
    std::array<Reach, kPlayers> held{};
    for (const void* entry : entries)
    {
        for (int p = 0; p < kPlayers; ++p)
        {
            const Reach reach = ReachOf(entry, all[p]);
            if (reach.inside && (held[p].metres == 0.0f || reach.metres < held[p].metres))
            {
                held[p] = reach;
            }
            if (reach.inside && (best == 0.0f || reach.metres < best))
            {
                pick = p;
                best = reach.metres;
                name = ReadField<std::uint64_t>(entry, facts::kEncounterEntryName);
            }
        }
    }
    const Reach& current = held[g_rulePlayer];
    const bool   keep    = current.inside && (best == 0.0f || current.metres - best < kCloserM || now - g_ruleTook < kStickMs);
    if (keep || pick == g_rulePlayer)
    {
        return;
    }
    const int was = g_rulePlayer;
    g_rulePlayer  = pick;
    g_ruleEntry   = name;
    g_ruleTook    = now;
    const bool         on   = g_noticeOn.load(std::memory_order_relaxed);
    const std::wstring line = std::format(
        L"encounter watch: {} {} ({}; {} {}){}", on ? L"field enemies now watch" : L"the rule would now watch", players::SeatName(pick),
        best == 0.0f ? std::wstring(L"nobody stands in a field enemy's territory")
                     : std::format(L"inside {}'s territory, {:.1f} m from it", EntryName(name), best),
        players::SeatName(was), held[was].inside ? std::format(L"{:.1f} m", held[was].metres) : std::wstring(L"outside"),
        on ? L"" : L"; the game watches player 1 (enemies-notice off)");
    on ? platform::Log(L"{}", line) : platform::LogDebug(L"{}", line);
}

/// Each field enemy's force bytes and perception states against last tick's, and players 2 and 3 coming near one.
void Diff(const std::vector<const void*>& entries, const std::array<Player, kPlayers>& all)
{
    g_next.clear();
    for (const void* entry : entries)
    {
        Entry seen{entry, ReadField<std::uint64_t>(entry, facts::kEncounterEntryName)};
        for (int i = 0; i < 3; ++i)
        {
            seen.force[i] = ReadField<std::uint8_t>(entry, facts::kEncounterEntryForce + static_cast<std::uint32_t>(i));
        }
        seen.state[0]   = ReadField<std::uint8_t>(entry, facts::kEncounterEntryState);
        seen.state[1]   = ReadField<std::uint8_t>(entry, facts::kEncounterEntryState + 1);
        // The list keeps its order from tick to tick: the same place first, then a search.
        const std::size_t at  = g_next.size();
        const Entry*      was = at < g_entries.size() && g_entries[at].entry == entry ? &g_entries[at] : nullptr;
        for (std::size_t i = 0; was == nullptr && i < g_entries.size(); ++i)
        {
            was = g_entries[i].entry == entry ? &g_entries[i] : nullptr;
        }
        if (was == nullptr)
        {
            platform::LogDebug(L"encounter watch: field enemy {} seen (force {},{},{}; states {},{})", EntryName(seen.name), seen.force[0],
                               seen.force[1], seen.force[2], seen.state[0], seen.state[1]);
        }
        else
        {
            seen.closeBy = was->closeBy;
            if (std::memcmp(seen.force, was->force, sizeof(seen.force)) != 0)
            {
                platform::Log(L"encounter watch: field enemy {}'s force bytes {},{},{} -> {},{},{} (states {},{}; the prompt {})",
                              EntryName(seen.name), was->force[0], was->force[1], was->force[2], seen.force[0], seen.force[1],
                              seen.force[2], seen.state[0], seen.state[1], g_prompt == 1 ? L"shown" : L"hidden");
            }
            if (std::memcmp(seen.state, was->state, sizeof(seen.state)) != 0)
            {
                platform::LogDebug(L"encounter watch: field enemy {}'s perception states {},{} -> {},{}", EntryName(seen.name),
                                   was->state[0], was->state[1], seen.state[0], seen.state[1]);
            }
        }
        // Players 2 and 3 only while they play (no character: nothing read); the distance alone, the territory test only
        // for the line.
        for (int p = 1; p < kPlayers; ++p)
        {
            const unsigned bit   = 1u << p;
            const float    metres = all[p].character != nullptr ? ReachOf(entry, all[p], false).metres : 0.0f;
            if (metres > 0.0f && metres <= kNearM && (seen.closeBy & bit) == 0)
            {
                seen.closeBy |= bit;
                platform::Log(L"encounter watch: {} is near field enemy {} ({}; player 1 {}); the game watches player 1",
                              players::SeatName(p), EntryName(seen.name), ReachText(ReachOf(entry, all[p])),
                              ReachText(ReachOf(entry, all[0])));
            }
            else if ((all[p].character == nullptr || metres > kFarM) && (seen.closeBy & bit) != 0)
            {
                seen.closeBy &= ~bit;  // unplaced (0) changes nothing: a weak that fails one tick does not log it again
            }
        }
        g_next.push_back(seen);
    }
    g_entries.swap(g_next);
}

void Observe(const void* one) noexcept
try
{
    const void* manager = Manager();
    if (manager == nullptr)
    {
        g_entries.clear();  // a record at a reused address after a load is a new field enemy
        g_state      = kUnread;
        g_prompt     = kUnread;
        g_fightBy    = 0;  // no fight goes on without the encounter: a later battle gathers around player 1
        g_rulePlayer = 0;
        return;
    }
    ++g_reads;
    const std::uint64_t now     = GetTickCount64();
    const auto&         entries = Entries(manager);
    const auto          all     = Players(one);
    const auto          state   = ReadField<std::uint8_t>(manager, facts::kEncounterState);
    const auto          prompt  = ReadField<std::uint8_t>(manager, facts::kEncounterPromptShown);
    if (state != g_state)
    {
        const std::wstring escape = state == facts::kEncounterEscaping
                                        ? std::format(L", outside every fight area for {:.2f} s",
                                                      ReadField<float>(manager, facts::kEncounterEscapeTimer))
                                        : std::wstring();
        const bool noticed = g_noticeOn.load(std::memory_order_relaxed) && g_rulePlayer >= 1;
        platform::Log(L"encounter watch: the field encounter {} -> {} (it watches {}{}); {} field enemies; {}", StateName(g_state),
                      StateName(state), noticed ? std::wstring(players::SeatName(g_rulePlayer)) : players::Who(one), escape,
                      entries.size(), NearestText(entries, all));
        g_state = state;
    }
    if (prompt != g_prompt)
    {
        if (g_prompt != kUnread || prompt != 0)
        {
            platform::Log(L"encounter watch: the preemptive-attack prompt {} (the battle-in notice's preemptive byte {}; danger "
                          L"{:.2f}, reach {:.2f})",
                          prompt != 0 ? L"shown" : L"hidden", ReadField<std::uint8_t>(manager, facts::kEncounterPreemptiveNotice),
                          ReadField<float>(manager, facts::kEncounterVigilanceGauge),
                          ReadField<float>(manager, facts::kEncounterVigilanceReach));
        }
        g_prompt = prompt;
    }
    Diff(entries, all);
    if (state == facts::kEncounterField)
    {
        g_fightBy = 0;
        if (g_noticeOn.load(std::memory_order_relaxed) || platform::Enabled(platform::Level::Debug))
        {
            Rule(entries, all, now);  // the watched player changes only in the field state: caution, fight and escape keep it
        }
    }
}
catch (...)
{
}

/// Player 1's character: the game's own answer of the read the tick makes (the party leader).
void* PlayerOne() noexcept
{
    return reinterpret_cast<LeaderFn>(g_subjectCall.Original())();
}

/// With the option on, the character the field enemies watch: the rule's player 2 or 3 while they play; null for player
/// 1's (the game's answer). A watched player who left falls back to player 1, chosen again in the field state.
void* Watched() noexcept
{
    const int player = g_rulePlayer;
    void*     member = player >= 1 && players::SeatBuilt(player) && players::generation::Active(player)
                           ? players::generation::MemberOf(player)
                           : nullptr;
    if (member == nullptr && player != 0 && g_state == facts::kEncounterField)
    {
        g_rulePlayer = 0;
    }
    return member;
}

/// Replaces the tick's read of the watched character: the game's answer, observed; with enemies-notice on, the rule's
/// player 2 or 3 instead while one stands nearest a field enemy inside its territory (kept through caution, fight and
/// escape: the fight follows them and ends when they leave its area).
void* Subject() noexcept
{
    const auto original = reinterpret_cast<LeaderFn>(g_subjectCall.Original());
    void*      one      = original();
    if (!players::generation::OnGameThread())
    {
        return one;
    }
    Observe(one);
    if (!g_noticeOn.load(std::memory_order_relaxed))
    {
        return one;
    }
    void* watched = Watched();
    return watched != nullptr ? watched : one;
}

std::wstring InsideText(const std::array<Player, kPlayers>& all)
{
    std::wstring text;
    for (int p = 0; p < kPlayers; ++p)
    {
        bool in = false;
        if (all[p].character != nullptr && g_inside != nullptr)
        {
            const bool read = platform::CallGuarded(g_inside, in, all[p].character);
            text += std::format(L"{}{} {}", text.empty() ? L"" : L", ", players::SeatName(p), !read ? L"not read" : in ? L"inside" : L"outside");
        }
    }
    return text.empty() ? std::wstring(L"not read") : text;
}

/// Replaces the fight's start: the enemies it starts with, the game's start, then who stands in its area.
void EnterBattle(void* manager, const void* engaged, void* character) noexcept
{
    const auto original = reinterpret_cast<EnterBattleFn>(g_enterBattleCall.Original());
    if (!players::generation::OnGameThread())
    {
        original(manager, engaged, character);
        return;
    }
    std::array<Player, kPlayers> all{};
    // The player 2 or 3 the fight noticed, with the option on (the game's own subject otherwise: player 1's).
    const int subject = character != nullptr ? players::generation::SeatOfMember(character) : -1;
    g_fightBy = g_noticeOn.load(std::memory_order_relaxed) && players::SeatBuilt(subject) && players::generation::Active(subject)
                    ? subject
                    : 0;
    try
    {
        ++g_starts;
        all                 = Players(g_fightBy != 0 ? PlayerOne() : character);
        const auto*  data   = static_cast<const std::uint8_t*>(ReadPointer(engaged, 0));
        const auto   count  = ReadField<std::int32_t>(engaged, 8);
        std::wstring text;
        for (std::int32_t i = 0; data != nullptr && i < count && i < kMaxEntries; ++i)
        {
            const void* entry = ReadPointer(data, 8 * static_cast<std::uint32_t>(i));
            if (entry == nullptr)
            {
                continue;
            }
            text += std::format(L"; {} (force {},{},{}, states {},{}, gauges {:.2f}/{:.2f}, reach {:.1f} m, named area {}:",
                                EntryName(ReadField<std::uint64_t>(entry, facts::kEncounterEntryName)),
                                ReadField<std::uint8_t>(entry, facts::kEncounterEntryForce),
                                ReadField<std::uint8_t>(entry, facts::kEncounterEntryForce + 1),
                                ReadField<std::uint8_t>(entry, facts::kEncounterEntryForce + 2),
                                ReadField<std::uint8_t>(entry, facts::kEncounterEntryState),
                                ReadField<std::uint8_t>(entry, facts::kEncounterEntryState + 1),
                                ReadField<float>(entry, facts::kEncounterEntryGaugeA), ReadField<float>(entry, facts::kEncounterEntryGaugeB),
                                std::sqrt(std::max(ReadField<float>(entry, facts::kEncounterEntryReach), 0.0f)) / 100.0f,
                                ReadField<std::uint8_t>(entry, facts::kEncounterEntryNamedArea));
            for (int p = 0; p < kPlayers; ++p)
            {
                if (all[p].character != nullptr)
                {
                    text += std::format(L" {} {}", players::SeatName(p), ReachText(ReachOf(entry, all[p])));
                }
            }
            text += L")";
        }
        platform::Log(L"encounter watch: a fight starts, watching {}, with {} field enemies{}", players::Who(character), count, text);
    }
    catch (...)
    {
    }
    original(manager, engaged, character);
    try
    {
        platform::Log(L"encounter watch: after the start, in the fight's area by the game's test: {}", InsideText(all));
    }
    catch (...)
    {
    }
}

/// Replaces the battle-in gather's anchor read: the game's answer, or for a fight that noticed player 2 or 3 their
/// character, so the game's own gather places the party around them (the battle-warps setting decides who it moves);
/// each player's distance to it at Debug.
void* Anchor(bool fieldLeader) noexcept
{
    const auto original = reinterpret_cast<FieldLeaderFn>(g_anchorCall.Original());
    void*      anchor   = original(fieldLeader);
    if (!players::generation::OnGameThread())
    {
        return anchor;
    }
    if (g_fightBy >= 1 && players::generation::Active(g_fightBy))
    {
        if (void* member = players::generation::MemberOf(g_fightBy); member != nullptr)
        {
            platform::Log(L"encounter watch: the battle-in gather stands around {} (the fight noticed them; the battle-warps "
                          L"setting decides who it moves)",
                          players::SeatName(g_fightBy));
            anchor = member;
        }
    }
    if (!platform::Enabled(platform::Level::Debug))
    {
        return anchor;
    }
    try
    {
        float at[3] = {};
        const bool placed = anchor != nullptr && players::unreal::ActorLocation(anchor, at);
        std::wstring text;
        for (const Player& player : Players(anchor))
        {
            if (player.character != nullptr && player.character != anchor)
            {
                text += std::format(L"{}{} {}", text.empty() ? L"" : L", ", players::Who(player.character),
                                    placed && player.placed ? std::format(L"{:.1f} m from it", std::hypot(player.at[0] - at[0], player.at[1] - at[1]) / 100.0f)
                                                            : std::wstring(L"unplaced"));
            }
        }
        platform::LogDebug(L"encounter watch: the battle-in gather anchors on {}{}{}", players::Who(anchor), text.empty() ? L"" : L"; ", text);
    }
    catch (...)
    {
    }
    return anchor;
}

#if LSS_DEV
std::wstring EncounterReading()
{
    std::wstring text = std::format(L"{}; state {}; reads {}, fight starts {}; the rule would watch {}{}",
                                    g_subjectCall.IsInstalled() ? L"on" : L"off", StateName(g_state), g_reads, g_starts,
                                    players::SeatName(g_rulePlayer),
                                    g_ruleEntry != 0 ? std::format(L" (nearest {})", EntryName(g_ruleEntry)) : std::wstring());
    for (const Entry& seen : g_entries)
    {
        text += std::format(L"; {} force {},{},{} states {},{}", EntryName(seen.name), seen.force[0], seen.force[1], seen.force[2],
                            seen.state[0], seen.state[1]);
    }
    return text;
}
#endif

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto leader      = platform::FindFunction(image, facts::kResolvePartyLeader);
    const auto fieldLeader = platform::FindFunction(image, facts::kResolveFieldLeader);
    const auto enterBattle = platform::FindFunction(image, facts::kEncounterEnterBattle);
    const auto contains    = platform::FindFunction(image, facts::kTerritoryContainsPoint);
    const auto inside      = platform::FindFunction(image, facts::kInsideAnyBattleArea);
    const auto root        = players::game_globals::Address(players::game_globals::Global::GameRoot);
    if (!leader || !fieldLeader || !enterBattle || root == 0 || !players::weak_object::Known())
    {
        platform::Log(L"encounter watch: an engine check failed; the field encounter is not watched");
        return false;
    }
    g_contains = contains ? reinterpret_cast<ContainsFn>(*contains) : nullptr;
    g_inside   = inside ? reinterpret_cast<CharacterFn>(*inside) : nullptr;
    if (!g_subjectCall.Install(image, facts::kEncounterSubjectCall, *leader, reinterpret_cast<std::uintptr_t>(&Subject)) ||
        !g_enterBattleCall.Install(image, facts::kEncounterEnterBattleCall, *enterBattle,
                                   reinterpret_cast<std::uintptr_t>(&EnterBattle)) ||
        !g_anchorCall.Install(image, facts::kBattleInAnchorCall, *fieldLeader, reinterpret_cast<std::uintptr_t>(&Anchor)))
    {
        Remove();
        platform::Log(L"encounter watch: a call site is not what the research read; the field encounter is not watched");
        return false;
    }
    namespace settings = platform::settings;
    g_noticeOn.store(settings::Of(settings::kEnemiesNotice).word == L"on", std::memory_order_relaxed);
    platform::Log(L"encounter watch: enemies-notice {} ({})", g_noticeOn.load(std::memory_order_relaxed) ? L"on" : L"off",
                  settings::Why(settings::kEnemiesNotice));
    try
    {
        control::RegisterOption(
            settings::kEnemiesNotice,
            [](const std::wstring& word) {
                const bool on = word == L"on";
                if (!on && word != L"off")
                {
                    return control::Outcome{false, L"on or off"};
                }
                // Turned on, the watched player is chosen afresh in the field state (the rule may have run for the log
                // alone): a fight already going on keeps player 1.
                if (on && !g_noticeOn.load(std::memory_order_relaxed))
                {
                    g_rulePlayer = 0;
                }
                g_noticeOn.store(on, std::memory_order_relaxed);
                platform::Log(L"encounter watch: enemies-notice {} (set){}", on ? L"on" : L"off",
                              on ? L": field enemies watch the player nearest one of them inside its territory" : L"");
                return control::Outcome{};
            },
            [] { return std::wstring(g_noticeOn.load(std::memory_order_relaxed) ? L"on" : L"off"); });
    }
    catch (...)
    {
        platform::Log(L"encounter watch: the enemies-notice option could not be registered (out of memory?); the page cannot set it");
    }
#if LSS_DEV
    try
    {
        control::Register({.name  = L"encounter-watch",
                           .shape = control::Shape::Text,
                           .help  = L"(read only) the field encounter's state, the field enemies last seen with their force bytes "
                                    L"and perception states, and the player the watch rule would hold (the game watches player 1)",
                           .get   = [] { return control::Value{EncounterReading()}; }});
    }
    catch (...)
    {
    }
#endif
    platform::Log(L"encounter watch: installed; the field encounter's state changes, its field enemies, the preemptive prompt and "
                  L"each fight's start are logged with every player's distance and territory{}",
                  g_contains != nullptr ? L"" : L" (the territory test not found: distances only)");
    return true;
}

int NoticedPlayer() noexcept
{
    return players::generation::OnGameThread() ? g_fightBy : 0;
}

void Remove() noexcept
{
    g_anchorCall.Restore();
    g_enterBattleCall.Restore();
    g_subjectCall.Restore();
}

}  // namespace lss::battle::encounter_watch
