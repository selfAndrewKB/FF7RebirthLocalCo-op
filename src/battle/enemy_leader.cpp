#include "battle/enemy_leader.h"

#include "battle/enemy_leader_rules.h"
#include "battle/equal_targeting.h"
#include "battle/target_coin.h"
#include "facts/engine.h"
#include "facts/in_battle.h"
#include "facts/party.h"
#include "platform/command_registry.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"
#include "players/tracked_object.h"
#include "session/party_switch.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lss::battle::enemy_leader {

namespace {

namespace rules = enemy_leader_rules;

// The game's leader-change walk (RE-enemy-leader-reads.md §3): its snapshot of the battle's flagged characters, the
// snapshot's step to the next live character, and the call that runs a battle AI blueprint's OnChangeLeader.
constexpr std::uint8_t kSnapshotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                           0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x50};
constexpr std::uint8_t kSnapshotNextBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C,
                                               0x24, 0x10, 0x41, 0x56, 0xB8, 0x80, 0x00, 0x00};
constexpr std::uint8_t kOnChangeLeaderBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0xB8, 0x20,
                                                 0x00, 0x00, 0x00, 0xE8, 0xD0, 0xA3, 0xEA, 0x00};
constexpr platform::FunctionFact kSnapshot{L"BattleCharMgr_SnapshotFlaggedEntries", 0x140DBC660, kSnapshotBytes};
constexpr platform::FunctionFact kSnapshotNext{L"BattleCharSnapshot_SkipToLiveEndCharacter", 0x140DBCA24,
                                               kSnapshotNextBytes};
constexpr platform::FunctionFact kOnChangeLeader{L"AIControllerBP_CallOnChangeLeader", 0x141204B50, kOnChangeLeaderBytes};

// The snapshot as the game's leader change keeps it on its stack: the index at +8, 128 inline 48-byte entries from
// +0x10 (the character first), the heap pointer at +0x1810 (null while inline) and the count at +0x1818. The snapshot
// function fills every field itself.
constexpr std::size_t kSnapshotBytesTotal = 0x1820;
constexpr std::size_t kSnapshotIndex      = 0x8;
constexpr std::size_t kSnapshotEntries    = 0x10;
constexpr std::size_t kSnapshotHeap       = 0x1810;
constexpr std::size_t kSnapshotCount      = 0x1818;
constexpr std::size_t kSnapshotEntryBytes = 48;
constexpr std::int32_t kSnapshotInline    = 128;

using SnapshotFn       = void (*)(void* snapshot);
using OnChangeLeaderFn = void (*)(void* controller, void* previous);
using FreeArrayFn      = void (*)(void* array);
using LeaderFn         = void* (*)();
using StandingFn       = std::uint8_t (*)(const void* character);
using CountFn          = std::int32_t (*)();
using TypeAtFn         = std::uint8_t (*)(std::int32_t index);
using CharacterFn      = void* (*)(std::uint8_t lane, std::uint8_t type);

// Set at install, then only read.
SnapshotFn       g_snapshot       = nullptr;
SnapshotFn       g_snapshotNext   = nullptr;
OnChangeLeaderFn g_onChangeLeader = nullptr;
FreeArrayFn      g_freeArray      = nullptr;
LeaderFn         g_leaderOf       = nullptr;
StandingFn       g_standing       = nullptr;
CountFn          g_battleCount    = nullptr;
TypeAtFn         g_battleTypeAt   = nullptr;
CharacterFn      g_characterOf    = nullptr;

// The stand-in and its player type: written by Tick (the character cleared first, so a reader that sees the pair
// change reads again), read on any thread.
std::atomic<void*>        g_instead{nullptr};
std::atomic<std::uint8_t> g_insteadType{facts::kNoPlayerType};

// Game thread.
void*                  g_leader = nullptr;  // the game's leader at the last tick (null outside a battle in co-op)
void*                  g_facing = nullptr;  // whom enemies followed at the last tick: the leader or its stand-in
players::TrackedObject g_facingObject;      // the same, checked still alive before it is handed to a blueprint
std::uint32_t g_changes  = 0;        // changes the enemies were told of
std::uint32_t g_told     = 0;        // enemy controllers told at the last one
std::uint32_t g_faults   = 0;        // walks a game call raised in

bool Standing(const void* character) noexcept
{
    std::uint8_t answer = 0;
    return character != nullptr && platform::CallGuarded(g_standing, answer, character) && answer != 0;
}

void* CharacterOfType(std::uint8_t type) noexcept
{
    void* character = nullptr;
    return type < facts::kNoPlayerType && platform::CallGuarded(g_characterOf, character, facts::kFieldLane, type)
               ? character
               : nullptr;
}

