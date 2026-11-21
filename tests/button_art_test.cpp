// Host-side tests for hud::button_art_rules, run without the game: each device's art by the game's own rule.

#include "hud/button_art_rules.h"

#include "expect.h"

namespace {

namespace rules = lss::hud::button_art_rules;
using rules::Art;
using rules::Device;

constexpr Art kXbox{rules::kFamilyXbox, 0};
constexpr Art kDualSense{rules::kFamilyPlayStation, 0};
constexpr Art kDualShock4{rules::kFamilyDualShock4, 0};

/// The game's table: a PlayStation pad shows its own kind's art unless the option asks for Xbox's; an Xbox pad shows
/// Xbox art unless the option asks for PlayStation's; the fourth set wins over both.
void TheFamilyTable()
{
    EXPECT(rules::PadFamily(false, false, rules::kOptionAuto) == rules::kFamilyXbox);
    EXPECT(rules::PadFamily(false, false, rules::kOptionXbox) == rules::kFamilyXbox);
    EXPECT(rules::PadFamily(false, false, rules::kOptionPlayStation) == rules::kFamilyPlayStation);
    EXPECT(rules::PadFamily(true, false, rules::kOptionAuto) == rules::kFamilyPlayStation);
    EXPECT(rules::PadFamily(true, true, rules::kOptionAuto) == rules::kFamilyDualShock4);
    EXPECT(rules::PadFamily(true, true, rules::kOptionPlayStation) == rules::kFamilyDualShock4);
    EXPECT(rules::PadFamily(true, true, rules::kOptionXbox) == rules::kFamilyXbox);
    EXPECT(rules::PadFamily(true, false, rules::kOptionFourth) == rules::kFamilyFourth);
    EXPECT(rules::PadFamily(false, false, rules::kOptionFourth) == rules::kFamilyFourth);
}

/// An XInput pad by its slot's type (a DualSense behind Steam Input is type 5), the game's own PlayStation reader's pad,
/// and the keyboard: keycaps over the game's family, so a `,pad` tag still shows the shared pad's art.
void EachDevice()
{
    EXPECT(rules::ArtOf(Device::XInputPad, 0, rules::kOptionAuto, kDualSense) == kXbox);
    EXPECT(rules::ArtOf(Device::XInputPad, 2, rules::kOptionAuto, kDualSense) == kXbox);
    EXPECT(rules::ArtOf(Device::XInputPad, rules::kPadTypeDualSense, rules::kOptionAuto, kXbox) == kDualSense);
    EXPECT(rules::ArtOf(Device::XInputPad, rules::kPadTypeDualShock4, rules::kOptionAuto, kXbox) == kDualShock4);
    EXPECT(rules::ArtOf(Device::PlayStationPad, 0, rules::kOptionAuto, kXbox) == kDualSense);
    EXPECT(rules::ArtOf(Device::PlayStationPad, 0, rules::kOptionXbox, kDualSense) == kXbox);
    EXPECT((rules::ArtOf(Device::Keyboard, 0, rules::kOptionAuto, kXbox) == Art{rules::kFamilyXbox, 1}));
    EXPECT((rules::ArtOf(Device::Keyboard, 0, rules::kOptionAuto, kDualSense) == Art{rules::kFamilyPlayStation, 1}));
}

/// Two Xbox pads: the seat's art is the game's, so its pass changes nothing.
void SameArtNoWork()
{
    EXPECT(rules::ArtOf(Device::XInputPad, 0, rules::kOptionAuto, kXbox) == kXbox);
    EXPECT((rules::ArtOf(Device::XInputPad, 0, rules::kOptionAuto, Art{rules::kFamilyXbox, 1}) != Art{rules::kFamilyXbox, 1}));
}

}  // namespace

int main()
{
    TheFamilyTable();
    EachDevice();
    SameArtNoWork();
    return lss::test::Report();
}
