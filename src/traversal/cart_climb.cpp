#include "traversal/cart_climb.h"

#include "facts/traversal.h"
#include "input/member_movement.h"
#include "platform/entry_install.h"
#include "platform/entry_redirect.h"
#include "platform/guarded.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/verified_code.h"
#include "platform/vtable_entry.h"
#include "players/call_site.h"
#include "players/describe.h"
#include "players/generation.h"
#include "players/object_fields.h"
#include "players/seats.h"
#include "players/weak_object.h"
#include "scopes/primary_controller.h"
#include "world/field_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <intrin.h>

namespace lss::traversal::cart_climb {

namespace {

using world::field_rules::AnyOn;

// The class's notify and follower slots (research/RE-cart-climb-seats.md).
constexpr std::uint8_t kNotifyBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x50,
                                         0x00, 0x00, 0x00, 0xE8, 0xD8, 0x13, 0xA8, 0xFE};
constexpr platform::SlotFact kNotifySlot{L"the cart climb's notify slot", 0x1466E9DD8,
                                         {L"CartJump_Vt748_Notify", 0x14362DB48, kNotifyBytes}};
constexpr std::uint8_t kFollowerBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                           0x24, 0x18, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
constexpr platform::SlotFact kFollowerSlot{L"the cart climb's follower-enter slot", 0x1466E9D40,
                                           {L"CartJump_Vt6B0_AIEnter_RequestJump", 0x14362E0A4, kFollowerBytes}};
// The shared climb list's add and remove. The add's first instruction crosses a 16-byte block in one build: its redirect
// goes at the second instruction (a store into the caller's home area that no branch lands on).
constexpr std::uint8_t kListBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                       0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x20};
constexpr platform::FunctionFact kListAdd{L"ParkourList_AddUnique_Global3C0", 0x143CFD8FC, kListBytes};
constexpr platform::FunctionFact kListRemove{L"ParkourList_RemoveAll_Global3C0", 0x143D01F20, kListBytes};
// The cart's own calls of them, known by their return addresses (each checked at install as an E8 to its function):
// the notify's add and remove, and the unregister's remove. Every other caller (the forced-parkour spots) passes through.
constexpr std::uintptr_t kNotifyAddCall        = 0x14362DBFE;
constexpr std::uintptr_t kNotifyRemoveCall     = 0x14362DC13;
constexpr std::uintptr_t kUnregisterRemoveCall = 0x14362DA33;

constexpr std::size_t kMaxCarts = 8;  // carts with recorded wishes at once

using NotifyFn   = void (*)(void* actor, void* context);
using FollowerFn = std::uint8_t (*)(void* actor, void* character);
using ListFn     = void (*)(void* actor);

/// One cart's players' wishes: on the climb list or off, as each player's own call last asked.
struct Cart
{
    std::uint64_t actor = 0;  // the actor's weak value
    bool          wish[players::kMaxPlayers] = {};
};

platform::VTableEntry   g_notify, g_follower;
platform::EntryRedirect g_add, g_remove;
std::uintptr_t          g_notifyAddReturn = 0, g_notifyRemoveReturn = 0, g_unregisterReturn = 0;
bool                    g_ready = false;

// Game thread.
std::array<Cart, kMaxCarts> g_carts{};
bool                        g_fullLogged = false, g_scopeLogged = false;
std::array<bool, players::kMaxPlayers> g_followerSaid{};  // by seat: a refused follower call said since the last allowed one
constexpr int                kNotifiedNone  = -1;  // no notify runs (the unregister: the open scope's player)
constexpr int                kNotifiedOther = -2;  // a notify for a character no player plays, or run outside its scope
thread_local int             t_notified     = kNotifiedNone;
constexpr int                kNotCartCall   = -3;  // a list call that is none of the cart's: passed through

/// A seat that left the game wishes nothing (its notify and leave never come): its wish dropped, and the cart taken off
/// the climb list with the game's own remove when no player's wish is left.
void DropLeftSeats(Cart& cart, void* actor) noexcept
{
    bool dropped = false;
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        if (!players::generation::Active(seat) && cart.wish[seat])
        {
            cart.wish[seat] = false;
            dropped         = true;
        }
    }
    if (!dropped || AnyOn(cart.wish))
    {
        return;
    }
    reinterpret_cast<ListFn>(g_remove.Original())(actor);
    try
    {
        platform::LogDebug(L"cart climb: {} is off the climb list: the player who wished it on left the game",
                           players::NameOf(actor));
    }
    catch (...)
    {
    }
}

