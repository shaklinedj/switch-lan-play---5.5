# Legacy Hardening Phase 6

This phase keeps compatibility with original LAN Play relays such as `tekn0.net`.
It does not add HELLO, rooms, custom TCP packet types, or server-side protocol changes.

## Goals

- Keep `lan-play` invisible until `ldn_mitm` reports real LDN activity.
- Close runtime sockets before sleep/wake transitions using PSC.
- Avoid touching PM services too early during boot.
- Improve LDN discovery stability with optional lobby cache.
- Add PC-side test tooling for Scan/ScanResp without requiring two Switches.

## Added hardening

### PSC sleep/wake monitor

The sysmodule initializes `pscm` at boot and starts a small monitoring thread after boot settles.
When Horizon reports `ReadySleep`, the monitor performs a fast runtime teardown:

- `lp->running = false`
- `ldn_bridge_close(lp)`
- `lan_client_close(lp)`
- `tap_close(lp)`

It does not wait for worker threads inside the PSC callback path. Normal cleanup joins the threads afterwards.

### Deferred PM initialization

`pm:shell` and `pm:info` are no longer initialized in `__appInit()`.
The foreground detector initializes after a boot delay from `main()`, using a short-lived SM session.

This avoids competing with early Horizon boot services.

### LDN IPC gate remains the primary trigger

LAN runtime starts only when both are true:

1. A probable game/application exists.
2. `ldn_mitm` reports LDN activity through `lanp:gt`.

The heavy network runtime still uses only the legacy relay protocol:

- KEEPALIVE
- IPV4
- IPV4_FRAG
- AUTH_ME
- INFO

### Optional lobby cache

Recent `ScanResp` packets are cached for up to 30 seconds and re-injected every 3 seconds while the bridge is active.
This helps games whose scan windows miss a remote response due to relay jitter.

Disable it by creating:

```text
sdmc:/config/lan-play/disable_lobby_cache
```

### Adaptive polling

The LDN UDP bridge polls aggressively while traffic is active and relaxes while idle:

- active: about 10 ms
- idle: 200–500 ms

This reduces CPU usage without delaying active scans too much.

## Test checklist

1. Boot to HOME and verify LAN runtime is not active.
2. Enter Internet Settings and verify no crash.
3. Open a game but do not enter local wireless; LAN runtime should stay idle.
4. Enter local wireless mode; `ldn_mitm` should notify `lanp:gt` and LAN runtime should start.
5. Leave local wireless; after timeout, runtime should stop.
6. Put console to sleep while local wireless is active; PSC should close sockets quickly.
7. Wake console and re-enter local wireless; runtime should start again.
8. Use `tools/pc-peer.ts` from a PC to send `autoscan` and inspect ScanResp.