/// The game's battle list in its order (the switch's): each member's character, type and whether it stands and a
/// player plays it; `leaderAt` is the leader's place (negative: not in it). Kept to the list's first eight.
struct BattleList
{
    std::array<rules::Character, 8> members{};
    int                             count    = 0;
    int                             leaderAt = -1;
};
BattleList ReadBattleList(const void* leader, const equal_targeting::PlayerTypes& types) noexcept
{
    BattleList   list;
    std::int32_t count = 0;
    if (!platform::CallGuarded(g_battleCount, count) || count <= 0)
    {
        return list;
    }
    list.count = count < static_cast<std::int32_t>(list.members.size()) ? count : static_cast<int>(list.members.size());
    for (int i = 0; i < list.count; ++i)
    {
        std::uint8_t type = facts::kNoPlayerType;
        platform::CallGuarded(g_battleTypeAt, type, static_cast<std::int32_t>(i));
        rules::Character& member = list.members[static_cast<std::size_t>(i)];
        member.type              = type;
        member.character         = CharacterOfType(type);
        member.standing          = Standing(member.character);
        for (int player = 0; player < players::kMaxPlayers; ++player)
        {
            member.human = member.human || (types.Has(player) && types.type[player] == type);
        }
        if (member.character != nullptr && member.character == leader)
        {
            list.leaderAt = i;
        }
    }
    return list;
}

/// Who the enemy-facing character is described as in the log and the reading.
const wchar_t* Describe(const void* facing, const void* leader) noexcept
{
    if (facing == nullptr || facing == leader)
    {
        return L"the game's leader";
    }
    const int seat = players::generation::SeatOfMember(facing);
    return players::SeatBuilt(seat) ? players::SeatName(seat) : L"the standing AI ally";
}

/// The game's leader-change walk with `previous` as the character enemies followed: every enemy controller among the
/// battle's characters gets its OnChangeLeader. The number told.
std::uint32_t TellEnemies(void* previous) noexcept
{
    alignas(16) static std::byte snapshot[kSnapshotBytesTotal];  // game thread only; the game keeps its own on the stack
    std::memset(snapshot, 0, sizeof(snapshot));
    if (!platform::CallGuarded(g_snapshot, static_cast<void*>(snapshot)))
    {
        ++g_faults;
        return 0;
    }
    std::uint32_t told    = 0;
    bool          faulted = false;
    for (;;)
    {
        std::int32_t index = 0, count = 0;
        std::memcpy(&index, snapshot + kSnapshotIndex, sizeof(index));
        std::memcpy(&count, snapshot + kSnapshotCount, sizeof(count));
        if (index < 0 || index >= count)
        {
            break;
        }
        std::byte* heap = nullptr;
        std::memcpy(&heap, snapshot + kSnapshotHeap, sizeof(heap));
        if (heap == nullptr && index >= kSnapshotInline)
        {
            break;  // more than the inline entries with no heap: not the layout the game writes
        }
        const std::byte* entries   = heap != nullptr ? heap : snapshot + kSnapshotEntries;
        const void*      character = nullptr;
        if (!platform::CopyGuarded(&character, entries + kSnapshotEntryBytes * static_cast<std::size_t>(index),
                                   sizeof(character)))
        {
            faulted = true;
            break;
        }
        const void* controller = target_coin::ControllerOfEnemy(character);
        if (controller != nullptr && target_coin::IsEnemyController(controller))
        {
            if (platform::CallGuarded(g_onChangeLeader, const_cast<void*>(controller), previous))
            {
                ++told;
            }
            else
            {
                faulted = true;
            }
        }
        ++index;
        std::memcpy(snapshot + kSnapshotIndex, &index, sizeof(index));
        if (!platform::CallGuarded(g_snapshotNext, static_cast<void*>(snapshot)))
        {
            faulted = true;
            break;
        }
    }
    platform::CallGuarded(g_freeArray, static_cast<void*>(snapshot + kSnapshotHeap));
    if (faulted)
    {
        ++g_faults;
        platform::LogWarning(L"enemy leader: a game call RAISED in the leader-change walk ({} enemies told before it)", told);
    }
    return told;
}

/// Publishes the stand-in (null: none) and its type.
void Publish(void* instead, std::uint8_t type) noexcept
{
    if (instead == g_instead.load(std::memory_order_relaxed) && type == g_insteadType.load(std::memory_order_relaxed))
    {
        return;
    }
    g_instead.store(nullptr, std::memory_order_release);
    g_insteadType.store(type, std::memory_order_release);
    g_instead.store(instead, std::memory_order_release);
}

