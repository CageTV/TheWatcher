# The Watcher

SKSE plugin for Skyrim AE (1.6.1170) that watches for the game freezing, especially on loading screens,
captures the frozen thread's call stack when it happens, and in aggressive mode logs performance stats every interval.

Version 1.0.4: dumps written by a separate helper (TheWatcherDump.exe, ship it next to the DLL), report saved before slower steps, a guard that logs if a capture itself gets stuck, SAME/CHANGED/INCOMPLETE sampling labels, sampling limited to the main thread unless bAllThreadStacks=1, log backup covers the whole game session, safer default thresholds (normal mode is the default).
Version 1.0.3 added Skyrim SE 1.5.97 support (SE Address Library file, SE hook addresses, runtime line in the log). SE is untested so far.
Version 1.0.2 added Address Library IDs on SkyrimSE.exe frames and FROZEN/MOVING thread sampling in each capture.
The new code has not been compiled yet; expect possible small compile fixes.

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
- `TheWatcher\stall_<date_time>_<n>\`: `stacks.txt`, `SkyrimSE.dmp`, `logs\` (every SKSE log from that session)

## Modes (TheWatcher.ini)
- `iMode=0` off
- `iMode=1` normal: detect stalls, capture evidence, one log line per loading screen
- `iMode=2` aggressive: tighter thresholds, all-thread stacks, stats row every `iStatsIntervalSeconds`.
  Keys in `[Aggressive]` replace the normal values.

## Known limits
- Default thresholds are starting points; tune them from the per-load lines in `TheWatcher.log`.
- Not yet confirmed that load-progress events fire during every kind of loading screen, or that the main loop
  ticks during loads. Each "Load #N finished" line reports both counts.
- The stack walk runs while the main thread is suspended. If that thread is frozen inside Windows' module-table lock,
  the watcher could block too (the game is already hung then). The minidump is written last for the same reason.
