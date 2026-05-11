# switch-lan-play — Sysmodule (Title ID: 42000000000000B1)

Sysmodule que corre en segundo plano en Nintendo Switch y conecta los juegos LAN al servidor relay a traves de internet.

---

## Arquitectura

```
Juego (modo inalambrico local)
    |
    v
ldn_mitm (4200000000000010) -- intercepta LDN -> LAN UDP
    |
    v  Puerto 11452 (UDP broadcast)
+------------------------------------------+
| sysmodule lan-play (42000000000000B1)    |
|                                          |
|  LDN Bridge (ldn_bridge.cpp)             |
|    - Captura UDP en puerto 11453         |
|    - Reescribe IPs: WiFi <-> 10.13.x.x  |
|    - Inyecta relay -> 127.0.0.1:11452    |
|                                          |
|  TCP Proxy (puerto 11453)                |
|    - Tunnela conexiones Station->AP      |
|                                          |
|  Relay Client (lan_client_nx.cpp)        |
|    - Conexion UDP al relay (puerto 11451)|
|    - Keepalive cada 10s                  |
|                                          |
|  Tap Interface (tap_iface.cpp)           |
|    - Raw socket para captura de paquetes |
|    - Inyeccion UDP hacia juegos locales  |
+------------------------------------------+
    |
    v  UDP sobre WiFi
Servidor relay (puerto 11451)
```

## Puertos internos

| Puerto | Protocolo | Funcion |
|--------|-----------|---------|
| 11451 | UDP | Conexion al relay server |
| 11452 | UDP | Puerto nativo de ldn_mitm (LDN game port) |
| 11453 | UDP/TCP | Bridge: captura de ldn_mitm + proxy TCP |

## Archivos fuente

| Archivo | Descripcion |
|---------|-------------|
| `source/main.cpp` | Entry point, inicializacion de servicios Horizon OS |
| `source/ldn_bridge.cpp` | LDN Bridge: captura, inyeccion, ALG de IPs |
| `source/ldn_bridge.h` | Definiciones del bridge (puertos, header LDN, offsets) |
| `source/tap_iface.cpp` | Interfaz de red virtual: raw socket + inyeccion UDP |
| `source/lan_client_nx.cpp` | Cliente relay: conexion UDP, keepalive, envio/recepcion |
| `source/nx_common.h` | Funciones comunes: logging, IP from serial, config |

## Compilacion

```sh
# Requiere devkitPro con switch-dev
cd sysmodule
make

# Resultado: atmosphere/contents/42000000000000B1/
# Copiar a la raiz de la SD
```

## Configuracion

El sysmodule lee `sdmc:/config/lan-play/config.ini`:

```ini
[server]
relay_addr = 192.168.1.100:11451
; ip = 10.13.5.10  (opcional, por defecto se genera del serial)
```

## Caracteristicas tecnicas

- **LDN Bridge con ALG**: Reescribe ipv4Address en NetworkInfo.nodes[] al vuelo (WiFi real <-> IP virtual 10.13.x.x)
- **Inyeccion por loopback**: `127.0.0.1:11452` en vez de broadcast — Horizon OS no loopea broadcasts UDP
- **TCP Proxy**: Acepta conexiones de ldn_mitm en :11453 y las tunnela al relay hacia el host remoto
- **Bypass DNS**: Detecta IPs numericas y salta sfdnsres para evitar "System Busy (EAI_AGAIN)"
- **Thread Spoofing**: Clona permisos de hilo dinamicamente para evitar crash 0xe201
- **IP determinista**: Calculada del numero de serie, sin DHCP

## Title ID

`42000000000000B1` — Rango personalizado para sysmodules de networking.
Configurable en `Makefile` y `config/main.json`.
