# AI Rules & Project Skills (Caveman Standard v2.0)

> Full project skills → `.github/copilot-instructions.md`

## Communication: Modo Cavernícola (10 Rules)
1. No filler. No greetings. No politeness.
2. Execute first, talk second.
3. Use fragments. Drop articles/pronouns.
4. No meta-commentary.
5. No preamble. No postamble.
6. No tool announcements.
7. Explain only when needed.
8. Code speaks.
9. Error = Fix. No apologies.
10. Be direct.

## Technical Quick Ref
- **Title ID**: `4200000000000011` (sysmodule), `4200000000000010` (ldn_mitm)
- **BPF raw-socket capture** → UDP relay tunnel (SLP protocol)
- **NPDM**: Use stable from `final_v1.13/`, NOT npdmtool output
- **Packaging**: `exefs.nsp` (build_pfs0) REQUIRED for Atmosphere boot2
- **Init order**: smInit → fsInit → nifmInit → bsdInit → socketInit → smExit
- **FS Sync**: `fsdevCommitDevice("sdmc")` mandatory after SD writes
- **Memory**: Heap 2MB. BPF buffers 4KB aligned
- **DNS**: Retry loop (10x), never fail-and-die
- **WiFi**: Poll `nifmGetCurrentIpAddress()` before network ops
- **Threads**: priority 31, stack 32KB, 3 threads (tap/relay/keepalive)
- **Config**: `sdmc:/config/lan-play/config.ini`
- **Log**: `sdmc:/lan-play.log` (100KB rotate)
- **IPC**: `sdmc:/tmp/lanplay.status` + `sdmc:/tmp/lanplay.reload`

## Atmosphere Compatibility Rules (CRITICAL)

### FW 22+ / Atmosphere 1.11+ breaking changes
- **TLS ABI roto en FW 21.0.0**: todo homebrew/sysmodule compilado con libnx < 4.10.0 crashea (corrupción de memoria). Recompilar con libnx ≥ 4.10.0.
- **Memoria reducida**: FW 20.0.0+ tiene ~10MB menos para sysmodules personalizados. Correr lan-play + ldn_mitm juntos puede ser justo.
- **ldn_mitm mínimo requerido**: v1.25.0+ para soporte FW 22+.

### Sysmodule no inicia: checklist obligatorio
1. `boot2.flag` presente en `atmosphere/contents/<TID>/flags/boot2.flag` → **SIN ESTO NO ARRANCA**
2. `toolbox.json` presente en `atmosphere/contents/<TID>/toolbox.json` (**AMBOS** sysmodules lo requieren — sin él no aparece en el overlay y puede no arrancar)
3. `exefs.nsp` generado con `build_pfs0 exefs/ exefs.nsp` (el .nso solo no es suficiente)
4. Binarios recompilados con libnx actual (hacer `make clean && make`, no confiar en caché)

### exefs.nsp packaging manual
```bash
# Si make no regenera el .nsp automáticamente:
cp lan_play_sysmodule.nso atmosphere/contents/<TID>/exefs/main
/opt/devkitpro/tools/bin/build_pfs0 atmosphere/contents/<TID>/exefs exefs.nsp
```

### Build commands
```bash
# sysmodule lan-play (forzar rebuild):
C:\devkitPro\msys2\usr\bin\bash.exe -lc "cd '/c/.../sysmodule' && export DEVKITPRO=/opt/devkitpro && make clean && make 2>&1"

# ldn_mitm (solo sysmodule + config, overlay falla por tesla.hpp faltante):
C:\devkitPro\msys2\usr\bin\bash.exe -lc "cd '/c/.../ldn_mitm-1.25.1' && export DEVKITPRO=/opt/devkitpro && make 2>&1"
# El overlay (.ovl) usar el del release oficial descargado (tesla.hpp = submodule no inicializado)
```

### Deploy script
- Script: `tools/deploy_allinone.ps1`
- Copia 9 archivos: exefs.nsp x2, boot2.flag x2, toolbox.json, ldnmitm_config.nro, ldnmitm_config.ovl, lanplay NROs x3
- `.ovl` viene de `C:\Users\Dell\Desktop\ldn_mitm_v1.25.1_release\` (release oficial)

### Puertos personalizados (nuestro cambio en ldn_mitm)
- `lan_discovery.hpp:119` → `DefaultPort = 11452`
- `lan_discovery.cpp:685` → conecta a `127.0.0.1:11453`
- `lan_protocol.cpp:208` → bridge UDP a puerto `11453`