/// The record of `actor`: its entry (a departed seat's wish dropped first), else (with `make`) a free, dead or wishless
/// one made its; null when none, the table is full or the actor has no weak value.
Cart* CartOf(const void* actor, bool make) noexcept
{
    const std::uint64_t weak = players::weak_object::WeakOf(const_cast<void*>(actor));
    if (weak == 0)
    {
        return nullptr;
    }
    for (Cart& cart : g_carts)
    {
        if (cart.actor == weak)
        {
            DropLeftSeats(cart, const_cast<void*>(actor));
            return &cart;
        }
    }
    if (!make)
    {
        return nullptr;
    }
    for (Cart& cart : g_carts)
    {
        if (cart.actor == 0 || !AnyOn(cart.wish) || players::weak_object::ObjectOf(cart.actor) == nullptr)
        {
            cart = {weak};
            return &cart;
        }
    }
    if (!g_fullLogged)
    {
        g_fullLogged = true;
        try
        {
            platform::LogWarning(L"cart climb: no room to record the players' wishes for {}: the game's own calls stand for it "
                                 L"(said once)",
                                 players::NameOf(actor));
        }
        catch (...)
        {
        }
    }
    return nullptr;
}

void LogWish(const void* actor, int player, bool wish, bool listed) noexcept
{
    if (!platform::Enabled(platform::Level::Debug))
    {
        return;
    }
    try
    {
        platform::LogDebug(L"cart climb: {}'s wish for {} is {}; the cart is {} the climb list{}", players::SeatName(player),
                           players::NameOf(actor), wish ? L"on" : L"off", listed ? L"on" : L"off",
                           !wish && listed ? L" (another player still wishes it on)" : L"");
    }
    catch (...)
    {
    }
}

/// The player a list call is for: the notified one for the notify's calls, the open scope's for the unregister's;
/// kNotifiedOther for a notify booked to no player, and kNotCartCall for another caller (passed through).
int PlayerOfCall(std::uintptr_t caller) noexcept
{
    if (caller == g_notifyAddReturn || caller == g_notifyRemoveReturn)
    {
        return t_notified >= 0 ? t_notified : kNotifiedOther;
    }
    return caller == g_unregisterReturn ? scopes::primary_controller::OpenSeat() : kNotCartCall;
}

/// The climb list's add: the cart's own calls record the caller's wish; the game's add always runs (it adds a cart
/// once).
void ListAdd(void* actor) noexcept
{
    const auto original = reinterpret_cast<ListFn>(g_add.Original());
    const int  player   = actor != nullptr && players::generation::OnGameThread()
                              ? PlayerOfCall(reinterpret_cast<std::uintptr_t>(_ReturnAddress()))
                              : kNotCartCall;
    if (player >= 0)
    {
        if (Cart* cart = CartOf(actor, true); cart != nullptr && !cart->wish[player])
        {
            cart->wish[player] = true;
            LogWish(actor, player, true, true);
        }
    }
    original(actor);
}

/// The climb list's remove: the cart's own calls record the caller's wish off, and the game's remove runs only when no
/// player still wishes the cart on (a call for no player's character takes nobody's wish away).
void ListRemove(void* actor) noexcept
{
    const auto original = reinterpret_cast<ListFn>(g_remove.Original());
    const int  player   = actor != nullptr && players::generation::OnGameThread()
                              ? PlayerOfCall(reinterpret_cast<std::uintptr_t>(_ReturnAddress()))
                              : kNotCartCall;
    if (player == kNotCartCall)
    {
        original(actor);
        return;
    }
    Cart* cart = CartOf(actor, false);  // no record: no player wishes the cart on, so the game's remove stands
    if (cart == nullptr)
    {
        original(actor);
        return;
    }
    const bool changed = player >= 0 && cart->wish[player];
    if (player >= 0)
    {
        cart->wish[player] = false;
    }
    const bool listed = AnyOn(cart->wish);
    if (!listed)
    {
        original(actor);
    }
    if (changed)
    {
        LogWish(actor, player, false, listed);
    }
}

/// The cart's notify: the game's, a seat's character's inside the seat's primary scope and leader answer, the player
/// named for the list calls meanwhile (a call that ran outside its scope carries player 1's answers: booked to nobody).
void Notify(void* actor, void* context) noexcept
{
    const auto original = reinterpret_cast<NotifyFn>(g_notify.Original());
    if (!players::generation::OnGameThread())
    {
        original(actor, context);
        return;
    }
    const int notified = players::generation::PlayerOfCharacter(ReadPointer(context, facts::kNotifyContextCharacter));
    const int saved    = t_notified;
    t_notified         = notified >= 0 ? notified : kNotifiedOther;
    if (notified < 1)
    {
        original(actor, context);
    }
    else if (!input::member_movement::NotifyInPrimaryScope(notified, original, actor, context))
    {
        if (!g_scopeLogged)
        {
            g_scopeLogged = true;
            try
            {
                platform::LogWarning(L"cart climb: the notify of {} for {}'s character ran outside their scope (it could not "
                                     L"be held): its answers are player 1's, booked to no player (said once)",
                                     players::NameOf(actor), players::SeatName(notified));
            }
            catch (...)
            {
            }
        }
        t_notified = kNotifiedOther;
        original(actor, context);
    }
    t_notified = saved;
}

