# Changelog

## 1.1.2
Built on top of the original author's 1.0.4, keeping all of its changes: the out-of-process dump helper
(`TheWatcherDump.exe`), saving the report before slower steps, the stuck-capture guard, SAME/CHANGED/INCOMPLETE
sampling labels, sampling only the main thread unless `bAllThreadStacks=1`, backing up logs for the whole game
session, SE 1.5.97 support, and the safer default thresholds. Everything below is added on top of that.
Every new feature has its own INI switch, and the defaults are safe.

### Why
A real stall capture from 1.0.x recorded the wrong moment. It said "main thread did not finish a frame for 4.1s",
but the heartbeat was only 11 ms old when the stack was taken, because the game had already recovered. So the
stack and the minidump showed normal gameplay, not the freeze. A stack taken only at the moment the threshold is
crossed misses any stall that ends near that threshold. Hitches of 1-2 s left no stack at all.

### Added
- **Stack history during slow moments** (`[Sampling]`). While a frame is running long (`fHitchSampleSeconds`,
  default 0.5 s), or a loading screen has had no progress for `fLoadGapSampleSeconds` (default 2 s), the main
  thread's stack is sampled every `iSlowSampleIntervalMs` (default 100 ms). When the episode ends, a summary goes to
  `TheWatcher.log` and one JSON line goes to `TheWatcher\slow_<date_time>.jsonl`. The summary has the most common
  stack, the most common first frame, the share of samples each module appears in, and a signature: the top 3
  non-system frames, written as Address Library IDs so it stays stable across game versions. Hitches are reported
  when they last at least `fHitchReportSeconds` (default 1 s), up to `iMaxSlowReportsPerSession` per session.
  Every stall capture includes the history collected so far.
- **Recovered captures** (`bSkipDumpIfRecovered`). If the game is running again by the time the stack is taken,
  the capture is labeled RECOVERED and points to the history section as the evidence. It skips the minidump, the
  all-thread pass and sampling, and it does not count toward `iMaxCapturesPerSession`.
- **Where the player is.** Worldspace, exterior cell grid, player position and location name, in the CSV, in
  reports and in the events log. Exterior cells have no name, so 1.0.x left `cell_name` empty outdoors.
- **Loading screen kind.** `save load`, `new game`, `fast travel` or `transition` (doors and other cell changes),
  shown in the log lines and the events log.
- **Actor breakdown** (`bActorBreakdown`). Mid-high and low process counts, the number of runtime-spawned
  (FF-prefixed) high-process actors, and the top plugins by actor base, overall and for spawned actors only.
  Leveled spawns are traced back to their original leveled base.
- **Papyrus load** (`bPapyrusStats`). Running script stacks, stacks waiting on latent calls, queued function
  messages and the VM's overstressed flag. These are read without taking the VM's locks, so they still work during
  a hang. Reads are SEH-guarded and range-checked, and an unreadable value is reported as -1.
- **VRAM** (`bVramStats`). Per-process local video memory use, the OS budget, and non-local use (video memory
  spilled into system RAM), from DXGI. The busiest adapter is reported.
- **Events log** (`bEventsLog`, `bEquipEvents`, `bEquipAllActors`). `TheWatcher\events_<date_time>.log` gets one line per load,
  menu open/close, cell/location change, save, save load, new game, fast travel, player equip/unequip, hitch
  signature, stall warning and capture. Each line is flushed immediately, so the last lines before a crash survive
  it and can be lined up with the crash logger's timestamp.
- **Open menus** are tracked and included in every report, CSV row and status line.
- **`summary.json`** in every stall folder: a machine-readable version of the capture (reason, recovered flag,
  signature, context, history, main stack, sampling result, thread groups).
- **Grouped thread stacks** (`bGroupThreadStacks`). Identical stacks print once as `[THREAD xN]`. One real capture
  had 320 idle threads, printed one by one.
- **Live status file** (`bStatusFile`). `TheWatcher\status.json` is rewritten every second (temp file + rename):
  the state (`running` / `hitching` / `stalled` / `loading` / `loading_stuck` / `unfocused`), heartbeat age, load
  number, counters and a context line. It can be read from disk while the game is hung.
- **New stats CSV columns**, appended after the original 24, so existing readers keep working:
  `worldspace, cell_x, cell_y, pos_x, pos_y, pos_z, location, open_menus, mid_high_actors, low_actors,
  spawned_actors, actor_plugins, spawned_plugins, papyrus_stacks, papyrus_latent, papyrus_queued,
  papyrus_overstressed, vram_local_mb, vram_budget_mb, vram_nonlocal_mb, slow_episodes, slow_max_s`
- The capture's log backup also copies this session's `events_` and `slow_` files.
- `tests/unwind_test.cpp`: a standalone check that the new stack walk gives exactly the same frames as the old one.

### Changed
- **Safer stack walking.** A thread is now suspended only for `GetThreadContext` plus one copy of its stack
  (up to 256 KB, no locks). The unwind then runs on the copy after the thread is resumed, with every register that
  pointed into the real stack redirected into the copy. 1.0.x unwound the live stack while the thread was
  suspended, which could deadlock if that thread held the loader or function-table lock. That risk would have
  grown with sampling during hitches. `tests/unwind_test.cpp` compares both methods at depths 5 to 400, including
  frame-pointer functions, and gets identical frames.
- **Module shares ignore the frame-hook chain.** Every main-thread sample also contains the main loop and every
  plugin chained on the same `Main::Update` call. Those used to show about 100% in every report. Each sample is now
  cut at the main loop (ID 36544 on AE / 35551 on SE) and the non-game frames right below it before modules are
  counted. Signatures and printed stacks are unchanged.
- **Log folder.** Logs go to the folder where `skse64.log` was written this session (the SE or GOG folder,
  whichever is newest). Some CommonLibSSE-NG builds return `My Games\Skyrim.INI\SKSE` from `log_directory()` on
  some installs. The capture's log backup reads from the same folder.
- The watchdog now checks every 50 ms (was 250 ms), so a hitch can be sampled while it is happening. It only reads
  atomics, on its own thread.
- The stall warning lines include the full context line (location, menus, actors, Papyrus load).
- CMakeLists: the unused `directxtk` package lookup is skipped when `THEWATCHER_LOCAL_DEPS` is set (off by default).

### Notes
- The main loop does not tick during loading screens ("0 frames during load" on every load on 1.6.1170).
- Building with Visual Studio 2026: the pinned vcpkg baseline pulls fmt 9.1, which no longer compiles because the
  STL removed `stdext::checked_array_iterator`. That's toolchain age, not a code problem. A newer vcpkg baseline or
  CommonLibSSE-NG build fixes it.
- Tested in game on 1.6.1170 (AE): the plugin loads, the frame hook installs, and the new outputs are written as
  described. SE 1.5.97 compiles but has not been tested in game.

## 1.0.4 and earlier
By the original author. See the version notes at the top of README.md.
