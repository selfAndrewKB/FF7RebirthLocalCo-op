# AGENTS.md

Rules for anyone, human or coding agent, changing this mod. LocalSplitScreen is local split-screen co-op for up to three
players in FINAL FANTASY VII REBIRTH, a UE4SS C++ mod. The README covers what the mod does, its layout, and how to build
and install it; `PLAYERS.md` is the players' guide. The design notes and research are in the documentation repository
(`../Documentation` beside this one in the maintainer's workspace).

If a rule here conflicts with an explicit instruction from the maintainer, follow the maintainer and say which rule
you set aside.

## The rules that matter most

1. **Drive the game's own systems.** First decide the co-op result: normally the single-player look and behaviour in
   each player's view. Then reach it through the levers the game itself uses (its setters, slots, input handlers,
   layout rules).
   - Never recreate, re-implement or bypass game behaviour. That means no polling input the game already delivers,
     no writing state the game writes itself, and no parallel driver beside the game's.
   - A recreation is allowed only when no game path exists, and the reason goes into the research doc and the plan.
   - Exception: frame-rate work may change anything, but only after IDA shows the setting is read on the live path
     and a measurement shows it helps.
2. **Single player is the reference.** Compare with single player before changing anything a player sees.
   - Match the cause, not just the outcome: something that happens on a button press in single player must happen
     on that player's press, not because the mod acted alone.
   - Keep the game's animations, fades and slides; never skip them unless the animated path provably cannot run.
   - Never cut a line of dialogue.
   - Cameras only adapt to split screen; no reframing such as centring.
3. **Never trust an address or offset you have not verified.** Every engine address is a declared fact (`src/facts/`,
   a `FunctionFact` or `SlotFact` with its entry bytes), found and byte-checked at install.
   - `tests/facts_once.py` fails if an address is declared twice, or if a module checks bytes itself.
   - Never write to a struct offset worked out only on paper: read it live first.
   - Addresses are per game build. The source keeps 1.004's; 1.005's come through `platform/builds/` (see
     `tools/version_match/README.md`).
   - Never carry an address over from the FF7 Remake mod or from an older build.
4. **Fail safe, whole or none.** A feature whose checks fail logs why and stands aside; it never crashes the game or
   half-installs.
   - Place a group of redirects that only work together all at once, or not at all.
   - Read game memory through the guarded helpers (`platform/guarded.h`). A stale pointer must fault safely, not
     crash.
5. **Log first, then build.** Add logging, run the game, read the log, then write the fix. Log levels:
   - Info: state changes;
   - Debug: one line per event;
   - Trace: per frame or per input.

   Too many lines means the wrong level: fix the level. Never delete logging or throttle it. The development
   watches (`LSS_DEV_TOOLS`) stay.
6. **Done means seen working in the game.** Before calling something done:
   - run it in the game (keys, console, screenshot, the log);
   - quote the log lines that show it worked;
   - use a test that would fail if the feature were broken (for example its own A/B, or per-pass state logs).

   If you could not check it live, say "unverified", plainly.
7. **Three players, every combination.** Code is indexed by seat, never written for "player 2" alone. Any change
   must hold for:
   - two and three players;
   - one window and a row of monitors;
   - each layout.

## Names and words

- In code, a player the mod adds is a seat:
  - `seat` + the thing (`seatController`, `seatMember`, `AnySeatMember`);
  - `...One` or `playerOne` for player 1's (`operatorOne`);
  - `column` (or `view`) for what exists once per picture on screen, whoever owns it. A mirror view has a column but
    no seat.
- Never use "further", "second" or "two" in names for an added player.
- Text a player reads (log lines, console help, README, the Co-op page) uses plain words: "player 2", "players 2 and
  3", "another player". Never "seat", never "further".
- The FF7 Remake mod (`../../Remake/LocalSplitScreen`) is the sibling project and has led on seats, menus, pads and
  the monitor row.
  - Before building or renaming a system, read Remake's version and take its design and its words.
  - Deviate only where Rebirth clearly works differently, and write the reason down.
  - Never copy its addresses.

## Code

- **One helper, not copies.** Before writing a reader or utility, search for an existing one (`players/unreal_access`,
  `players/object_fields`, `platform/`).
  - A second copy is a second point of failure.
  - Delete dead code rather than keep it "just in case".
- **Nothing kept by hand that restates the code.** Generate it from the source or drop it. The settings file is
  written by the mod from `src/platform/settings.h`. The README's Layout section is made by `tools/layout_doc.py` and
  checked by a test.
