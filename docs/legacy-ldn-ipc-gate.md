# Legacy LDN IPC Gate

This phase keeps switch-lan-play compatible with original relays such as `tekn0.net`.

It does **not** add new relay protocol types, rooms, HELLO packets, or server-side changes.

## Goal

The LAN runtime should stay asleep unless a foreground game actually starts using LDN/local wireless.

## Final gate rule

LAN runtime starts only when all of these are true:

1. A valid foreground game/application exists.
2. `ldn_mitm` intercepted real LDN activity.
3. The LDN activity is fresh.
4. Network/config are available.

## IPC service

`lan-play` registers a lightweight local IPC service:

```text
lanp:gt
```

This service is not network I/O. It only receives small state notifications from `ldn_mitm`.

## Events sent by ldn_mitm

Runtime activation events:

- `Scan()` -> `LANP_GATE_EVENT_SCAN`
- `CreateNetwork()` -> `LANP_GATE_EVENT_HOST`
- `Connect()` -> `LANP_GATE_EVENT_CONNECT`
- private equivalents also notify with zero intent when no details are available

Runtime idle events:

- `Disconnect()`
- `DestroyNetwork()`
- `CloseAccessPoint()`
- `CloseStation()`
- `Finalize()`

## Timeout

The runtime is considered active for 30 seconds after the last activation event.
If no fresh LDN event arrives, `lan-play` shuts the heavy runtime down.

## Why IPC instead of SD or UDP?

- No SD write/read loop.
- No file races.
- No loopback UDP socket.
- `lan-play` can remain network-dormant.
- The only always-on component is a tiny IPC service.

## Emergency fallback

Create this file to disable only the LDN activity gate:

```text
sdmc:/config/lan-play/disable_ldn_gate
```

Create this file to disable the foreground gate too:

```text
sdmc:/config/lan-play/disable_foreground_gate
```

## Expected behavior

| Console state | LAN runtime |
|---|---|
| Home Menu | OFF |
| Internet Settings | OFF |
| Game open but not in local wireless | OFF |
| Game enters LDN scan/host/connect | ON |
| Game exits LDN or no LDN activity for 30s | OFF |

## Compatibility

Compatible with original LAN Play relays because all game traffic still uses the classic protocol:

- `KEEPALIVE`
- `IPV4`
- `IPV4_FRAG`
- `AUTH_ME`
- `INFO`

No server update is required.