/// The cart's follower enter: the game's for an AI member, refused for a player's member (a player's character follows
/// nobody; 0 is the game's own answer for a character that may not take the cart).
std::uint8_t FollowerEnter(void* actor, void* character) noexcept
{
    const int player = players::generation::OnGameThread() ? players::generation::PlayerOfCharacter(character) : -1;
    if (player < 1)
    {
        return reinterpret_cast<FollowerFn>(g_follower.Original())(actor, character);
    }
    auto& said = g_followerSaid[static_cast<std::size_t>(player)];
    if (!said)
    {
        said = true;
        try
        {
            platform::LogDebug(L"cart climb: the game's follower path asked {}'s member to take {}: refused (a player's "
                               L"member follows nobody; said once while the seat plays)",
                               players::SeatName(player), players::NameOf(actor));
        }
        catch (...)
        {
        }
    }
    return 0;
}

}  // namespace

void Install(const platform::GameImage& image) noexcept
{
    const auto add          = platform::FindFunction(image, kListAdd);
    const auto remove       = platform::FindFunction(image, kListRemove);
    const auto notifyAdd    = image.Rebase(kNotifyAddCall, 5);
    const auto notifyRemove = image.Rebase(kNotifyRemoveCall, 5);
    const auto unregister   = image.Rebase(kUnregisterRemoveCall, 5);
    // Each return address is trusted only once its site is checked as a call of its function.
    using players::call_site::Calls;
    bool ready = add && remove && notifyAdd && notifyRemove && unregister && Calls(*notifyAdd, *add) &&
                 Calls(*notifyRemove, *remove) && Calls(*unregister, *remove);
    if (ready)
    {
        g_notifyAddReturn    = *notifyAdd + 5;
        g_notifyRemoveReturn = *notifyRemove + 5;
        g_unregisterReturn   = *unregister + 5;
    }
    ready = ready && platform::InstallAtEntryOrSecond(g_add, image, kListAdd, reinterpret_cast<std::uintptr_t>(&ListAdd)) &&
            platform::InstallAtEntryOrSecond(g_remove, image, kListRemove, reinterpret_cast<std::uintptr_t>(&ListRemove)) &&
            platform::ReplaceSlot(image, kNotifySlot, reinterpret_cast<std::uintptr_t>(&Notify), g_notify) &&
            platform::ReplaceSlot(image, kFollowerSlot, reinterpret_cast<std::uintptr_t>(&FollowerEnter), g_follower);
    if (!ready)
    {
        Remove();
        platform::LogWarning(L"cart climb: NOT placed (a slot, the climb list or one of its calls is not as the research read "
                             L"it): cart climbs stay player 1's");
        return;
    }
    g_ready = true;
    platform::Log(L"cart climb: ready; players 2 and 3 climb the carts with their own character (the climb list keeps a cart "
                  L"while any player wishes it on; a player's member takes no follower climb)");
}

void Remove() noexcept
{
    g_ready = false;
    for (platform::EntryRedirect* entry : {&g_add, &g_remove})
    {
        if (entry->IsInstalled())
        {
            (void)entry->Restore();
        }
    }
    for (platform::VTableEntry* slot : {&g_notify, &g_follower})
    {
        if (slot->Original() != 0)
        {
            (void)slot->Restore();
        }
    }
    g_carts.fill({});
    g_followerSaid.fill(false);
    g_fullLogged = g_scopeLogged = false;
}

void Tick() noexcept
{
    if (!g_ready || !players::generation::OnGameThread())
    {
        return;
    }
    for (Cart& cart : g_carts)
    {
        void* actor = cart.actor != 0 ? players::weak_object::ObjectOf(cart.actor) : nullptr;
        if (actor == nullptr)
        {
            cart = {};
            continue;
        }
        DropLeftSeats(cart, actor);
    }
    for (int seat = 1; seat <= players::kSeatsBuilt; ++seat)
    {
        g_followerSaid[static_cast<std::size_t>(seat)] =
            g_followerSaid[static_cast<std::size_t>(seat)] && players::generation::Active(seat);
    }
}

bool Ready() noexcept
{
    return g_ready;
}

bool Admits(const void* actor) noexcept
{
    return actor != nullptr && ReadField<std::uint8_t>(actor, facts::kCartJumpParkourControl) != 0;
}

}  // namespace lss::traversal::cart_climb