- **New features ship on by default**, with logging. Options go in the settings table, not in ad-hoc toggles.
- **Comments** say what the code does and why, in plain sentences that stay true as the code ages. A comment has no:
  - dates, version numbers or history ("used to", "now", "until");
  - quotes of or credit to people;
  - mention of the Remake mod;
  - raw addresses (name the game's function; the address lives in `facts/`);
  - research section numbers.

  It may name a research file and mark a claim `[CONFIRMED]` or `[ASSUMPTION]`.
- **Never edit comments or names with a regex or script sweep.** Read each site and edit it by hand; a pattern
  cannot tell which uses are meant.
- Look up a real API's signature before calling it. Never write code against an API you guessed.
- Line endings: `CMakeLists.txt` is CRLF and most sources are LF. Keep each file's existing endings.
- UE4SS object searches (`FindFirstOf`, `FindAllOf`) cost about 20-25 ms in this game: never per frame. Cache the
  result and check it is still alive.

## Build, test, commit

- Build with `.\tools\build_locked.ps1 -Tests` (the development DLL and the host tests).
- The player's build is configured with `-DLSS_DEV_TOOLS=OFF`; `python package.py` builds it and makes the release
  zip.
- Regression: `python -I tools\regression\run.py`, or add `--build release` for the player's build.
  - Before a run: the game closed, port 4419 free, at least 17 GB of free commit memory.
  - No builds during a run.
  - Scripts that need the dev console are marked `# needs: control`.
- Have someone else review every change before it is committed, and fix the findings first.
- **Commits:**
  - Make them by hand, one step each: read the diffs, stage the files by name, build each step.
  - Never make a commit script.
  - Title a milestone `vX.Y.Z: <a plain sentence about what changes for the player>` and tag it. Bump the version
    in `src/mod.cpp`.
  - Push only when the maintainer asks.

## The game

- Close the game before copying a DLL: it holds `main.dll` open. Use `tools\relaunch.ps1 -Deploy` (`-NoLaunch` to
  install only). Close the game at the end of a task.
- Deploy only this mod's DLL, never the `UE4SS.dll` this build produces. The game keeps the Nexus Mods UE4SS build the
  mod targets (see `tools/ue4ss_nexus/README.md`).
- Never turn on the D3D12 debug layer or GPU validation with the mod loaded: creating the device fails.
- A launch that silently does not start is usually Steam waiting (a cloud-save conflict, a dialog, a stuck shader
  update). Read Steam's `logs/console_log.txt`. Do not keep relaunching.
- Crash dumps go to `Documents\My Games\...\Saved\Crashes`, not the game folder.
- Never move or rename the game's files or other installed copies of it.

## Reverse engineering

- Work in IDA. Read `research/RE-ida-database-health.md` (in the Remake docs) before trusting a database or a backup.
  A tool result that looks wrong means stop and report, never a footnote.
- Rename functions and comment in IDA as you go. Comments record what was read, never what is proposed.
- Save decompiles beside the research doc that uses them.
- Never convert number bases by hand.

## Documentation (the documentation repository)

- `plan/README.md` is the front page: focus, next step, backlog, and an index of topics.
- One file per topic in `plan/topics/`:
  - **State** on top: rewrite it whenever things change.
  - **History** below: dated entries, appended, never reworded.
- Research goes in `research/RE-*.md`. For each system record:
  - the mechanism, and how it was found;
  - what is stock Unreal Engine 4 and what is Square Enix's `End*` code;
  - which addresses belong to which build.

  Mark every claim `[CONFIRMED]` (decompile, live test, or two sources), `[ASSUMPTION]` or `[UNKNOWN]`. Never build
  on an assumption as if it were confirmed.
- Update the docs in the same piece of work as the code. A stale document misleads; a missing one only slows you down.
- In plans and reports, describe what happens to the player and their character, in plain words, not the project's
  internal terms.

## For coding agents

- Read before you edit, every time. Do not rely on a summary of a file you have not opened.
- When the code and a doc disagree, the code and the log are right. Fix the doc and say so.
- Never guess a cause. Name it from evidence (the log, a decompile, a measurement), or call it unknown.
- Ask the maintainer about facts in the game world (what is near the save, what the player saw) before building
  logging to find them out.
- Split large mechanical work into slices of about eight files.
  - Commit before handing a slice out, so the diff shows only that work.
  - Each worker appends to a progress log after every file, so work survives a cut-off.
  - Check the result against the diff and greps, not against the worker's report.
- Report outcomes as they are: failures with their output, skipped steps as skipped.
