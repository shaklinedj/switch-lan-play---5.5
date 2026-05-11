# Runtime Foreground Gate

## Purpose

LAN Play must not open network sockets immediately at boot. The sysmodule now
waits until Process Manager reports a probable user Application/game process
before starting the heavy LAN runtime.

This is the Phase 2 gate. It prevents LAN Play from being active in:

- boot with no game
- HOME Menu with no game launched
- Internet Settings with no application process
- applets/system titles

## How it works

`foreground_detector.cpp` initializes `pm:shell` and `pm:info` from `__appInit()`.
The main supervisor loop calls:

```cpp
wait_for_foreground_game();
```

before `run_service()` starts the runtime.

The detector uses:

- `pmshellGetApplicationProcessIdForShell()`
- `pminfoGetProgramId()`

Then it blocks known system/applet title-id ranges.

## Runtime stop behavior

Once the LAN runtime is active, the service loop keeps polling foreground state.
If the application process disappears, the runtime breaks out of the loop and
executes cleanup:

1. `lp->running = false`
2. close LDN, relay, and TAP sockets
3. wait/close all threads
4. free runtime context

## Emergency fallback

Create this file on SD to temporarily disable the foreground gate:

```text
sdmc:/config/lan-play/disable_foreground_gate
```

When present, the sysmodule uses the older always-on startup behavior.

## Known limitation

This gate detects an Application process. It does not yet prove that the game is
visually focused when HOME is open with a game suspended in the background.
For stricter behavior, the next phase should combine this with LDN/local-wireless
activity detection or a focus source similar to SaltyNX-style game focus state.
