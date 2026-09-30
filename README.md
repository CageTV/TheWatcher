# The Watcher

SKSE plugin for Skyrim AE (1.6.1170) that watches for the game freezing, especially on loading screens,
captures the frozen thread's call stack when it happens, and in aggressive mode logs performance stats every interval.

Version 1.1.2: stack history sampled DURING hitches, stalls and slow loading gaps; recovered captures labeled and kept
light; stacks walked from a copy after the thread is resumed; location / actor / Papyrus / VRAM context; events log,
live status file and machine-readable summaries. See CHANGELOG.md.
Version 1.0.4: dumps written by a separate helper (TheWatcherDump.exe, ship it next to the DLL), report saved before slower steps, a guard that logs if a capture itself gets stuck, SAME/CHANGED/INCOMPLETE sampling labels, sampling limited to the main thread unless bAllThreadStacks=1, log backup covers the whole game session, safer default thresholds (normal mode is the default).
Version 1.0.3 added Skyrim SE 1.5.97 support (SE Address Library file, SE hook addresses, runtime line in the log). SE is untested so far.
Version 1.0.2 added Address Library IDs on SkyrimSE.exe frames and FROZEN/MOVING thread sampling in each capture.

## Build
Same as your EscapeRestore template: open `C:\dllmaker\thewatcher` in VS2022, pick the Release preset, build.

## Install (MO2)
```
SKSE\Plugins\TheWatcher.dll
SKSE\Plugins\TheWatcher.pdb
SKSE\Plugins\TheWatcher.ini
```

## Output
`Documents\My Games\Skyrim Special Edition\SKSE\`
- `TheWatcher.log`: settings, one line per loading screen, warnings, captures
- `TheWatcher\stats_<date_time>.csv`: stats, one file per session (aggressive mode or `bStatsLog=1`)
- `TheWatcher\stall_<date_time>_<n>\`: `stacks.txt`, `summary.json` (1.1), `SkyrimSE.dmp`, `logs\` (every SKSE log from that session)
- `TheWatcher\slow_<date_time>.jsonl` (1.1): one JSON line per hitch / slow loading gap, with the sampled stack history
- `TheWatcher\events_<date_time>.log` (1.1): loads, menus, cell changes, saves, fast travel, equips, hitches, captures
- `TheWatcher\status.json` (1.1): current state, rewritten every second

## Modes (TheWatcher.ini)
- `iMode=0` off
- `iMode=1` normal: detect stalls, capture evidence, one log line per loading screen
- `iMode=2` aggressive: tighter thresholds, all-thread stacks, stats row every `iStatsIntervalSeconds`.
  Keys in `[Aggressive]` replace the normal values.

## Known limits
- Default thresholds are starting points; tune them from the per-load lines in `TheWatcher.log`.
- Confirmed on 1.6.1170: the main loop does NOT tick during loading screens ("0 frames during load" on every load).
  Load-progress events do fire during normal door / save loads.
- 1.1: a thread is suspended only for GetThreadContext + one copy of its stack; the unwind runs on the copy after
  the thread is resumed, so the watcher can no longer deadlock on a lock the suspended thread holds. The minidump
  (MiniDumpWriteDump) is still written last because it suspends every thread itself.
