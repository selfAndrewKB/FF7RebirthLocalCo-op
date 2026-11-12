# LocalSplitScreen

Split-screen for up to three players for FINAL FANTASY VII REBIRTH, as a UE4SS C++ mod.

- **Plan and state:** `../Documentation/plan/README.md` (the front page; one file per topic in `plan/topics/`)
- **Why it is built this way:** `../Documentation/SPLITSCREEN-REBUILD-PHILOSOPHY.md`; the rules in
  `../Documentation/plan/rules.md`
- **How it works, module by module:** `../Documentation/SPLITSCREEN-ARCHITECTURE.md`; the systems reversed in
  `../Documentation/research/RE-*.md`

## What it does

**Joining with a controller:** press **Start** on a controller that plays no one to join on it (player 2, then player
3). Hold **View** (Back) for a second to open your own window in your view: **LB**/**RB** move your view one screen left
or right (the order is saved), **A** closes it, holding **B** for a second leaves the game (player 1 stays). On the title
screen, the controller that presses a button plays player 1. A controller that plays no one never pauses the game.
Change characters through the game's own Party menu. Players 2 and 3: click **L3** twice to be brought back beside
player 1 in the field (the game's own catch-up warp; not in a menu, a scene, a battle, or when already beside).

**Co-op settings in the game:** the pause menu has a **Co-op** row above System, and the title menu one below Options.
It opens the game's own Options screen
with the mod's settings (monitors, screen order, layouts, and the two split-screen FOV weights as the game's sliders);
a change applies at once and is saved to `LocalSplitScreen.ini`.

Press **Ctrl+F7** to add a player from the keyboard (player 2, then player 3, on pads 2 and 3) and **Ctrl+F8** to remove
the last one in. When another local player exists, the engine splits the screen by itself (two halves; with three
players one large view and two, the layout a setting). This mod makes the split correct: each player's
view is rendered by the engine as its own view family, so the game's meshlet renderer never has
two views in one pass. With one player, the mod changes nothing except the D3D12 residency manager
(see Performance).

Players 2 and 3 each start on their own camera and their own pad (sticks and movement) and play their slot of the
party order (player 2 the 2nd, player 3 the 3rd), and the HUD is laid out in each view as it is in single player, with
each player's view showing their own HUD. None of this needs a key.

| Keys | Switch |
|------|--------|
| Ctrl+F5 | Top/bottom or side by side |
| Ctrl+F4 | Halves filled, or 16:9 halves with bars |
| Ctrl+F6 | For testing: players 2 and 3's views show player 1's camera |
| Ctrl+F9 | For testing: every pad drives every player's sticks (the game's merge) |
| Ctrl+F10 | For comparison: the NPC population follows the leader alone instead of every player |
| Ctrl+F11 | For comparison: characters near player 2 or 3 take their detail from player 1's camera alone |
| Ctrl+F3 | The three-player monitor mode on and off: the window across the row of monitors, player 1's picture in every column no player has |
| Ctrl+F2 | The game window across its row of monitors, and back |

**Several monitors:** one full view per player, each on its own monitor.

1. With the game closed, run **Multi Monitor Setup** (`MultiMonitorSetup.exe` in the mod's folder,
   `Mods/LocalSplitScreen`).
2. Click two or three monitors in the order you want them, left to right. Each is marked with the player it will show.
3. Set **Multiple Monitors** to On and press **Save**.
4. Start the game as always, in windowed fullscreen (borderless) or windowed mode. The app need not stay open.

The monitors need not stand side by side in Windows, and they may differ in size and scaling: each view fills its own
monitor edge to edge, a wider monitor showing more to the sides at the same scale and a shorter one its own height.
While the game runs they are moved into a row, and they go back when it ends, even after a crash. Windows' own arrangement is never changed, so
a restart of the PC always returns your layout. If a game ever ends without putting them back, the app shows a
**Restore Monitors** button. A monitor you picked that is not connected is waited for, not replaced: with two of three
connected the game uses those two, with fewer it stays in one window. A virtual monitor made by a streaming program
(Apollo, Vibepollo) works like any other and is recognised by its name, so give each device you stream to a name of its
own. With no monitors picked, the game takes the longest row of monitors of one height it finds. Leave
`MultiMonitorSetup.exe` where it is and under that name: the mod starts it in the background to put your monitors back
if the game dies.

**Settings file:** the mod writes `Mods/LocalSplitScreen/LocalSplitScreen.ini` at every start from its own list of
options, each with its words, its default and a line of help, and reads it once at the next start: change a value after
an `=` to make it the default (`multi-monitor = on` holds the three-player monitor mode from the first steady frame;
`two-player-split = vertical` puts two players side by side from the first join; `split-fov.horizontal` sets the filled
halves' FOV weight). The keys above change an option for the session only. The repository ships no copy: the file is
the code's own.

With the development tools built in, control commands switch the rest for diagnosis, e.g. `hud-band off`,
`hud-image off`, `hud-levers off`, `leak-fixes 0` (`help` lists them all, each with its reading; every option of the
settings file is a command of the same name): typed in the console (`python tools/remote.py shell`, 127.0.0.1:4419)
or sent with `-c` or `run <file>` (`src/control/script.h` has the language).

## Performance

Split screen renders the scene twice, so the mod trims engine work that a second view makes redundant. Each change
was measured live and leaves the picture unchanged (`../Documentation/PLAN-P2-NATIVE-PLAY.md`, performance section).
A file in the mod's folder keeps the game's behaviour:

| Change | When | Keep the game's way |
|--------|------|---------------------|
| D3D12 residency manager off (one GPU wait per submission less) | whole session | `residency_on.txt` |
| GPU frame-time gap recorder off (timing method 1 for dynamic resolution) | player 2 or 3 in | — |
| Parallel passes without their own GPU submission | player 2 or 3 in | `submit_hints_on.txt` |
| Players 2 and 3's duplicate scenery-streaming work skipped | player 2 or 3 in | `stream-feedback each` (control) |
| D3D12 async compute (measured slower; off) | opt-in | `async_compute_on.txt` turns it on |

`watch_only.txt` runs the game unmodified for comparisons. `perf_log.txt` turns the perf log on from launch, and
`census.txt` (or `census on`) adds a count of every GPU submission per frame to it.

## Comments

A comment says what the code does and why, in plain sentences, and stays true as the code ages. It carries no date, no
version number, no quote of or credit to a person, and no history (`until`, `used to`, `now`): the reason behind a rule is
stated as a fact, and a fault the code prevents as what would happen without it. It does not name the Remake mod: where a
piece came from, and how the two mods differ, is in the topic's History in the documentation. Addresses live in
`src/facts/` and `src/platform/builds/`, research in `../Documentation/research`: a comment names the game's function
without its address (in a machine-code stub, the file's named constant, with an offset where needed), and may name a
research file, without a section number or pass tag. It may mark a claim `[CONFIRMED]` or `[ASSUMPTION]`.
A hook enforces this (`../../.claude/hooks/comment_style.py`): an edit or a commit whose new comment text breaks the
rule is refused with the line and the reason; `python ../../.claude/hooks/comment_style.py check src tests` runs the
same check by hand, and a game term that matches by accident goes in `comment_style_allow.txt` beside it.

## Layout

Generated from each module's first header comment by `tools/layout_doc.py` (a ctest checks it is current).

```
src/
  engine_tick.cpp           The mod's heartbeat: a call before each UGameEngine::Tick, on the game thread, before any
                            world ticks
  mod.cpp                   LocalSplitScreen: the UE4SS entry point
  modules.cpp               The module table (modules.cpp): every module the entry point installs after the session, in
                            install order, each with its Remove or none
  platform/                 the game image, the fact checks, the redirects, guards, the log, the settings file, the
                            command registry
    angles.h                Angle units, and the one quaternion reading the mod makes
    binned_pools            How full the engine allocator's small-block pools are (research/RE-oom-path.md, "Reading the
                            pools live")
    block_patch             One change to the game's code, as every redirect and in-place patch makes it
    build_translation       Game builds, and the translation of the research build's IDA addresses to another build
    build_value.h           A value that differs between game builds: a field offset or vtable offset that a later build
                            moved
    builds/build_1_005.h    Generated by tools/version_match/match.py (research build 1.004 -> 1.005)
    builds/build_1_005_replaced.h
                            Game version 1.005: the addresses the offline matcher could not place, re-found by hand in
                            IDA
    byte_scope              The game's single slots answered for one player for the length of a call
    call_redirect           Sends one `call rel32` (or one tail call, `jmp rel32`) in the game's code to a function of
                            ours instead of its usual target, and puts it back
    code_memory             The two memory operations the mod's code changes share (code_redirect.h, call_redirect.h):
    code_redirect           Sends execution at one verified spot in the game's code through a few bytes of our own, and
                            puts the original bytes back
    command_registry        The command registry: every switch, action and readable value the control console knows, as
                            the modules register them
    deferred_sites          A module's call sites placed at install, or from the game thread when the build needs it
    dev_switch.h            The dev-tools switch (CMake option LSS_DEV_TOOLS, dev/dev_tools.h) as one macro for `#if`: 1
                            with the dev tools, 0 in a player's build
    entry_install.h         An entry redirect placed where the build allows: at the function's entry when its first
                            instruction sits inside one 16-byte block, else at its second instruction
                            (platform/entry_redirect.h)
    entry_redirect          Sends every call of one game function through a function of ours, which may call the game's
                            function itself (Original()), and puts it back
    every.h                 A thing done at most once per period, and the counters it reports
    frame_watch             Frame hitches, and what the mod did in them
    game_array.h            An FString or TArray as the engine lays them out (Data, Num, Max), and a source FString over
                            the mod's own text for the game's string copies to read
    game_thread_sites.h     Call sites placed from the game thread instead of at install: the ones whose five bytes
                            cross a 16-byte block in game version 1.005 (platform/call_redirect.h, InstallFromOwnThread)
    guarded                 Structured-exception guards for touching engine memory and calling engine code
    guarded_read.h          The guarded reads of engine memory: platform/guarded.h's CopyGuarded and WriteGuarded under
                            the shapes every module reads with
    hitch_step.h            The frame watch's hitch rule (platform/frame_watch.h) as a pure step, tested on the host
                            (tests/hitch_step_test.cpp)
    image                   The game's executable image in memory, and the translation from IDA addresses to live ones
    instruction_patch.h     Rewrites one instruction of the game's code in place, with one of the same length, and puts
                            it back
    level_names.h           The log levels' names, as the control command `log-level` and the mod folder's log-level.txt
                            spell them
    log                     The mod's log: one line per event, written to UE4SS.log with a "[LocalSplitScreen]" prefix
    memory_alarm.h          When the memory watch writes a line (platform/memory_watch.h), as pure rules tested on the
                            host: a jump in commit, the card's use or a pool; a change of the card's budget state; a
                            lasting low-commit or full-pool state repeated …
    memory_watch            The game's memory, the graphics card's, and the system's commit headroom, in the log
    module_folder           Where this mod's files are: the folder the loaded DLL sits in (Mods/LocalSplitScreen/dlls),
                            and the mod's own folder above it (Mods/LocalSplitScreen); and the flag files a player puts
                            there
    module_pin              Keeps this DLL loaded until the process exits
    pool_bits               The counting behind platform/binned_pools.h, pure so it is tested on the host
                            (tests/pool_bits_test.cpp)
    rip_relative.h          RIP-relative addressing, as the mod's checks of the game's instructions compute it: an
                            instruction's operand is reached from the instruction's end (its start plus its length) plus
                            the signed 32-bit displacement it carries
    seqlock.h               N floats one thread writes and another reads (Boehm's seqlock): the writer bumps the version
                            to odd, stores the values, bumps it to even; a reader retries while it sees an odd version
                            or a version that changed under it
    settings                The mod's own settings file: LocalSplitScreen.ini in the mod's folder
                            (Mods/LocalSplitScreen/LocalSplitScreen.ini, one level above the dlls folder this module is
                            loaded from: platform/module_folder.h), read with the Windows …
    settings_write.h        The settings file (platform/settings.h) as the mod writes it: the whole text from the
                            options table (FileText), so every option, its words, its default and its help in the file
                            are the code's own and never a stale copy
    settled_log.h           When a state line is due, for lines whose state has two kinds of parts: steady parts that
                            change on events (a layout, a mode, a target) and moving parts that change every frame while
                            something animates (a camera's FOV through a …
    sparse_array.h          Walks the allocated elements of an Unreal TSparseArray (the element array of a TSet or TMap)
                            by reading memory only
    text.h                  Small text helpers with no home of their own
    thread_times            CPU time per frame of the engine's three frame threads (game, render, RHI), for the perf log
    tset_find.h             Looks a key up in an Unreal TSet or TMap keyed by an FName by the set's own hash, by reading
                            memory only, and walks its allocated elements as the check of that lookup and the fallback
    verified_code           Finding engine functions and vtable slots only where the research says they are
    vtable_entry            Replaces one entry of an engine vtable with a function of our own, and puts it back
    x64_jump.h              Encoding of the x64 jumps the mod writes: `jmp rel32` into game code (E9 + a signed 32-bit
                            displacement measured from the end of the 5-byte instruction), the rel32 itself (a call's
                            displacement is the same rule), and the absolute …
  facts/                    the engine facts two or more modules share, one declaration per address
    cameras.h               The End camera manager (AEndPlayerCameraManager) and its operators: classes, fields, the
                            spawn names its BeginPlay uses, SetViewTarget, and the slots the mod replaces
    command_controller.h    The field command controller (FieldCommandCtrl, EndUISystem[23]): the game's one, and each
                            seat's own, which the mod builds the same way (battle/command_menu)
    controller.h            The End player controller (AEndPlayerController): its class, fields, the functions and the
                            slot the mod uses, its leader-action component, and Square's input object behind it
    engine.h                The stock engine's facts: what the mod relies on in Unreal's own code and objects, with the
                            bytes that prove each one at startup
    field_action.h          The FieldActionMgr ([g_EndGameRoot]+0x1A0): the point registry's live list and lock, the
                            eight slots' rows, targets and latches, the pending flag, and a point's key, preset and
                            flags
    hud.h                   The HUD: the UI system (g_EndUISystem) and the slots two or more modules read, the End menu
                            renderer's layer slots and entries, the root overlay, and the user widget (its viewport
                            layout, SetPositionInViewport, the field-action …
    in_battle               The game's own in-battle test, found once for every module that asks it
    level_state.h           The level's state manager as the lead gate reads it: the state-change and state-trigger
                            rows, the active slots, the AI system's setting groups, the field icons the story shows, and
                            the AI states a level's walk or lead enters on a …
    menu.h                  The Co-op page's facts: the game's UI system, the pause menu's first page and its row
                            tables, the Options screen the mod opens as its page, and the helpers that write its data
                            (menu/*)
    move_state.h            The move states (a party member's movement machine): the base and player states, their
                            slots, the field-action rows they request, and the movement gate's per-character tests
    party.h                 The party: its manager, roster and leader, the characters and their AI, the tag registry,
                            and the game globals that exist once the game has started (the option holder, the battle
                            state)
    traversal.h             The field-action actors (End2FieldActionActorBase and its classes, the traversal: ladders,
                            squeezes, crawls, pole slides, lifting ropes, one-shot moves, climbing walls, monkey bars,
                            spider walls, zip lines, beams, wall runs, …
    vehicles.h              The rides: the vehicle coordinators and their kinds, the chocobo ride manager, the chocobo's
                            and the scooter's character fields, and their move states (the slots and the chocobo's
                            prompt rows)
    world.h                 The world-level managers: the game root (g_EndGameRoot) and the managers hanging off it, the
                            location volume manager, the animation world manager
  players/                  the engine's own paths: local players, reflection calls, console and settings access; the
                            split layout; each player's member generation, the log descriptions and the field reads
                            every module shares
    call_site.h             Checks for the build-specific paths that hear one caller of a game function some other way
                            than a plain call redirect: at the function's entry, by the caller's return address, or with
                            CallRedirect::InstallFromOwnThread
    console_access          The game's console through UKismetSystemLibrary's reflected functions: a console variable's
                            value, a variable set, a command run
    describe                Short descriptions of engine objects for log lines
    first_player_scope.h    A seat's controller first in the world's player controller list for one call, so "the first
                            local player" the game asks for is the seat's; player 1's order put back after
    generation              The seats' players, as every wrapper sees them: one generation per seat (players/seats.h)
    held_setting            A game setting held at the mod's value for a while, then put back: the console settings
                            two-player rendering needs (split_screen_settings.h), the two-player layout (split_layout.h)
                            and the three-player layout (mod.cpp, while …
    join_keys               The keys that express player intent: Ctrl+F7  add a player (2, then 3)   Ctrl+F8  remove the
                            last one in Ctrl+F5  top/bottom <-> side by side Ctrl+F4  halves that fill <-> halves kept
                            16:9 with bars Ctrl+F6  the seats' views: …
    local_players           A local player of the mod's, added to and removed from the engine's list of local players
                            through UE's own path, by controller id: a seat's (players/seats.h; seat 1 is player 2,
                            session/session.h), which is the seat, or a mirror …
    object_fields.h         A native engine object's fields at fixed offsets, under the mod's names (namespace lss,
                            every module)
    object_slot.h           One GUObjectArray slot, read through the loader
    seat_state.h            A seat's state in one word, derived from the session's record of the seat
    seats.h                 The seats: the human players the mod adds: seat 1 is player 2, seat 2 player 3
    single_view             The engine's single view: the whole screen player 1's view alone while the seats' players
                            are in (their controllers, characters and pads untouched)
    split_layout            Which way the screen is split for two players: top/bottom or side by side
    split_screen_settings   Engine settings several views need, held while more than one view renders and put back then
                            (each one a players::HeldSetting over the console; SplitScreenSettings holds the three, the
                            entry point owns it)
    split_settings_access   The engine's split-screen settings the mod reads and writes through reflection: the two- and
                            three-player layouts on UGameMapsSettings' class default object (the value the engine reads
                            every frame), the offset of …
    stage_counters.h        What each of the seats' wrappers did, counted for the periodic status line
    tracked_object.h        A UObject the mod remembers across frames, identified so that a reused address is not taken
                            for it: its GUObjectArray slot must still hold this address, not collected
    unreal_access           The engine objects the players module works with, reached through UE4SS reflection: guarded
                            calls, the game instance and its local players, a reflected static function, a controller's
                            viewport size, an FName's text
    weak_object             The engine's weak object pointers, resolved and made by its own functions
    widget_opacity          A UMG widget's render opacity, set through the game's own native UWidget::SetRenderOpacity:
                            it writes the widget's RenderOpacity (+0xC4) and, when the Slate widget is built and its
                            opacity differs, stores it there and invalidates …
  session/                  players 2 and 3's join, party member and leave; the lead gate and the ride-along
    field_exit              The seats' players leave before the game leaves the field, and rejoin once the field is
                            running again (research/RE-p2-retry-reload.md)
    field_hold.h            When a script needs a player-held party member back, and when it does not
                            (research/RE-held-member-scripts.md)
    handover_watch          Logs, read-only, what the member choice and the hand-over need; a dev tool (dev/dev_tools.h
                            installs and ticks it; a player's build has the empty stubs below, and keeps the party-order
                            Info line through party_member::ChooseSlot's …
    lead_gate               Whether a walk or lead the level wrote on player 2's or 3's member is needed finished (the
                            member goes to its AI and the player rides along) or only staging (the player keeps it),
                            from the game's own data
    lead_keep_rules.h       Whether another player keeps a member the level walks to its marks, and when the game takes
                            it over at a mark
    leave_sequence          The order in which a seat's player leaves, as a small state machine (tested on the host:
                            tests/leave_sequence_test.cpp)
    leave_watch             Logging only: what becomes of a seat's controller after a leave
                            (research/RE-p2-controller-after-leave.md)
    leftover_actors         The actors a seat's objects leave behind, destroyed by exact pointer once its player is
                            removed
    outside_join            A seat is out and wants back in: when the join is tried, as a pure state machine (tested on
                            the host: tests/outside_join_test.cpp)
    own_controller          A seat's own PlayerController: the camera manager's BeginPlay, made safe for a player beyond
                            the first
    party_events            The game's two party funnels, heard by the session: leader   the one call of
                            AEndPlayerController::SwitchLeaderInternal, inside OnLeaderTransition
    party_lease             The rules for lending a party member to a seat's controller, as pure functions over values
                            read from the game (tested on the host: tests/party_lease_test.cpp)
    party_member            A seat takes a party member, and gives it back
    party_switch            No player switches onto a party member another player plays; a player's own switch takes any
                            member no human holds
    party_switch_rules.h    The battle switch's rules in co-op (session/party_switch.h), as pure functions over what the
                            game reports; tested on the host
    player_window           Each player's own window: joining, and leaving, by pad (the rules:
                            session/player_window_rules.h; the text: session/player_window_text.h)
    player_window_rules.h   The pure rules of the player windows (session/player_window)
    player_window_text.h    What a player's window says (session/player_window; the rules:
                            session/player_window_rules.h)
    seat_out                A seat's player going out: every module that holds something of the seat's lets go of it, in
                            one place
    session                 The seats' players as their own players: join, party member, camera, and leave, in that
                            order, per seat (players/seats.h; seat 1 is player 2)
    session_join            Session: installing the stages, and a seat's join
    session_rules.h         The session's pure decisions (session/session.h), as functions over values read from the
                            game, tested on the host (tests/session_rules_test.cpp)
    state_path.h            A member's own chain of level states ahead of its current setting, walked as the game's data
                            lays it out
    tag_counts              How many times the game has registered an actor tag, for the join and leave log (a dev tool:
                            dev/dev_tools.h installs it; a player's build has the empty stubs below)
  scopes/                   one game call made as player 2 or 3
    leader_answer           The party-leader test answered for a seat (research/RE-p2-enemy-target-bias.md,
                            RE-p2-chocobo-ride.md, RE-p2-chocobo-run-refresh.md)
    leader_scope            The party leader answering a seat's member for one engine call (research/RE-p2-targeting.md)
    primary_controller      Running one engine call as if a seat's player (players/seats.h) were the primary player
    scope_ladder.h          A seat's scopes opened in one order for one game call, all or nothing
    scope_leaks             Putting back what the mod's two "as a seat's player" calls change outside that player's own
                            objects (RE-p2-scope-leak-audit.md, leaks 1, 2 and 5)
    seat_ladder.h           A seat's scope ladder over the modules' scope types (scopes/scope_ladder.h is the mechanism)
    swap_window             Temporarily replaces one 8-byte engine value for the length of one engine call, and puts it
                            back only if nothing else changed it meanwhile
    thread_hold.h           A game call made with this thread's answer held
  camera/                   each player's cameras
    attack_cameras          The game's attack cameras (battle camera sequences: grabs, enemy specials, bursts) on each
                            player's own camera (research/RE-p2-attack-cameras.md)
    attack_framing          Which player an enemy's attack camera shows (research/RE-p2-attack-cameras.md):
    camera_access           The lookups every camera module makes, once: a controller's End camera manager (checked by
                            vtable and by its owner field naming the controller back), its Field and Battle operators
                            (checked by vtable), the module in a slot of an …
    camera_distance         Every player's camera follows the Options camera distance (research/RE-p2-battle-camera.md)
    camera_override         Each player's camera plays only its own character's animation camera overrides
                            (research/RE-p2-battle-camera.md)
    camera_repair           A seat's camera with a non-finite pose gets the game's own player-warp camera reset
                            (research/RE-p2-chocobo-warp-camera.md, RE-p2-party-warp.md)
    field_camera            Each seat's camera (players/seats.h): the field camera's settings, and the seat's own camera
                            update
    framing_watch           Logs each player's field-camera framing while the split is shown (read-only; roadmap 2b)
    location_rows           Each player's camera takes location camera rows from its own character only
                            (RE-p2-framing.md: unscoped, the indoor row reaches every camera)
    location_volumes        The game's location volumes, walked for each seat (camera/location_rows.h: the camera row a
                            seat's character stands in; input/walk_limits.h: the move-type volumes it stands in)
    main_camera_shape       Split halves fill their part of the screen by the game's own camera setting
    near_fade               No seat's character (players/seats.h) is ever faded by the game's proximity fades
                            (research/RE-p2-camera-near-fade.md)
    seat_camera_effects     Each seat's member's own battle camera effects on that seat's camera (players/seats.h;
                            research/RE-p2-attack-cameras.md)
    shoulder_scale          The camera's sideways (shoulder) offset keeps its single-player look in a split half
    vault_camera            A seat's chocobo vaulting off a ledge no longer takes that seat's and player 1's ride
                            framing (research/RE-p2-chocobo-jump-camera.md, lever L1b)
    view_target             Pointing a seat's camera manager at its own camera actor
  input/                    pads, sticks and movement
    approach_own_camera     Every seat's player (players/seats.h) starts ladders and the other walk-up traversals from
                            any side, as player 1 does (research/RE-p2-ladder-approach-L2.md)
    button_view.h           Which player a merged button belongs to (pure logic, tested on the host)
    climb_context           Climbing walls, monkey bars and spider walls read their input as each climber's own (W5,
                            research/RE-p2-traversal-climb-context.md)
    member_movement         Moving each seat's party member with its own pad (players/seats.h)
    move_census             The movement census (a dev tool): which characters' move states run, how often, and how the
                            movement gate's per-character tests answer for each (input/move_state.h counts into it from
                            both tick slots; the session's status line …
    move_state              Move-state ticks: which characters' move states run, and with which pad
    pad_buttons             Each pad's buttons for its own player (RE-p2-per-pad-buttons.md)
    pad_presence            Which XInput users have a pad, asked of the game's own xinput1_3 once a second on the game
                            thread (the game's input system polls the same API every frame; a disconnected slot's query
                            is the slow one, so never per frame)
    pad_rules.h             The pure rules of each physical pad's input, as the pad table reads it (input/pad_table) and
                            the player windows hear it (session/player_window)
    pad_sticks              Each pad's sticks drive only its own player
    pad_table               Which physical pad answers each of the game's pad slots (research/RE-pad-slots.md)
    seat_walk_cap           Each seat's walk cap from a walk-only step ends when the step ends, as player 1's does
                            (players/seats.h; research/RE-p2-walk-mode.md)
    stick_select            Choosing one pad's sticks from Square's per-pad records, as a pure function (tested on the
                            host: tests/stick_select_test.cpp)
    walk_limits             Each seat's walk and run limits come from its own surroundings (players/seats.h;
                            research/RE-p2-walk-mode.md)
  feedback/                 rumble and sound
    combat_rumble           Combat rumble on a seat's own pad (research/RE-p2-rumble.md)
    listener_rules.h        The sound listeners' decisions as pure functions (feedback/sound_listeners.h), tested on the
                            host (tests/listener_rules_test.cpp)
    pad_rumble              Each player's rumble on their own pad (research/RE-p2-rumble.md)
    rumble_watch            Logging only: where each player controller's force feedback goes, pad by pad, frame by frame
                            (research/RE-p2-rumble.md)
    seat_ffb_managers       Each seat's own force-feedback manager (research/RE-p2-ffb-manager-per-pc.md)
    sound_listeners         Every player hears the sounds near them: each SEAD sound is measured from the nearest
                            player's listener
    traversal_rumble        Each player's traversal rumble plays on their own pad
                            (research/RE-p2-zipline-rumble-hook-who.md)
    traversal_rumble_code.h The bytes feedback/traversal_rumble_sources.cpp places at the 1.004 jump box's inline list
                            load (PlaceJumpListLoad): after the game's own load, on a thread a jump route named, the
                            rumble list the route named with it (the seat's …
    traversal_rumble_copy   The shared ride's copy (feedback/traversal_rumble.h): while player 1 drives the buggy or the
                            bronco and the seats stand aside, every add and stop on the game's list is copied onto the
                            list of each seat with a force-feedback …
    traversal_rumble_rules.h
                            The rumble list's pure rules (feedback/traversal_rumble.h), tested on the host
                            (tests/traversal_rumble_rules_test.cpp)
    traversal_rumble_shared.h
                            The rumble list's state its units share (feedback/traversal_rumble.h): the game's lock and
                            each seat's list, the add and the stop into one, the counts, the shared ride's flags and
                            copied keys, each seat's member type and the jump …
    traversal_rumble_sources
                            The rumble sources that run outside the seats' scopes (feedback/traversal_rumble.h): the
                            scooter move state's slots, the sky glide's delegate, the jump box's OnEnded, the drift and
                            brake actions
  battle/                   players 2 and 3's battle: menus, targets, the enemies' coin, synergies, the warps and the
                            rates
    arena_warp              Player 2's or 3's character brought into the fight on a double click of the left stick: a
                            character can be shut outside a fight (a crawlspace the game closes while it lasts), and
                            nothing in Rebirth brings a battle member in …
    battle_camera           Each seat's battle camera (RE-p2-battle-camera.md)
    battle_input            A seat's battle state, read and logged inside the seat's member tick scopes, before the tick
                            (a dev tool: Trace, on change; dev/dev_tools.h installs it, input/member_movement.h calls
                            it; a player's build has the stubs below)
    battle_rates            The battle's rates: enemy damage, enemy HP, boss HP, the enemies' stagger gauge, EXP and AP,
                            each a multiplier from 0.1 to 10.0 set from the Co-op page, the console or the settings file
                            (platform/settings.h)
    battle_rates_rules.h    The battle's rates (battle/battle_rates.h): the multipliers' words and the arithmetic, done
                            the game's way
    battle_warps            Which party members the game warps at a battle's start and end
                            (research/RE-p2-battle-gather-warp.md)
    battle_warps_rules.h    Who the game warps at a battle's start and end, as the setting decides it without the game
                            (battle/battle_warps.h applies it at the game's two calls)
    coin_table.h            The coins enemies hold (battle/target_coin.h): a lock-free table keyed by the enemy's
                            controller, each coin the player it names (1 for player 1, seat + 1 for a seat's player: 1
                            to 3, players/seats.h) and when it was tossed
    command_locks           Each seat's command controller gets the battle's command values through the game's own
                            setters (research/RE-p2-command-menu-levers.md)
    command_menu            Each seat's own battle command menu: another FieldCommandCtrl
                            (research/RE-p2-command-menu.md, RE-p2-command-menu-input.md)
    command_menu_shared.h   What the seats' command menus' units share (battle/command_menu.h): whether a game call may
                            be made again on a seat's controller, whether its stage is live, and the UI system's address
    command_menu_tick       Each seat's command controller ticked at the game's own spot, and that seat's battle actions
                            driven through it
    command_menu_tick_shared.h
                            What the command menu tick's units share (battle/command_menu_tick.h): the pass in flight
                            and the tick's reads the route and the tint need, the controller's request indices and their
                            reader
    command_menu_view       Each seat's command menu widgets per view (research/RE-p2-command-menu-views.md)
    command_rules           Who may issue commands to whom in the battle command menu (research/RE-p2-command-menu.md)
    command_rules_code.h    The machine code that answers "no" for one member in FieldCommandCtrl_IsMemberCommandable
                            (research/RE-p2-command-menu.md)
    enemy_leader            Enemies turn on a standing member when the game's leader is down in co-op
                            (RE-enemy-leader-reads.md)
    enemy_leader_rules.h    Whom enemies follow while the game's leader is down in co-op (battle/enemy_leader.h), as a
                            pure rule over what the game reports; tested on the host
    equal_targeting         Condition enemies favour every player equally (research/RE-p2-enemy-target-bias.md)
    focus_scope             Each seat's own UI focus context (research/RE-p2-command-menu-input.md)
    leader_reads            The game's own native reads of "the party leader" made for an enemy follow that enemy's coin
                            (research/RE-p2-leader-native-reads.md)
    lock_group_coin         Lock groups prefer player 1 or a seat's player by each picking enemy's coin
                            (research/RE-p2-enemy-target-bias.md)
    lock_group_rules.h      The lock groups' rules (battle/lock_group_coin.h) as pure pieces, tested on the host
                            (tests/lock_group_rules_test.cpp)
    lock_on                 Each seat's targeting: the game's own targeting tick run again, for that seat
                            (research/RE-p2-targeting-scoped-tick.md; the fields in RE-p2-lock-on.md)
    menu_focus              Player 1's UI focus, logged on change (research/RE-p2-command-menu-input.md)
    menu_input_route        A seat's pad's presses and releases to the seat's menu in battle
                            (research/RE-p2-command-menu-input.md)
    menu_rules.h            The command menu's pure rules (battle/command_menu_tick.h, command_menu.h), tested on the
                            host (tests/menu_rules_test.cpp)
    menu_tint               The tactical-mode tint (UI filter type 0) and the wait-mode sound, shared by every
                            controller's menu (RE-p2-command-menu-tint.md)
    panel_gate_code.h       The machine code that keeps a seat's targeting run out of player 1's target panel, at the
                            one panel request whose call site straddles a code block
    player_hits             The seats' hits, and the hits they take, count as the player's
                            (research/RE-p2-enemy-target-bias.md)
    shortcut_follow         The shortcut tables followed on the other controller (RE-p2-shortcut-refresh.md)
    synergy_partner         Synergy skills and synergy abilities with a human's character as the partner
                            (research/RE-p2-synergy-skills.md, research/RE-p2-synergy-abilities.md)
    synergy_rules.h         The synergy partner's decisions (battle/synergy_partner.h) as pure functions over values
                            read from the game, and the table it keeps, tested on the host
    target_census           How enemies choose whom to attack, and how often they choose each player
                            (research/RE-p2-enemy-target-bias.md)
    target_coin             Enemies aim their moves at one of the players, one coin per enemy action
                            (research/RE-p2-enemy-target-bias.md)
    ucpc_choice.h           Which player's character an enemy's Condition target selector counts as "the UCPC" (the
                            user-controlled player character) for one select
    widget_owner            Each seat's widgets tick in their owner's view (research/RE-p2-party-list-views.md)
  traversal/                climbing, vaulting and the rest of traversal
    chocobo_climb           A seat's player climbs chocobo walls with their own chocobo
    chocobo_spots           A seat's chocobo launches from the launch pads, jumps the jump boxes and takes the sliders
                            with its own rider
    hooks                   The grappling gun's points and the hook ropes for players 2 and 3
                            (research/RE-hooks-for-seats.md, RE-p2-zipline-rumble-hook-who.md)
    traversal_camera        The seats' traversal cameras (research/RE-p2-traversal-L3-ladder-camera.md,
                            RE-p2-traversal-cameras.md)
    traversal_collision     Logging only (Debug): every player's traversal camera's collision, frame by frame while a
                            traversal module holds slot 15: the frames the sweep pulled the camera in, with the climbing
                            wall's hip framing off and on, then the …
    traversal_exec          The executer classes of the seats' traversal cameras (traversal/traversal_camera.h): Start,
                            Tick and End run with the seat's camera handle in the actor and in the seat's scopes; the
                            ladder's exit blend, the squeeze's and crawl's …
    traversal_facts.h       The traversal cameras' facts and tables (traversal/traversal_camera.h): the executer classes
                            (ladder, squeeze, crawl, pole slide, lifting rope, one-shot move) with the fields and exits
                            the mod reads on them, the move classes …
    traversal_holders       The move classes of the seats' traversal cameras (traversal/traversal_camera.h): a seat's
                            move Begin installs on the seat's own holder for the actor, the game's holder functions run
                            on it, the holder check keeps it, the actor's …
    traversal_shared.h      The traversal camera's state its units share (traversal/traversal_camera.h): the "inside a
                            scoped call" flag, the executer and actor readers, each seat's last ladder module, and each
                            unit's placement, removal and tick, which …
    traversal_watch         Traversal for the seats' players, logging only (research/RE-p2-traversal.md,
                            RE-p2-chocobo-traversal.md)
  world/                    the world around each player: crowds, triggers, story boxes, pickups, shops, the catch-up
                            warps
    anim_views              Characters near a seat's player animate as the game animates them near player 1
                            (research/RE-p2-anim-optimisation-two-views.md)
    catch_up_move           The game's own way to put a character beside another on the field, as its battle gather
                            calls it (research/RE-member-story-boxes.md):
    charapop_centre         The game's NPC population follows every player: each NPC is scored from the nearest player
    charapop_facts.h        The CharaPop manager, its entries and a camera manager's pose, as the game's character LOD
                            reads them
    charapop_near           The characters a seat's player is near keep the detail the game would give them from that
                            player's own camera (research/RE-p2-charapop-per-view.md)
    charapop_rule.h         CharaPop's Light/Middle rule, as its evaluators apply it to one camera, for the entries a
                            seat's camera needs (world/charapop_near.h)
    charapop_watch          Logs, read-only, how the game's character detail treats the characters near the seats'
                            players (research/RE-p2-charapop-per-view.md)
    data_rows               The game's mapped data rows (StateChange, StateTrigger, Action, ActionGroup...): a row found
                            by its FName through the game's own accessor, and the mapped fields read out of it
    field_pickup            A seat's player picks up collectibles and materials with the game's own field prompts
                            (research/RE-p2-field-interact.md)
    field_rules.h           The field-interaction modules' pure pieces (vending, shop_talk, seat_triggers, scope_leaks),
                            tested on the host (tests/field_rules_test.cpp)
    interact_watch          Logs the game's field interactions (FieldActionMgr) around the seats' pickups
                            (research/RE-p2-field-interact.md)
    nav_strand              Logs when a seat's character is stranded off the loaded navmesh and asks the game's follow
                            warp to bring it back
    party_warp              Brings a seat's player back to the party through the game's own follow warp
                            (research/RE-p2-party-warp.md; RE-p2-chocobo-warp.md for the ridden chocobo)
    party_warp_rules.h      A player's ask to bring their character back beside player 1's (world/nav_strand.h, from L3
                            clicked twice): the double click and the "already beside" refusal as pure rules
    run_catch_up            A character player 2 or 3 walked past its own story box is brought back beside player 1's
                            character when the level's walk through that box goes to its AI
    seat_switch_volumes     A seat's player inside CharaPop switch volumes (research/RE-p2-shop-npcs-near-p2.md)
    seat_triggers           The seats' trigger pass (research/RE-p2-trigger-overlap.md)
    shop_talk               A seat's player uses a shopkeeper's shop with the game's own field prompt and shop
    story_box_rules.h       The rules of world/story_boxes, without the game: which of a held member's story-box entries
                            wait, when a waiting one fires, whether player 1's character has come to the box, and
                            whether a straight run passes through it
    story_boxes             Player 2's or 3's character never fires its own story box ahead of the story, and never
                            loses one (research/RE-member-story-boxes.md)
    strand_watch            When player 2's member counts as stranded off the loaded navmesh
                            (research/RE-p2-party-leash.md)
    vending                 A seat's player uses vending machines with the game's own field action and shop
                            (research/RE-p2-field-interact.md)
    world_actor_reach       The game's world actors (chests and the other WorldActorMgr items) pop in around a seat's
                            player too (research/RE-p2-world-streaming.md)
    world_rules.h           The camera and world modules' pure decisions (attack_framing, location_rows, walk_limits,
                            world_actor_reach, seat_switch_volumes, camera_repair, party_warp, traversal_camera,
                            camera_distance, shoulder_scale), tested on the host
  rides/                    chocobos, the wheelie and the shared rides
    chocobo_exits           A seat's chocobo leaves its glide or water jet in the seat's scope, whoever ends it
                            (research/RE-chocobo-exits-pads.md)
    chocobo_ride            Each seat's player rides their own chocobo (research/RE-p2-chocobo-ride.md)
    own_ride                The lease layer the two own rides share (rides/chocobo_ride.h, rides/wheelie_ride.h): a
                            seat's controller holds a vehicle through the seat's own coordinator, the member is her AI's
                            for the ride, and at the end the seat's …
    ride_rules.h            The rides' pure decisions (rides/chocobo_ride.h, wheelie_ride.h, shared_ride.h), tested on
                            the host (tests/ride_rules_test.cpp)
    ride_watch              Logs, read-only, the game's chocobo and buggy rides while a seat's player is in
                            (research/RE-p2-chocobo-ride.md)
    shared_ride             The buggy and the Tiny Bronco are each one vehicle for the whole party: while player 1 rides
                            one, the players take turns at it (one picture for a shared vehicle, no mirror images; the
                            same for the bronco, whose crossing the other …
    wheelie_ride            Each seat's player rides a Wheelie (SB scooter) of their own
                            (research/RE-p2-wheelie-ride.md, RE-wheelie-polish.md)
    wheelie_watch           Logs, read-only, the game's Wheelie (SB Scooter) system while a seat's player is in
                            (research/RE-p2-wheelie-ride.md)
  views/                    the view points, the whole-screen states, the streaming views, the columns and mirror views,
                            the window span
    axis_records.h          The players whose FOV rule the mod wrote (views/player_view_point.h): the value to put back,
                            the rule the mod wrote, and the value last seen (so each change the game makes is noted
                            once)
    column_layout           The view columns through the engine's own layout table: on a span of monitors of different
                            widths each column is its own monitor's width; the screen order and the one-window layouts
                            for three players
    column_rules.h          The pure rules of the engine's layout records for the view columns
    column_source           Where each column's view comes from, decided in one place each tick
    columns.h               The view columns: how many views the screen shows and whose each one is
    display_align           The monitors placed to make the row, for as long as the game holds it, and put back at the
                            leave
    display_config          Windows' display configuration as QueryDisplayConfig gives it: the outputs (paths) and their
                            modes, with the names that go with a path
    display_move            The desktop's monitor positions through Windows' display-configuration API, the one Display
                            settings itself uses, and the way back from a move by the record kept while they are moved
                            (views/row_journal for what it means, …
    game_window             The game's own window, reached through the engine: the lever the span (views/window_span)
                            pulls
    minimal_view_info.h     The FMinimalViewInfo fields the mod reads or writes, as ULocalPlayer::GetViewPoint fills the
                            struct in this build
    mirror_player           The mirror views: a local player of the mod's in every column no player has, showing player
                            1's picture, so the held columns are never black (the columns rule: a third column only
                            while the three-player monitor mode is on, and …
    monitor_list            The desktop's monitors as Windows has them now, for the mod's span (views/window_span) and
                            the setup app (setup/): each monitor's rect and effective DPI, its GDI device name, and an
                            id and a name from the display configuration
    monitor_picks           The monitors the player picked for the row, as the settings file keeps them, and the row
                            they give
    monitor_row             Which monitors a spanned game window covers: a row, one view per monitor
    player_view_point       Each local player's view point: its shape while the screen is split, and which camera each
                            seat's view shows
    record_file             The record of moved monitors as a file beside the settings file, and the process that owns
                            it
    row_journal.h           The record of a desktop the mod has moved, on disk from just before the move until the
                            monitors are back, so whoever finds it can undo the move: the guard process when the game
                            dies (setup/guard.cpp), the next start of the game …
    screen_widgets          The widgets the game puts on the screen, as it adds them to its root overlay
    span_desktop            The desktop as the span (views/window_span) reads it: every monitor, which one holds the
                            game window, and the monitors the settings file names found among them; with the row those
                            give (views/monitor_picks) and the moves that …
    split_fov_rules.h       The split FOV rule as arithmetic, apart from the engine so it is tested on the host
                            (views/split_fov.h applies it)
    stream_view_math        A camera as the game's streaming takes it (views/stream_views.h): location, unit forward
                            vector and horizontal field of view
    stream_views            Each split player's camera as a streaming view of the game's own (RE-p2-distant-lod-swap.md)
    view_shape              How a split player's view fills its part of the screen: the arithmetic on the
                            FMinimalViewInfo (views/minimal_view_info.h) and the FOV rule's values, kept apart from the
                            engine so they are tested on the host
    whole_screen            Whether the game shows something that takes the whole screen, from the game's own state: the
                            reason players/single_view.h turns the split off
    whole_screen_reason     Whether the game shows something that takes the whole screen: the decision, pure, over the
                            signals views/whole_screen.cpp reads each tick (views/whole_screen.h names the game state
                            behind each one)
    whole_screen_rules.h    Pure pieces of the whole-screen reads (views/whole_screen.cpp), tested on the host: which
                            location names are the title and the minigame-only maps, the Chadley manager's record check,
                            and the encounter states that are a battle
    window_span             The game's window spanned across a row of monitors, one full view per monitor, through the
                            engine's own window object
    window_span_rules.h     The window span's pure rules, tested on the host (tests/window_span_rules_test.cpp)
  hud/                      the HUD in each view: the passes, the band, the minimap, prompts, markers and pins, the
                            battle HUD
    battle/enemy_gauges     The enemy name and life gauges (EnemyStatus_C) placed in each player's view (Stage B3b;
                            research/RE-p2-enemy-markers.md, RE-p2-bar-targeted.md)
    battle/enemy_gauges_code.h
                            The machine code that records, as the game's enemy-status tick runs, each untargeted gauge's
                            placement call (research/RE-p2-enemy-markers.md)
    battle/placement_record.h
                            Capture-and-replay of the game's placement calls, written once (hud/battle/popups_code.h
                            records battle popups, hud/battle/enemy_gauges_code.h the untargeted enemy gauges)
    battle/popups           Battle popups (damage and heal numbers, battle texts such as Dodge, UMG popups such as
                            level-up) placed in each player's view (research/RE-p2-battle-popups.md)
    battle/popups_code.h    The machine code that records, as the game places them, each battle popup's placement call
    battle/target_views     Each view's target marks follow its own player's target (research/RE-p2-hud-targets.md)
    context_owners.h        Whose player context each HUD root carries (hud/hud_roots.h), so a switch calls the game's
                            setter only on the roots whose player changes
    field/action_icon_base  The field-action icons ("Call Chocobo" and the other prompt rows, UEndFieldActionIcon /
                            FieldAction_Command_C) keep their single-player corner in a split half, by the game's own
                            layout
    field/action_rows       The field-action row list, read and laid out once for its two users
                            (hud/field/prompt_rows.h: each player's own rows; hud/field/action_icon_base.h: the rows'
                            base in a split half)
    field/field_markers     Each view shows only its own player's field markers: the field-action point markers and the
                            scooter coordinators' auto-ride icons
    field/jet_gauges        The chocobo's water-jet gauge in each view: a seat's chocobo's gauge placed from the seat's
                            view and shown there only (research/RE-chocobo-exits-pads.md)
    field/marker_timers     Each view's own linger timer for the off-screen enemy arrows (OutOfScreenTarget_C;
                            research/RE-p2-enemy-markers.md)
    field/prompt_rows       The field prompt rows (FieldAction_Command_C): each player sees their own, in their own view
                            (research/RE-p2-field-prompts.md)
    field/search_pins       The field search pins in each view: placed from that view's camera, and only that view's
                            player's own (research/RE-p2-ranged-reticle.md, RE-p2-world-markers.md)
    field/wrangle_targets   The chocobo wrangle's targets in each view: placed from that view's camera
    hud_roots               The HUD's root widgets and whose they are (lever L1 of hud/passes/seat_hud_content.h)
    hud_rules.h             The HUD modules' pure decisions (hud_placement, search_pins, marker_timers, minimap,
                            target_views, enemy_gauges), tested on the host
    hud_trace               Logging only: how the HUD's content is made each frame, and for which player
                            (research/RE-p2-hud.md)
    hud_updates             The HUD's view-dependent updates that can run more than once a frame, for
                            hud/passes/seat_hud_content.h: the lists at EndUISystem+0x98, +0xB0, +0x150 (markers),
                            +0x188, +0x170, +0x180, then the leader-target icon at +0x140, in …
    layout/arrow_edge_code.h
                            The machine code that picks the screen edge an off-screen enemy arrow pins to by where the
                            ray from the centre actually leaves the half, while the HUD is laid out in a split half
    layout/centered_frame_code.h
                            The machine code that makes the HUD's "centred 1920x1080 frame" the whole layout while the
                            HUD is laid out in a split half (research/RE-p2-overlay-ui-size.md)
    layout/hud_placement    Where the HUD's main widgets sit in a split half: as in single player, each in its corner or
                            edge of the half: compass top centre, command menu and button prompts bottom left, the
                            Members panel bottom right; and the World Intel …
    layout/layout_scale     The HUD overlay's layout scale by the game's own contain rule
    layout/layout_scale_code.h
                            The bytes hud/layout/layout_scale.cpp places over the overlay's layout-scale instruction:
                            the contain rule, min(height/1080, width/1920), with the game's own constants
    layout/overlay_band     Where Square's HUD must be laid out so that a split view shows all of it, and the host scale
                            its world markers must use there
    layout/overlay_layout   Lays Square's HUD out inside the part of its UI image a split view shows, so each half shows
                            the whole HUD with its world markers on their objects (hud/layout/overlay_band.h has the
                            arithmetic)
    layout/viewport_placement
                            Where a HUD root goes in a split half (pure; no game access)
    lead_mark               Player 2's or 3's own marker for the walk they keep: the game's own main-objective diamond
                            and minimap pin at the walk's stop, in that player's view only
    minimap/column_area_icons
                            Each further column's own area-map icon manager (research/RE-p2-hud-icon-manager.md)
    minimap/label_relayouts Which text blocks the game lays out again, where in the frame, and why (logging only;
                            research/RE-perf-solo-vs-split-2026-10-03.md)
    minimap/label_states    A seat's own copy of each minimap label's text and layout, one book per seat
                            (research/RE-p2-minimap-cost.md)
    minimap/minimap         Each seat's own minimap: its centre, rotation and icons, in its view
                            (research/RE-p2-own-minimap.md)
    minimap/minimap_icons   Each player's minimap keeps the icons another player's minimap needs
                            (research/RE-p2-minimap-cost.md)
    minimap/minimap_labels  Each player's own minimap label text and layout (hud/minimap/label_states.h;
                            research/RE-p2-minimap-cost.md)
    minimap/minimap_once    The minimap's map rendered once per frame, by player 1's HUD pass (RE-oom-path.md, "The 32
                            KB leak")
    minimap/minimap_once_code.h
                            The machine code that lets a seat's HUD pass skip the minimap's own map render
                            (research/RE-oom-path.md)
    passes/column_hud_images
                            The further columns' HUD images: the game's own overlay rendered once more each frame per
                            further column with a pass, each into a layer entry of its own in the same pool-2 slot, and
                            each view family drawing only its own column's …
    passes/entry_filter_code.h
                            The machine code that gives each further column's HUD entry its own buffers and lets each
                            view family draw only its own column's entry
    passes/first_player_code.h
                            The machine code that lets a seat's overlay pass ask "the first game player" and get the
                            seat's player (research/RE-p2-hud.md)
    passes/menu_buffers.h   A second set of the menu renderer's vertex and index buffers, for a layer entry drawn beside
                            the game's own in one slot (hud/passes/column_hud_images.h: the overlay's pool 2;
                            hud/minimap/minimap.h: the minimap's pool 0)
    passes/overlay_pass.h   Which pass of the overlay render is running, by the column whose view it draws: first one
                            extra render per further column with an image this frame, highest column first (the mod's,
                            hud/passes/column_hud_images.h), then the game's …
    passes/pass_opacity     Widgets taken out of one overlay pass's image: the other players' prompt rows
                            (hud/field/prompt_rows.h), point markers (hud/field/field_markers.h), search pins
                            (hud/field/search_pins.h), chocobo jet gauges …
    passes/root_overlay_render
                            The moment just before the HUD overlay renders, each frame
    passes/seat_hud_content Each seat's content in its HUD image: around the seat's overlay pass
                            (hud/passes/column_hud_images.h), the game's own answers to "which player" are switched to
                            the seat, and the game's own view-dependent HUD updates are run again …
    passes/shared_slot      Each further column's layer entry beside the game's own in one menu-renderer slot: the root
                            overlay's pool 2 (hud/passes/column_hud_images.h) and the navimap circle's pool 0
                            (hud/minimap/minimap.h), one instance each
    passes/ui_effect_pass   The UI particles (the summon gauge's glow, every Niagara effect whose render target is the
                            composite foreground) placed and sized per column
    position_probe          A logging probe on how HUD widgets place themselves from Blueprints: the field command menu
                            and its party switcher are children of the HUD overlay, but no native code sets their
                            position, so their Blueprints do
    user_widgets            The live UUserWidgets, kept current without walking every UObject
    view_prompt             A few lines of text in one player's own view: the player windows where the game's own window
                            cannot be shown
    view_window             A player's window in the game's own message window, in that player's view only
                            (session/player_window; research/RE-message-window.md)
    widget_dump             The dump commands (a dev tool): what the HUD's widgets report about themselves, and which
                            player the HUD is built for, through the engine's own reflected functions
    widget_visibility       A Slate widget's visibility set as the game sets it (SWidget::SetVisibility, an unbound
                            attribute), for the HUD modules that show or withhold a game widget in one view's paint
                            (hud/minimap/column_area_icons, hud/lead_mark)
  renderer/                 one view family per player, and the renderer's performance levers
    additional_family_output
                            Makes the game honour bAdditionalViewFamily again, so a later player's family keeps the
                            earlier players' parts of the screen instead of clearing them
    async_compute           The GPU's async compute queue on for the session (RE-perf-me-shadows.md, Phase 2, option A)
    before_device           Whether the game has created its D3D12 device yet
    family_boundary         Between the players' view families, the game's own end-of-frame state reset, in RHI stream
                            order (research/RE-p2-main-menu-crash.md)
    family_boundary_list.h  The RHI command-list and context layout the family boundary relies on (family_boundary.h;
                            research/RE-p2-main-menu-crash.md)
    me_feedback_once        One streaming-feedback hand-in per frame for the game's scenery ("ME") streaming, as with
                            one player (research/RE-p2-distant-lod-swap.md)
    me_shadow_feedback_code.h
                            The machine code that lets an additional family skip the ME shadow pass's streaming-feedback
                            writes (RE-perf-me-shadows.md, Phase 3, option D; performance work)
    one_family_per_view     Submits a multi-view family to the renderer as one family per view
    output_load_action_code.h
                            The machine code that restores stock Unreal's rule for the final output's load action
    residency_off           The D3D12 residency manager off for the session (research/RE-perf-submission-path.md, option
                            A)
    scene_view_family.h     The few FSceneViewFamily and FSceneView fields the mod reads or writes, with the evidence
                            for each
    submission_census       Diagnosis: a census of the game's GPU submissions (research/RE-perf-submission-census.md)
    submit_hints            Split screen without the per-parallel-pass GPU submissions
    view_family_submission  Routes the game viewport's view family through SubmitOneFamilyPerView
  menu/                     the Co-op page: a row in the pause and title menus opening a page of the game's Options
                            screen
    coop_menu               The Co-op row of the game's pause main menu and the page it opens
    coop_menu_rules.h       The Co-op page's rules: what the game's Options screen shows when the mod opens its own copy
                            from the Co-op row of the pause menu or the title menu
    coop_page               The Co-op page itself: the mod's own instance of the game's Options screen, filled with the
                            settings table's options, and its value reads and commits
    coop_page_open          Opening and closing the Co-op page over the pause main menu, as the game shows and closes
                            its own windows
    coop_slider             The Co-op page's sliders: an option with more values than the game's longest list (its value
                            list cannot scroll) is the game's own Range row (menu/coop_page builds it;
                            coop_menu_rules::IsSlider), made to scroll
    game_ui                 The game's UI pieces the Co-op menu's modules share: the UI system's managers, the main
                            menu's first page, weak pointers held in the game's widgets, and FStrings read and copied
                            the game's way
    title_row               The Co-op row of the title menu, directly below Options, so a player sees it on starting the
                            game
    title_row_rules.h       Where the title menu's Co-op row goes among its rows' kinds: directly below Options, so the
                            rows through Options keep their places
  control/                  the console: the command language, the runner and the socket (built with the dev tools)
    remote                  The console: the socket (control/server.h) on 127.0.0.1:kPort and the runner over the mod's
                            registry, joined on the game thread
    runner                  Runs the control commands against the registry
    script                  The control language: one command per line, UTF-8, '#' starts a comment
    server                  The console's socket: a listening TCP socket on 127.0.0.1 and one thread that reads lines
                            from every client into a queue and writes replies out
    stick_holds             The control runner's stick holds on the running mod: a scripted hold is a per-pad stick
                            override (input/pad_sticks.h) on the held player's controller id, so it takes a real pad's
                            path
  dev/                      the dev tools: the console's install, the watches' install and ticks
    dev_tools               The development tools, behind one switch: the console (control/), the HUD trace
                            (hud/hud_trace.h), the position probe (hud/position_probe.h), the dump commands, and the
                            watches that only log: the rumble, ride, Wheelie, framing, …
tests/                      106 host-side test programs (ctest), one per pure header or code stub; run without the game
tools/                      the control scripts, the regression harness, the version match, this file
```

`src/` is searched before UE4SS's include folders, and Windows ignores case, so a mod header named like a UE4SS one
under a folder of the same name (`src/input/Handler.hpp` against UE4SS's `<Input/Handler.hpp>`) would replace it;
and a file in `src/hud/` that includes `"battle/x.h"` finds `src/hud/battle/x.h` before `src/battle/x.h`. Keep the
mod's headers `.h` and their names distinct from those.

Engine facts (addresses, vtable slots, field offsets) are written as IDA shows them for the research
build, each with the bytes that prove it and the research document that established it. The ones two
or more modules share live in `src/facts/`, one declaration per address, grouped by engine object
(`facts/engine.h` is the stock engine's); a fact one module reads stays in that module. The startup
facts (`facts::kStartupFacts`) are checked before anything is installed: if any fails, the mod installs
nothing and says why in `UE4SS.log`. Every other fact is checked where its feature installs
(`platform/verified_code.h`), and that feature alone stands aside.

## Game versions

One DLL serves every supported game version (1.004 and 1.005). At startup `GameImage` reads the
exe's PE header and logs the version it found.
- The source keeps the research build's (1.004) addresses. For other builds,
  `platform/build_translation` maps them through `platform/builds/build_<ver>.h` (generated by
  `tools/version_match/`) and `build_<ver>_replaced.h` (found by hand in IDA).
- Field offsets that moved are `PerBuild` constants (`platform/build_value.h`).
- An unknown build gets a warning. Its checks then fail, and those features stand aside.

Adding a new build is described step by step in `tools/version_match/README.md`.

## Building

This is its own CMake root. It builds RE-UE4SS from the checkout at
`../_tools/UE4SS_CppMods/RE-UE4SS-c0a8d4c` (override with `-DUE4SS_ROOT=<path>`): the source of the UE4SS build
players get from Nexus Mods (Git SHA `c0a8d4c`), so the mod loads under that loader and players install nothing
else. `tools/ue4ss_nexus/README.md` makes that checkout and names what it holds; `tools/ue4ss_nexus/check_exports.py`
checks a built DLL against the Nexus loader's exports. If the configure cannot pick a Rust toolchain for corrosion,
add `-DRust_TOOLCHAIN=stable-x86_64-pc-windows-msvc`.

```
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Game__Shipping__Win64
cmake --build build --config Game__Shipping__Win64 --target lss_tests
ctest --test-dir build -C Game__Shipping__Win64 --output-on-failure
```

The first build compiles UE4SS and takes a while; later builds only compile this mod. Each test is
its own executable (`tests/<name>_test.cpp`, one `lss_test()` line in `CMakeLists.txt`); the
`lss_tests` target builds them all, since ctest runs only what is built. `tools/build_locked.ps1
-Tests` does both under the build lock. One test is a script: `tests/facts_once.py` walks `src/`
and fails if an engine address is declared in two files, or a module checks a function's bytes
itself instead of declaring a `FunctionFact` and finding it (the facts rule).

The development tools are built in by default: control scripts, the HUD trace, the position probe
and the dump commands (`src/dev/dev_tools.h`). A player's build leaves them out:

```
cmake -S . -B build-release -G "Visual Studio 17 2022" -DLSS_DEV_TOOLS=OFF
cmake --build build-release --config Game__Shipping__Win64 --target LocalSplitScreen SetupApp
```

The player's build keeps the settings table's command entries, which the Co-op page sets its rows through; the console
and every other command are left out. `python package.py` runs the host tests, builds the player's build and zips it
into `dist/` (`dlls/main.dll`, the setup app, `enabled.txt`, `PLAYERS.md` as its README); `--no-build` zips what is
built. `PLAYERS.md` is the players' guide.

## Installing

Copy `build/Game__Shipping__Win64/LocalSplitScreen.dll` to
`<game>/End/Binaries/Win64/ue4ss/Mods/LocalSplitScreen/dlls/main.dll`, and in `Mods/mods.txt`
enable `LocalSplitScreen : 1`. Enable only one split-screen mod at a time. Close the game first:
it holds the DLL open while running. Copy `build/Game__Shipping__Win64/MultiMonitorSetup.exe` (the setup app, built
with the mod; also the guard that puts moved monitors back) to `Mods/LocalSplitScreen/`, beside the settings file.
`tools/relaunch.ps1 -Deploy -Label <name>` does both and sets an open copy of the app aside.

The monitor row, the record of moved monitors and the setup app are shared with the FF7 Remake mod: `tools/remake_drift.py`
lists the files kept identical and the adapted ones, and says which have drifted from Remake's.

Do not deploy the `UE4SS.dll` this build produces; the game keeps its installed UE4SS, which is the Nexus Mods
"UE4SS-Rebirth_FFVII_ReMenu (1.005 Compatible)" upload with its own `UE4SS_Signatures` (`tools/ue4ss_nexus/README.md`:
the loader refuses the signature scripts written for newer builds).

## Not to be confused with

`../SplitScreen/` is RebirthLocalCoop, a separate project kept as evidence only. None of its code
is used here.
