# Runtime Lifecycle Stabilization Plan

## Main problem

The original sysmodule stayed active permanently after boot. This could keep network services, sockets and threads alive while Horizon was changing WiFi state or opening Internet Settings.

## Stabilization completed in these phases

### Phase 1 — Relay/thread safety

- keepalive no longer closes/recreates `relay_fd` from its own thread
- relay send/close paths are guarded with the existing mutex
- `keepalive_thread` is now tracked in `struct lan_play`
- server simpleAuth password logging is redacted

### Phase 2 — Foreground application gate

- added `foreground_detector.cpp/.h`
- uses PM services to detect a probable user application/game
- waits for a game before entering LAN runtime
- stops runtime when the application disappears
- adds emergency fallback file:

```text
sdmc:/config/lan-play/disable_foreground_gate
```

### Phase 3 — Lazy network runtime

- boot no longer initializes NIFM/BSD/socket
- NIFM/BSD/socket are initialized only inside runtime startup
- network services are released during cleanup
- waiting loops stop if the game disappears while waiting for WiFi/config/DNS
- log writes use a mutex and throttle `fsdevCommitDevice()` calls

## Desired lifecycle

| State | LAN Runtime | NIFM/BSD/socket |
|---|---|---|
| Boot | OFF | OFF |
| Home Menu / no game | OFF | OFF |
| Game detected | STARTING | ON |
| LAN runtime active | ON | ON |
| Game closed | STOPPING -> OFF | OFF |
| Internet Settings without game | OFF | OFF |

## Remaining work before calling this production-ready

- compile with devkitPro/libnx
- test on real Switch with Atmosphere
- verify PM service availability on target firmware
- add LDN/LAN activity gate for stricter foreground behavior
- add server room/session isolation for robust multijugador
- redesign TCP tunneling with explicit connection IDs

## Validation tests

- boot with no game and confirm no LAN runtime starts
- enter Internet Settings with no game 20 times
- open game and confirm runtime starts
- close game and confirm network services are released
- sleep/resume during runtime
- change WiFi during runtime
- relay offline during runtime
- open/close game 30+ times without reboot


## Phase 4 Legacy Relay Compatibility

- Keep original relay protocol compatibility.
- Do not require server rooms, HELLO, or TCP custom packet types.
- Add LDN scan filtering by carrying ScanFilter in Scan packets between updated clients.
- Preserve compatibility with old clients and public relays such as tekn0.net.
- Keep LDN NodeCountMax at 8; do not change the ABI-sized NetworkInfo layout to force 12.

## Phase 5 Legacy: LDN IPC Gate

This phase changes the runtime activation condition from:

```text
game detected -> start LAN runtime
```

to:

```text
game detected + ldn_mitm reports LDN activity -> start LAN runtime
```

The gate uses a local IPC service named `lanp:gt`; it does not use SD files or UDP loopback.

## Phase 6 — Legacy hardening

- PSC monitor added for fast sleep teardown.
- PM foreground detector is initialized after boot delay, not from early app init.
- LDN bridge uses adaptive polling.
- LDN bridge can cache/reinject recent ScanResp packets for discovery stability.
- `tools/pc-peer.ts` is included as the preferred relay/ScanResp test tool.

This phase remains compatible with original LAN Play relays such as `tekn0.net`.
