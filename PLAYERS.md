# Local Split Screen for FINAL FANTASY VII REBIRTH

Play the game in co-op on one PC: up to three players, each with their own controller, character, camera, HUD and
battle menu. In one window the screen splits. With two or three monitors each player gets a full screen of their own.

## What you need

- FINAL FANTASY VII REBIRTH on Steam, version 1.005 (the one the mod is tested on; 1.004 also loads). At every start
  the mod checks the game's code and leaves out, with a line in its log, whatever does not match.
- UE4SS, the mod loader, in the build made for this game: "UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible)" from Nexus
  Mods, installed as its page says. The mod is built against that loader.
- A controller for player 2 and one for player 3: an Xbox controller, or any other that Steam shows to games as one
  (Steam Input). Player 1 plays on a controller or the keyboard.

## Installing

1. Close the game.
2. Copy the `LocalSplitScreen` folder from the zip into `<game>\End\Binaries\Win64\ue4ss\Mods\`.
3. Start the game from Steam as always.

The folder carries its own `enabled.txt`, so UE4SS's `mods.txt` needs no line for it. To switch the mod off, delete
`enabled.txt`; to remove it, delete the folder. Run no other split-screen mod beside it.

## Playing

- **Player 1** is whoever presses a button first on the title screen.
- **Joining.** Press **Start** on a controller nobody is playing on. Player 2 plays the second character of the party's
  order and player 3 the third, each with a view of their own, and each keeps that place when you reorder the party.
  If that character is not free at that moment (a solo stretch of the story, a character the story is moving), the
  player joins as soon as it is. Change characters through the game's own Party menu.
- **Your own window.** Hold **View** (the small left button) for a second. It opens in your own view:
  - left and right on the d-pad change your player number, trading it with the player next to it, in the field or in
    a fight. You keep your controller and your screen and take over their character: become player 1 to lead the
    party, or hand the lead on;
  - LB and RB move your view to another screen;
  - A closes it, and holding B for a second leaves the game. Player 1 cannot leave.
- **Controller died?** Turn it back on and press **Start**: it takes back the player whose controller is gone.
- **By keyboard**, if you need it: Ctrl+F7 brings in player 2, then player 3, and Ctrl+F8 takes out the last one in.
- **Each player** walks, talks, shops, opens chests and doors, climbs and fights on their own controller, with their
  own prompts, markers, minimap, lock-on and command menu.
- **One picture for everyone.** Menus, shops, cutscenes, minigames and scripted conversations show as they do in single
  player, and the split returns when they end. A character the story takes over is handed back afterwards.
- **In battle** a knocked-out or petrified player stays on their character until it is revived or cured, and enemies
  divide their attention among the players.
- **Left behind?** Players 2 and 3 can click the left stick (L3) twice. Outside a battle it brings their character back
  beside player 1's; in a battle it brings a character that was shut out of the fight's area into it. It does nothing
  during a scene or a talk, in a menu, or when the character is already beside player 1's.

## The Co-op page

The pause menu has a **Co-op** row above System, and the title menu one below Options. It opens a page of the game's
own Options screen with the mod's options: how the screen splits for two and for three players, each view's field of
view, which screen shows which player, whether a battle gathers the players at its start and end, the battle's rates (enemy damage, enemy
and boss HP, the stagger gauge, EXP and AP) and more. Every option says what it does when you move onto it. A choice
applies at once and is saved to `LocalSplitScreen.ini` in the mod's folder, which the mod writes itself with every
option explained in it.

## Several monitors

One full view per player, each on its own monitor.

1. With the game closed, run **Multi Monitor Setup** (`MultiMonitorSetup.exe` in the mod's folder).
2. Click two or three monitors in the order you want them, left to right. Each is marked with the player it will show.
3. Set **Multiple Monitors** to On and press **Save**.
4. Start the game from Steam, in Borderless or Windowed display mode. The app need not stay open.

The monitors need not stand side by side in Windows, and they may differ in size and scaling. While the game runs they
are moved into a row, and they go back when it ends, even after a crash. Windows' own arrangement is never changed, so a
restart of the PC always returns your layout. If a game ever ends without putting them back, the app shows a **Restore
Monitors** button.

A monitor you picked that is not connected is waited for, not replaced: with two of three connected the game uses those
two, with fewer it starts in split-screen mode. A virtual monitor made by a streaming program (Apollo, Vibepollo) works like any
other and is recognised by its name, so give each device you stream to a name of its own.

Leave `MultiMonitorSetup.exe` where it is and under that name: the mod starts it in the background to put your monitors
back if the game dies.

## If something goes wrong

The mod writes to UE4SS's log, the newest file in `<game>\End\Binaries\Win64\ue4ss\logs\` (one per game start), in
lines marked `[LocalSplitScreen]`. The first says the version (`v` and its number, then `starting`); warnings say what
could not be set up and what was left out because of it. Send that file with a report.

## Thanks

Built on [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS), and on the Nexus Mods loader build for this game.
