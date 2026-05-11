# Runtime Lazy Network Gate

## Purpose

This phase makes the sysmodule less visible while idle.

Before this change, `__appInit()` initialized NIFM/BSD/socket services during boot. That meant the module could hold network-related sessions even when no game was running.

After this change:

- boot initializes FS, set:sys and the foreground detector only
- NIFM/BSD/socket are initialized inside `run_service()`
- `run_service()` is only entered after the foreground gate sees a probable game/application
- cleanup releases sockets, BSD and NIFM before returning to the waiting loop

## Runtime model

```text
Boot
  -> minimal sysmodule
  -> wait for foreground game
  -> runtime_network_init()
  -> LAN runtime starts
  -> game disappears/reload
  -> close sockets
  -> wait/close threads
  -> runtime_network_exit()
  -> wait for game again
```

## Emergency fallback

Create this file to bypass the foreground gate and use legacy always-on startup:

```text
sdmc:/config/lan-play/disable_foreground_gate
```

## Remaining limitation

The foreground detector uses PM application process presence. This is safer than always-on boot startup, but it is not a perfect visual focus detector. The next stricter layer should use LDN/LAN activity or a reliable applet focus signal before enabling heavy relay/bridge threads.
