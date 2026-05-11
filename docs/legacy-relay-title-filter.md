# Legacy Relay Title/Intent Filter

This phase keeps compatibility with original switch-lan-play relays such as tekn0.net.

No server protocol changes are required. The relay still sees only the classic packet types.

## What changed

`ldn_mitm` now includes the game-provided `ScanFilter` inside outbound `Scan` packets. Updated hosts inspect that filter before answering with `ScanResp`.

This reduces cross-game noise on public relays because a host only responds when its `localCommunicationId`, `sceneId`, `networkType`, SSID, or session fields match the scan request flags.

## Backward compatibility

Older clients send empty `Scan` packets. Updated hosts still answer those requests to preserve compatibility.

Older hosts ignore the scan payload and continue answering as before. Updated scanning clients still apply the same local filter before returning results to the game.

## Player limits

For LDN/local wireless, `ldn_mitm` keeps Nintendo's 8-node NetworkInfo shape. Do not change `NodeCountMax` to 12 for LDN; that changes the ABI-sized structure and can break games.

Mario Kart 8 Deluxe supports up to 12 players in official LAN Play, but local wireless/LDN is up to 8 players.