void Clear() noexcept
{
    g_leader = g_facing = nullptr;
    g_facingObject      = {};
    Publish(nullptr, facts::kNoPlayerType);
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto find     = [&image](const platform::FunctionFact& fact) { return platform::FindFunction(image, fact); };
    const auto snapshot = find(kSnapshot), next = find(kSnapshotNext), onChange = find(kOnChangeLeader),
               freeArray = find(facts::kFreeArrayData), leader = find(facts::kResolvePartyLeader),
               standing = find(facts::kIsBattleActive), count = find(facts::kRosterCount),
               typeAt = find(facts::kRosterPlayerType), character = find(facts::kCharacterByPlayerType);
    if (!snapshot || !next || !onChange || !freeArray || !leader || !standing || !count || !typeAt || !character ||
        !facts::InBattleKnown() || !equal_targeting::Installed())
    {
        platform::Log(L"enemy leader: UNAVAILABLE (an engine check failed); in co-op, enemies keep following player 1's "
                      L"downed character");
        return false;
    }
    g_snapshot       = reinterpret_cast<SnapshotFn>(*snapshot);
    g_snapshotNext   = reinterpret_cast<SnapshotFn>(*next);
    g_onChangeLeader = reinterpret_cast<OnChangeLeaderFn>(*onChange);
    g_freeArray      = reinterpret_cast<FreeArrayFn>(*freeArray);
    g_leaderOf       = reinterpret_cast<LeaderFn>(*leader);
    g_standing       = reinterpret_cast<StandingFn>(*standing);
    g_battleCount    = reinterpret_cast<CountFn>(*count);
    g_battleTypeAt   = reinterpret_cast<TypeAtFn>(*typeAt);
    g_characterOf    = reinterpret_cast<CharacterFn>(*character);
    platform::Log(L"enemy leader: installed; in co-op, while player 1's character is down, enemies turn on another "
                  L"player's standing character, else the standing AI ally");
    return true;
}

void Tick() noexcept
{
    if (g_snapshot == nullptr || !players::generation::OnGameThread())
    {
        return;
    }
    const auto types = equal_targeting::Types();
    if (!types || !session::party_switch::AnotherPlayerPlays() || !facts::InBattle())
    {
        Clear();
        return;
    }
    void* leader = nullptr;
    if (!platform::CallGuarded(g_leaderOf, leader) || leader == nullptr)
    {
        Clear();
        return;
    }
    const bool leaderStands = Standing(leader);
    std::array<rules::Character, players::kSeatsBuilt> seats{};
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        void* const member = players::generation::Active(seat) ? players::generation::MemberOf(seat) : nullptr;
        if (member != nullptr && member != leader && types->Has(seat))
        {
            seats[static_cast<std::size_t>(seat - 1)] = {member, types->type[seat], Standing(member), true};
        }
    }
    rules::Character standIn{};
    if (!leaderStands)
    {
        const BattleList list = ReadBattleList(leader, *types);
        standIn = rules::StandIn(false, seats, std::span(list.members.data(), static_cast<std::size_t>(list.count)),
                                 list.leaderAt);
    }
    void* const facing         = standIn.character != nullptr ? const_cast<void*>(standIn.character) : leader;
    const bool  leaderSwitched = g_leader != nullptr && leader != g_leader;
    // A change since the last look; with no last look (the battle's first, or the first after a tick out of co-op), a
    // stand-in already needed counts as one, from the game's leader.
    const bool changed = g_facing != nullptr ? facing != g_facing : standIn.character != nullptr;
    // The stand-in first: a blueprint asking "the player" inside its event gets the new answer.
    Publish(standIn.character != nullptr ? facing : nullptr, standIn.character != nullptr ? standIn.type : facts::kNoPlayerType);
    if (rules::TellsEnemies(changed, leaderSwitched, standIn.character != nullptr, leaderStands))
    {
        // The character they followed, as the game hands its event the old leader; none if it is gone since.
        void* const previous = g_facing == nullptr ? leader : g_facingObject.Alive() ? g_facing : nullptr;
        g_told               = TellEnemies(previous);
        ++g_changes;
        platform::LogDebug(L"enemy leader: enemies now follow {}{} ({} enemies told the leader changed)",
                           Describe(facing, leader), standIn.character != nullptr ? L"; player 1's character is down" : L"",
                           g_told);
    }
    g_leader = leader;
    g_facing = facing;
    auto* const object = static_cast<RC::Unreal::UObject*>(facing);
    g_facingObject     = players::IsLiveObject(object) ? players::TrackedObject::Of(object) : players::TrackedObject{};
}

void* Instead(std::uint8_t* type) noexcept
{
    // The character, its type, then the character again: a pair Tick changed in between is read again, so the type
    // always belongs to the character returned.
    for (;;)
    {
        void* const        instead = g_instead.load(std::memory_order_acquire);
        const std::uint8_t kind    = g_insteadType.load(std::memory_order_acquire);
        if (g_instead.load(std::memory_order_acquire) == instead)
        {
            if (type != nullptr)
            {
                *type = kind;
            }
            return instead;
        }
    }
}

#if LSS_DEV

void RegisterCommands()
{
    control::Register({.name  = L"enemy-leader",
                       .shape = control::Shape::Text,
                       .help  = L"whom enemies follow while player 1's character is down in co-op (the game's leader, "
                                L"another player's character or the standing AI ally); the changes they were told of",
                       .get   = [] {
                           try
                           {
                               return control::Value{std::format(
                                   L"enemies follow {}; changes told {} (enemies at the last {}), walks that raised {}",
                                   Describe(g_facing, g_leader), g_changes, g_told, g_faults)};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
}

#endif

}  // namespace lss::battle::enemy_leader
