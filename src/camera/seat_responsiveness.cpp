#include "camera/seat_responsiveness.h"

#include "facts/engine.h"
#include "platform/byte_scope.h"
#include "platform/command_registry.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/generation.h"
#include "players/seats.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace lss::camera::seat_responsiveness {

namespace {

namespace settings = platform::settings;

using OptionDataFn = std::uint8_t* (*)();

/// Each seat's option, by seat (player 1's level is the game's own option).
constexpr std::array<const settings::Option*, players::kMaxPlayers> kSeatOptions{nullptr, &settings::kResponsivenessTwo,
                                                                                 &settings::kResponsivenessThree};
static_assert(players::kMaxPlayers == 3, "one option per added player");
static_assert(settings::kResponsivenessWords.size() == facts::kResponsivenessMax - facts::kResponsivenessMin + 1,
              "one word per level of the game's Responsiveness");

struct Seat
{
    std::uint8_t          level = 0;  // the game's level for the seat; 0 for a seat without an option
    platform::ByteStore<1, 1> context;
    std::uintptr_t        definedAt = 0;  // the live byte the context is defined over
    std::uint64_t         opened    = 0;
    std::uint64_t         undone    = 0;  // game writes inside a seat's scope, put back
};

// Set once at install.
OptionDataFn g_optionData = nullptr;

// Game thread.
std::array<Seat, players::kMaxPlayers> g_seats;
bool                                   g_undoneWarned = false;

std::optional<std::uint8_t> LevelOf(const std::wstring& word) noexcept
{
    const auto place = settings::WordIndex(settings::kResponsivenessTwo, word);
    return place ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(facts::kResponsivenessMin + *place)) : std::nullopt;
}

const wchar_t* WordOf(std::uint8_t level) noexcept
{
    return level >= facts::kResponsivenessMin && level <= facts::kResponsivenessMax
               ? settings::kResponsivenessWords[level - facts::kResponsivenessMin]
               : L"none";
}

void SetLevel(int seat, std::uint8_t level, const wchar_t* why)
{
    g_seats[static_cast<std::size_t>(seat)].level = level;
    platform::Log(L"responsiveness: {} at {} ({})", players::SeatName(seat), level, why);
}

/// The live address of the game's Responsiveness byte (the game's getter answers its options, or a static block before
/// they load), or 0 when the getter cannot be called.
std::uintptr_t LiveByte() noexcept
{
    std::uint8_t* options = nullptr;
    return g_optionData != nullptr && platform::CallGuarded(g_optionData, options) && options != nullptr
               ? reinterpret_cast<std::uintptr_t>(options) + facts::kOptionResponsiveness
               : 0;
}

/// The context over `live`, defined anew when the byte moved (the options loaded after the last define).
bool DefineOver(Seat& seat, std::uintptr_t live) noexcept
try
{
    if (seat.definedAt == live)
    {
        return true;
    }
    const platform::ByteContext::Range range{live, 1, platform::ByteContext::Kind::Hold};
    if (!seat.context.Define(std::span(&range, 1)))
    {
        return false;
    }
    seat.definedAt = live;
    return true;
}
catch (...)
{
    return false;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    const auto options = platform::FindFunction(image, facts::kSystemOptionData);
    g_optionData       = options ? reinterpret_cast<OptionDataFn>(*options) : nullptr;
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const settings::Option& option = *kSeatOptions[static_cast<std::size_t>(seat)];
        const auto              level  = LevelOf(settings::Of(option).word);
        SetLevel(seat, level.value_or(LevelOf(option.fallback).value_or(facts::kResponsivenessMin)),
                 settings::Why(option).c_str());
        control::RegisterOption(
            option,
            [seat](const std::wstring& word) {
                const auto chosen = LevelOf(word);
                if (!chosen)
                {
                    return control::Outcome{false, L"not a level from 1 to 5"};
                }
                SetLevel(seat, *chosen, L"set");
                return control::Outcome{};
            },
            [seat] { return std::wstring(WordOf(g_seats[static_cast<std::size_t>(seat)].level)); });
    }
#if LSS_DEV
    control::Register({.name  = L"responsiveness",
                       .shape = control::Shape::Text,
                       .help  = L"(read only) the game's Responsiveness byte (player 1's level) and players 2 and 3's levels, "
                                L"with the scopes each was held for and the game writes put back",
                       .get   = [] {
                           try
                           {
                               return control::Value{Reading()};
                           }
                           catch (...)
                           {
                               return control::Value{std::wstring(L"unreadable (an exception)")};
                           }
                       }});
#endif
    if (g_optionData == nullptr)
    {
        platform::LogWarning(L"responsiveness: the game's option getter is not what the research read; players 2 and 3 turn at "
                             L"player 1's level");
        return false;
    }
    return true;
}
catch (...)
{
    return false;
}

Scope::Scope(int seat) noexcept : seat_(seat)
{
    if (seat <= 0 || seat >= players::kMaxPlayers || !players::generation::OnGameThread())
    {
        return;
    }
    Seat&                state = g_seats[static_cast<std::size_t>(seat)];
    const std::uintptr_t live  = LiveByte();
    if (state.level == 0 || live == 0 || state.context.IsOpen() || !DefineOver(state, live))
    {
        return;
    }
    const auto value = state.context.Value(0);
    if (value.empty())
    {
        return;
    }
    value[0] = state.level;
    held_    = state.context.Open();
    if (held_)
    {
        ++state.opened;
    }
}

Scope::~Scope()
{
    if (!held_)
    {
        return;
    }
    Seat& state = g_seats[static_cast<std::size_t>(seat_)];
    if (state.context.Close() != 0)
    {
        ++state.undone;
        if (!g_undoneWarned)
        {
            g_undoneWarned = true;
            platform::LogWarning(L"responsiveness: the game wrote its Responsiveness byte during {}'s step; put back to player "
                                 L"1's (logged once)",
                                 players::SeatName(seat_));
        }
    }
}

std::wstring Reading()
{
    std::uint8_t one  = 0;
    const auto   live = LiveByte();
    std::wstring text = live != 0 && platform::Read(reinterpret_cast<const void*>(live), one)
                            ? std::format(L"the game's byte (player 1) {}", one)
                            : std::wstring(L"the game's byte unread");
    for (int seat = 1; seat < players::kMaxPlayers; ++seat)
    {
        const Seat& state = g_seats[static_cast<std::size_t>(seat)];
        text += std::format(L"; {} {} (held for {} steps, {} game writes put back)", players::SeatName(seat), state.level,
                            state.opened, state.undone);
    }
    return text;
}

}  // namespace lss::camera::seat_responsiveness
