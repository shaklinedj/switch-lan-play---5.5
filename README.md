# switch-lan-play

[![Chat en Discord](https://img.shields.io/badge/chat-en%20discord-7289da.svg)](https://discord.gg/zEMCu5n)

Juega con tus amigos en modo multijugador local a traves de internet — directamente desde tu Nintendo Switch, sin PC.

---

## Tabla de contenidos

1. [Como funciona?](#como-funciona)
2. [Instalacion rapida](#instalacion-rapida)
3. [Componentes del proyecto](#componentes-del-proyecto)
4. [Servidor relay](#servidor-relay)
5. [Herramientas de desarrollo](#herramientas-de-desarrollo)
6. [Compilacion desde fuente](#compilacion-desde-fuente)
7. [Configuracion avanzada](#configuracion-avanzada)
8. [Protocolo](#protocolo)
9. [Solucion de problemas](#solucion-de-problemas)

---

## Como funciona?

El proyecto combina **dos sysmodules** que trabajan juntos en la Switch:

```
Switch (juego en modo "inalambrico local")
    |
    v
ldn_mitm (Title ID: 4200000000000010)
    |  Intercepta el servicio LDN de Nintendo y lo convierte
    |  en paquetes UDP/TCP LAN en el puerto 11452
    |
    v
sysmodule lan-play (Title ID: 42000000000000B1)
    |  LDN Bridge: captura trafico del puerto 11452,
    |  reescribe IPs (ALG) y lo envia al relay
    |
    v  UDP sobre WiFi
Servidor relay (puerto 11451)
    |
    v
Otros jugadores (Switch / PC / Emulador)
```

### Flujo de datos

1. **ldn_mitm** intercepta las llamadas LDN del juego y las convierte en UDP broadcast local (puerto 11452)
2. **sysmodule lan-play** captura esos paquetes en el puerto bridge (11453), reescribe las IPs WiFi reales por IPs virtuales `10.13.x.x`, y los envia al relay
3. Los paquetes del relay se inyectan de vuelta a ldn_mitm via `127.0.0.1:11452` (loopback, compatible con Horizon OS)
4. **TCP proxy** en el puerto 11453 maneja las conexiones Station->AccessPoint a traves del relay

### Que juegos son compatibles?

- **Juegos con "modo LAN" oficial** (ej: Mario Kart 8 DX con L+R+L-Stick): funcionan directamente
- **Juegos solo con "inalambrico local"** (ej: Super Smash Bros, Pokemon, Animal Crossing): funcionan gracias a ldn_mitm que convierte wireless-local -> LAN

Cada Switch obtiene automaticamente una **IP virtual unica** en el rango `10.13.0.0/16` calculada a partir del numero de serie del dispositivo.

---

## Instalacion rapida

> Requiere Nintendo Switch con **Atmosphere CFW >= 1.11.0** y conexion WiFi.

### Paso 1 — Descargar

Descarga `switch-lan-play-all-in-one-v1.14-rc1.zip` desde [Releases](https://github.com/shaklinedj/switch-lan-play---5.5/releases).

### Paso 2 — Copiar a la SD

Extrae el zip en la **raiz de tu tarjeta SD**. La estructura resultante:

```
sdmc:/
+-- atmosphere/
|   +-- contents/
|   |   +-- 4200000000000010/          <- ldn_mitm (intercepta LDN -> LAN)
|   |   |   +-- exefs.nsp
|   |   |   +-- flags/boot2.flag
|   |   |   +-- toolbox.json
|   |   +-- 42000000000000B1/          <- sysmodule lan-play (relay bridge)
|   |       +-- exefs.nsp
|   |       +-- flags/boot2.flag
|   |       +-- toolbox.json
|   +-- hosts/
|       +-- default.txt                <- DNS overrides (opcional)
+-- switch/
    +-- .overlays/
    |   +-- ldnmitm_config.ovl         <- overlay Tesla para ldn_mitm
    +-- lan-play/
    |   +-- lanplay-setup.nro           <- app configuradora
    |   +-- lanplay-debug.nro           <- debug homebrew
    |   +-- lanplay-sys-debug.nro       <- sysmodule debug
    +-- ldnmitm_config/
        +-- ldnmitm_config.nro          <- config de ldn_mitm
```

### Paso 3 — Configurar el servidor relay

1. Reinicia la Switch (ambos sysmodules arrancan automaticamente con `boot2.flag`)
2. Abre **Homebrew Menu** -> lanza **"LanPlay Setup"**
3. Pulsa **A**, escribe la direccion del relay (ej: `192.168.1.100:11451`) y pulsa **+**
4. Reinicia la Switch

### Paso 4 — Jugar

1. Abre cualquier juego compatible
2. Selecciona **"Juego inalambrico local"** (o modo LAN si el juego lo tiene)
3. Los jugadores conectados al mismo relay se veran automaticamente!

> **Nota:** Actualmente el sysmodule acepta IPs directas en la configuracion del relay.
> El soporte de hostnames (ej: `tekn0.net:11451`) esta pendiente de verificacion en Switch.

---

## Componentes del proyecto

| Directorio | Descripcion |
|------------|-------------|
| `sysmodule/` | Sysmodule principal (Title ID `42000000000000B1`). LDN bridge, relay client, IP virtual. |
| `ldn_mitm-1.25.1/` | Fork modificado de ldn_mitm v1.25.1. Intercepta LDN -> LAN. **Modificado**: TCP connect via relay proxy `127.0.0.1:11453` para IPs virtuales. |
| `hbapp/` | App homebrew "LanPlay Setup" para configurar el relay desde la Switch. |
| `server/` | Servidor relay UDP en Node.js/TypeScript. |
| `all_in_one/` | Paquete listo para SD con todos los binarios compilados. |
| `tools/` | Herramientas de desarrollo: `pc-peer.ts` (peer de prueba), `decode_scanresp.js` (decodificador de payloads LDN). |
| `src/` | Cliente PC original (captura paquetes con libpcap). |
| `lwip/` | Stack TCP/IP ligero (usado por el cliente PC). |

### Versiones actuales

| Componente | Version | Title ID |
|------------|---------|----------|
| Sysmodule lan-play | v1.14 | `42000000000000B1` |
| ldn_mitm (modificado) | v1.25.1 | `4200000000000010` |
| Atmosphere requerido | >= 1.11.0 | -- |

---

## Servidor relay

El servidor escucha en el puerto `11451/UDP` y reenvia paquetes entre todas las consolas conectadas.

### Docker (recomendado)

```sh
cd server
docker compose up -d
```

### Node.js directo

```sh
cd server
npm install
npm run build
npm start
```

Opciones:

| Parametro | Descripcion |
|-----------|-------------|
| `--port 11451` | Puerto UDP (por defecto `11451`) |
| `--simpleAuth usuario:clave` | Autenticacion basica |
| `--jsonAuth ./users.json` | Autenticacion por archivo JSON |

### Monitor de estado

```
GET http://TU_IP:11451/info
-> { "online": 5 }
```

### Puertos requeridos

| Puerto | Protocolo | Uso |
|--------|-----------|-----|
| 11451 | **UDP** | Relay de paquetes LAN (**obligatorio**) |
| 11451 | TCP | API de estado (opcional) |

### Hosting gratuito

Ver [server/README.md](server/README.md) para guias de despliegue en Oracle Cloud, fly.io, Railway, etc.

---

## Herramientas de desarrollo

### pc-peer.ts — Peer de prueba desde PC

Conecta tu PC al relay como un peer virtual para probar Scan/ScanResp sin necesidad de dos consolas.

```sh
cd server && npm install && cd ..
npx --prefix ./server ts-node --project ./server/tsconfig.json ./tools/pc-peer.ts <relay> <port> <virtualIP>
```

Comandos disponibles en la consola interactiva:
- `scan` — Envia un LDN Scan broadcast
- `autoscan [ms]` — Escaneo automatico cada N milisegundos
- `stopscan` — Detener autoscan
- `ping <ip>` — Ping a un peer virtual
- `stats` — Mostrar estadisticas
- `quit` — Salir

### decode_scanresp.js — Decodificador de payloads

```sh
node tools/decode_scanresp.js
```

Muestra un volcado hex+ASCII del payload ScanResp para analisis visual.

---

## Compilacion desde fuente

### Sysmodule (Switch)

Requiere **devkitPro** con soporte Switch:

```sh
dkp-pacman -S switch-dev switch-atmo-tools

cd sysmodule
make
# Resultado: atmosphere/ -> copiar a la raiz de la SD
```

### ldn_mitm (Switch)

```sh
cd ldn_mitm-1.25.1
git submodule update --init --recursive
make
# O con Docker:
docker-compose up --build
```

### App configuradora homebrew

```sh
cd hbapp
make
# Resultado: lanplay-setup.nro -> sdmc:/switch/lan-play/
```

### Cliente PC (alternativo)

```sh
mkdir build && cd build
cmake ..
make
```

Requiere `libpcap-dev` (Linux), `npcap` (Windows) o `libpcap` (macOS).

#### Windows (MSYS2/MinGW)

1. Instala **MSYS2** y abre `MSYS2 MinGW 64-bit`.
2. Instala toolchain y utilidades:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make git
```

3. Asegurate de que `C:\msys64\mingw64\bin` este en `PATH`.
4. Desde PowerShell en la raiz del repo:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_pc_windows_mingw.ps1 -BuildDir build -Config Release
```

Si el repo viene desde un ZIP sin `.git`, CMake descargara automaticamente `libuv` y `uvw` durante la configuracion.

---

## Configuracion avanzada

### Archivo de configuracion

Ubicacion: `sdmc:/config/lan-play/config.ini`

```ini
[server]
relay_addr = 192.168.1.100:11451

; Opcional: fijar IP virtual (por defecto se genera del numero de serie)
; ip = 10.13.5.10

; Opcional: autenticacion
; username = miusuario
; password = miclave
```

### IP virtual automatica

Cada Switch obtiene una IP determinista en `10.13.1.1-10.13.254.254` derivada de su numero de serie. No requiere DHCP ni configuracion manual.

### Overlay Tesla (ldn_mitm)

Si tienes Tesla Menu instalado, el overlay `ldnmitm_config.ovl` permite activar/desactivar ldn_mitm en tiempo real sin reiniciar.

---

## Protocolo

### Relay (puerto 11451)

```c
struct packet {
    uint8_t type;       // 0=KEEPALIVE, 1=IPV4, 2=PING, 3=IPV4_FRAG
    uint8_t payload[];
};
```

### LDN (puerto 11452)

```c
struct ldn_header {
    uint32_t magic;             // 0x11451400
    uint8_t  type;              // 0=Scan, 1=ScanResp, 2=Connect, 3=SyncNetwork
    uint8_t  compressed;
    uint16_t length;            // tamano del body
    uint16_t decompress_length;
    uint8_t  reserved[2];
};
// Seguido de NetworkInfo (para ScanResp/SyncNetwork) o vacio (para Scan)
```

---

## Caracteristicas tecnicas

- **LDN Bridge**: Modulo que captura trafico LDN de ldn_mitm y lo envia al relay con reescritura de IPs (ALG)
- **Inyeccion por loopback**: Usa `127.0.0.1:11452` en vez de broadcast, compatible con Horizon OS que no loopea broadcasts
- **TCP Proxy**: Puerto 11453 tunnela conexiones TCP Station->AP a traves del relay
- **Bypass DNS (inet_pton)**: Si usas una IP directa, se salta la resolucion DNS de Nintendo evitando el error "System Busy"
- **Thread Spoofing**: Clona dinamicamente permisos de hilo para evitar crashes por restricciones de CPU de Atmosphere
- **IP determinista**: Derivada del numero de serie de hardware, inmutable sin DHCP

---

## Solucion de problemas

| Sintoma | Que revisar |
|---------|-------------|
| Sysmodule no arranca | Verifica Atmosphere >= 1.11.0; comprueba que `boot2.flag` existe en ambos Title IDs |
| No ve salas de otros jugadores | Verifica que ambos jugadores usan el mismo relay; el juego debe estar en modo "inalambrico local" |
| "LanPlay Setup" no aparece en hbmenu | Verifica NRO en `sdmc:/switch/lan-play/lanplay-setup.nro` |
| Error de conexion al relay | Verifica que el puerto 11451/UDP esta abierto en el firewall del servidor |
| ldn_mitm no intercepta el juego | Verifica que `4200000000000010` tiene `boot2.flag`; reinicia la Switch |
| Latencia alta | Usa un relay geograficamente cercano a todos los jugadores |

---

## Licencia

[MIT](LICENSE.txt)

---

## Creditos

- [spacemeowx2/switch-lan-play](https://github.com/spacemeowx2/switch-lan-play) — Proyecto original
- [spacemeowx2/ldn_mitm](https://github.com/spacemeowx2/ldn_mitm) — ldn_mitm original
- [Atmosphere-NX](https://github.com/Atmosphere-NX/Atmosphere) — CFW para Nintendo Switch
