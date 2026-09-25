#include "dev/terrain_watch.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UObject.hpp>

#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "player_two/generation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace lss::dev::terrain_watch {

namespace {

using RC::Unreal::UObject;

// RE-p2-terrain-action-conflict.md §1 and §6 (this build). The detector offsets are derived there
// (comp+0xEE0 plus the offsets 0x140EECE8C reads), so this log is also their live check.
constexpr std::uint32_t kLeaderActions = 0x598;  // PC -> the leader-action component
constexpr std::uint32_t kPawn          = 0x258;  // PC -> its pawn
constexpr std::uint32_t kDetector      = 0xEE0;  // component -> the terrain detector
constexpr std::uint32_t kRowElevenByte = 0xA2C;  // component: row 11's request byte
constexpr std::array<std::uint32_t, 7> kDetectorBytes{0x121, 0x126, 0x12B, 0x12C, 0x12D, 0x130, 0x13F};
constexpr const wchar_t*               kDetectorNames[] = {L"ledge", L"126", L"landingFail", L"12C", L"requestBlock",
                                                           L"130", L"13F"};
constexpr std::uint32_t kTarget = 0x170;  // FVector
constexpr std::uint32_t kStamp  = 0x590;  // last request, ms

// The request block's global gates: G = [[0x1490FEDE8]+0x170], int32 counters.
constexpr std::uintptr_t kGameState     = 0x1490FEDE8;
constexpr std::uint32_t  kGameStateGates = 0x170;
constexpr std::array<std::uint32_t, 6> kGates{0x1A12784, 0x1A124D4, 0x1A12720, 0x1A12470, 0x1A12288, 0x1A17548};

constexpr int kMaxLines = 400;

struct Player
{
    std::array<std::uint8_t, kDetectorBytes.size()> bytes{};
    std::uint8_t                                     row = 0;
    float                                            target[3]{};
    float                                            at[3]{};
    std::int64_t                                     stamp = 0;
    bool                                             read  = false;
};

std::uintptr_t g_gameState = 0;

// Game thread.
std::int32_t g_rootOffset     = -1;  // AActor::RootComponent
std::int32_t g_locationOffset = -1;  // USceneComponent::RelativeLocation
bool         g_offsetsTried   = false;
std::array<std::uint8_t, 2 * kDetectorBytes.size() + 2> g_loggedBytes{};
std::array<std::int32_t, kGates.size()>                  g_loggedGates{};
int  g_lines  = 0;
bool g_logged = false;

template <class T>
bool Read(std::uintptr_t address, T& out) noexcept
{
    return platform::CopyGuarded(&out, reinterpret_cast<const void*>(address), sizeof(T));
}

/// The reflected offsets, found once from the first pawn and its root component.
void FindOffsets(UObject* pawn)
{
    if (g_offsetsTried || pawn == nullptr)
    {
        return;
    }
    g_offsetsTried = true;
    auto* root     = pawn->GetPropertyByNameInChain(STR("RootComponent"));
    if (root == nullptr || root->GetSize() != sizeof(void*))
    {
        return;
    }
    UObject* component = nullptr;
    if (!Read(reinterpret_cast<std::uintptr_t>(pawn) + root->GetOffset_Internal(), component) || component == nullptr)
    {
        g_offsetsTried = false;  // tried again with a pawn that has its root
        return;
    }
    auto* location = component->GetPropertyByNameInChain(STR("RelativeLocation"));
    if (location != nullptr && location->GetSize() == 3 * sizeof(float))
    {
        g_rootOffset     = root->GetOffset_Internal();
        g_locationOffset = location->GetOffset_Internal();
    }
}

Player ReadPlayer(void* controller)
{
    Player player;
    const auto pc = reinterpret_cast<std::uintptr_t>(controller);
    std::uintptr_t component = 0;
    std::uintptr_t pawn      = 0;
    if (pc == 0 || !Read(pc + kLeaderActions, component) || component == 0)
    {
        return player;
    }
    const std::uintptr_t detector = component + kDetector;
    for (std::size_t i = 0; i < kDetectorBytes.size(); ++i)
    {
        Read(detector + kDetectorBytes[i], player.bytes[i]);
    }
    Read(component + kRowElevenByte, player.row);
    Read(detector + kTarget, player.target);
    Read(detector + kStamp, player.stamp);
    if (Read(pc + kPawn, pawn) && pawn != 0)
    {
        FindOffsets(reinterpret_cast<UObject*>(pawn));
        std::uintptr_t root = 0;
        if (g_rootOffset >= 0 && Read(pawn + g_rootOffset, root) && root != 0)
        {
            Read(root + g_locationOffset, player.at);
        }
    }
    player.read = true;
    return player;
}

std::wstring Describe(const Player& p)
{
    std::wstring text = std::format(L"row11 {:#04x}", p.row);
    for (std::size_t i = 0; i < kDetectorBytes.size(); ++i)
    {
        text += std::format(L" {} {}", kDetectorNames[i], p.bytes[i]);
    }
    return text + std::format(L" target ({:.0f},{:.0f},{:.0f}) at ({:.0f},{:.0f},{:.0f}) stamp {}", p.target[0], p.target[1],
                              p.target[2], p.at[0], p.at[1], p.at[2], p.stamp);
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto state = image.Rebase(kGameState, sizeof(void*));
    g_gameState      = state ? *state : 0;
}

void Tick() noexcept
{
    if (g_lines >= kMaxLines || g_gameState == 0 || !player_two::generation::Active())
    {
        g_logged = false;
        return;
    }
    try
    {
        const Player one = ReadPlayer(player_two::generation::PlayerOneController());
        const Player two = ReadPlayer(player_two::generation::PlayerTwoController());
        std::array<std::int32_t, kGates.size()> gates{};
        std::uintptr_t mode = 0, g = 0;
        if (Read(g_gameState, mode) && mode != 0 && Read(mode + kGameStateGates, g) && g != 0)
        {
            for (std::size_t i = 0; i < kGates.size(); ++i)
            {
                Read(g + kGates[i], gates[i]);
            }
        }
        std::array<std::uint8_t, 2 * kDetectorBytes.size() + 2> bytes{};
        std::copy(one.bytes.begin(), one.bytes.end(), bytes.begin());
        std::copy(two.bytes.begin(), two.bytes.end(), bytes.begin() + kDetectorBytes.size());
        bytes[2 * kDetectorBytes.size()]     = one.row;
        bytes[2 * kDetectorBytes.size() + 1] = two.row;
        if (g_logged && bytes == g_loggedBytes && gates == g_loggedGates)
        {
            return;
        }
        g_logged      = true;
        g_loggedBytes = bytes;
        g_loggedGates = gates;
        ++g_lines;
        platform::Log(L"terrain watch: P1 {} | P2 {} | gates {} {} {} {} {} {}", one.read ? Describe(one) : L"-",
                      two.read ? Describe(two) : L"-", gates[0], gates[1], gates[2], gates[3], gates[4], gates[5]);
    }
    catch (...)
    {
    }
}

}  // namespace lss::dev::terrain_watch
