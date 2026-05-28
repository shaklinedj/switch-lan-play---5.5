# 🎮 Guía completa — Switch LAN Play

## Tabla de contenidos
1. [Cómo funciona todo junto](#cómo-funciona)
2. [Levantar el servidor con Docker](#servidor-docker)
3. [Exponer tu IP pública](#exponer-ip-pública)
4. [Instalar en la Switch](#instalar-en-la-switch)
5. [Configurar la Switch con LanPlay Setup](#configurar-la-switch)
6. [Probar la conexión](#probar-la-conexión)
7. [Solución de problemas](#solución-de-problemas)

---

## Cómo funciona

```
Switch A  ──────┐
                ▼
            [Servidor Relay]  ◄──── tu PC con Docker
                ▲                   (IP pública: tuip:11451)
Switch B  ──────┘
```

- Cada Switch se conecta al servidor relay por **UDP en el puerto 11451**.
- El servidor reenvía los paquetes de juego entre todas las consolas conectadas.
- El juego *cree* que todos están en la misma red local — sin configuración de red manual.

---

## Servidor Docker

### Paso 1 — Verificar Docker

Abre PowerShell y comprueba que Docker esté corriendo:

```powershell
docker --version
docker ps
```

Si ves la versión y la lista (vacía está bien), Docker funciona.

### Paso 2 — Levantar el servidor

```powershell
docker run -d `
  --name lanplay-server `
  --restart unless-stopped `
  -p 11451:11451/udp `
  -p 11451:11451/tcp `
  shaklinedj/switch-lan-play
```

> [!TIP]
> El flag `--restart unless-stopped` hace que el servidor arranque automáticamente si reinicias el PC.

### Paso 3 — Verificar que el servidor está corriendo

```powershell
docker ps
```

Deberías ver `lanplay-server` con estado `Up`.

Para ver los logs en tiempo real:

```powershell
docker logs -f lanplay-server
```

### Paso 4 — Verificar el endpoint HTTP de estado

Abre tu navegador en:

```
http://localhost:11451/info
```

Deberías ver respuesta como:
```json
{ "online": 0 }
```

Cuando se conecten Switches, el número subirá.

### Comandos útiles de Docker

```powershell
# Detener el servidor
docker stop lanplay-server

# Reiniciar el servidor
docker restart lanplay-server

# Ver logs de las últimas 50 líneas
docker logs --tail 50 lanplay-server

# Eliminar el contenedor (para reinstalar)
docker rm -f lanplay-server
```

---

## Exponer tu IP pública

Para que las Switches de otras personas se conecten desde internet, necesitás **abrir el puerto 11451** en tu router.

### Paso 1 — Conocer tu IP local (PC)

```powershell
ipconfig
```

Buscá la línea `Dirección IPv4` bajo tu adaptador WiFi o Ethernet. Ejemplo: `192.168.1.100`

> [!IMPORTANT]
> Esta IP local debe ser **fija (estática)** para que el port forwarding funcione siempre. Configuralo en tu router o en la configuración de red de Windows.

### Paso 2 — Fijar IP estática en Windows (opcional pero recomendado)

1. `Win + R` → `ncpa.cpl`
2. Click derecho en tu adaptador → **Propiedades**
3. **Protocolo de Internet versión 4 (TCP/IPv4)** → Propiedades
4. Marcá **Usar la siguiente dirección IP**:
   - Dirección IP: `192.168.1.100` (la que tenés ahora)
   - Máscara: `255.255.255.0`
   - Puerta de enlace: `192.168.1.1` (la IP de tu router)
   - DNS preferido: `8.8.8.8`

### Paso 3 — Abrir el puerto en el router (Port Forwarding)

1. Abrí el navegador y entrá a tu router:
   - Probá `192.168.1.1` o `192.168.0.1`
   - Usuario/contraseña: generalmente `admin` / `admin` o lo que figura en la etiqueta del router

2. Buscá la sección **"Port Forwarding"**, **"Virtual Server"**, o **"NAT"**

3. Creá una nueva regla:

   | Campo | Valor |
   |-------|-------|
   | Nombre | `LanPlay` |
   | Protocolo | `UDP` |
   | Puerto externo | `11451` |
   | Puerto interno | `11451` |
   | IP interna | `192.168.1.100` (tu PC) |

4. Guardá y reiniciá el router si se pide.

> [!NOTE]
> También podés agregar una segunda regla para **TCP** con el mismo puerto (opcional, solo para el endpoint `/info`).

### Paso 4 — Conocer tu IP pública

```powershell
Invoke-WebRequest -Uri "https://api.ipify.org" -UseBasicParsing | Select-Object -ExpandProperty Content
```

O simplemente buscá **"cual es mi ip"** en Google. El resultado es tu IP pública.

Ejemplo: `201.x.x.x`

> [!WARNING]
> Las IPs públicas en conexiones domésticas suelen ser **dinámicas** — cambian cada varios días o al reiniciar el router. Si querés una IP estable:
> - Usá un servicio de **DNS dinámico** (DynDNS, No-IP, Duck DNS — todos tienen plan gratuito)
> - O contratá IP fija con tu proveedor

### Paso 5 — Verificar desde internet

Con el servidor corriendo, verificá que el puerto esté abierto desde fuera:

```
https://portchecker.co
```

Ingresá tu IP pública y el puerto `11451`. Si aparece como **abierto**, todo está bien.

También podés probar el endpoint de estado desde fuera:

```
http://TU_IP_PUBLICA:11451/info
```

---

## Instalar en la Switch

### Lo que necesitás

- Nintendo Switch con **Atmosphere CFW**
- Tarjeta SD con al menos 10 MB libres
- Conexión WiFi en la Switch

### Archivos compilados (ya los tenés listos)

| Archivo | Ubicación en tu PC |
|---------|-------------------|
| Sysmodule (Atmosphere) | `sysmodule\atmosphere\` |
| App configuradora | `hbapp\lanplay-setup.nro` |

### Estructura en la SD

Copiá los archivos de esta manera:

```
sdmc:/
├── atmosphere/
│   └── contents/
│       └── 42000000000000B1/
│           ├── exefs/
│           │   ├── main          ← sysmodule NSO (ya compilado)
│           │   └── main.npdm     ← permisos (ya compilado)
│           └── flags/
│               └── boot2.flag    ← arranque automático (ya compilado)
└── switch/
    └── lanplay-setup/
        └── lanplay-setup.nro     ← app configuradora (hbapp\lanplay-setup.nro)
```

> [!TIP]
> La carpeta `atmosphere/` del sysmodule ya está generada completa en `sysmodule\atmosphere\`. Solo copiala a la raíz de la SD. Después creá la carpeta `/switch/lanplay-setup/` y copiá el NRO.

### Procedimiento

1. Apagá la Switch
2. Sacá la tarjeta SD
3. Insertala en tu PC
4. Copiá la carpeta `atmosphere/` a la raíz de la SD
5. Copiá `lanplay-setup.nro` a `/switch/lanplay-setup/`
6. Expulsá la SD con seguridad
7. Insertá la SD en la Switch y encendela con **Atmosphere** (hold VOL+ al encender, o como tengas configurado tu CFW)

---

## Configurar la Switch

### Primera vez

Al arrancar, el sysmodule detecta que no hay configuración y muestra un aviso en pantalla.

1. Abrí el **Homebrew Menu** (usualmente: en la pantalla principal, mantené R y lanzá cualquier juego)
2. Buscá y lanzá **"LanPlay Setup"**
3. Presioná **A**
4. Se abre el teclado en pantalla — escribí la dirección de tu servidor:
   ```
   TU_IP_PUBLICA:11451
   ```
   Ejemplo: `201.55.123.45:11451`
5. Presioná **+** para confirmar
6. La app guarda la config y te pide reiniciar

### Reiniciar y verificar

1. Mantené **POWER** → **Opciones de energía** → **Reiniciar**
2. La consola arrancará con el sysmodule activo automáticamente

### Verificar que el sysmodule está corriendo

En los logs del servidor Docker, cuando la Switch se conecte deberías ver:

```
docker logs -f lanplay-server
```

Y en el endpoint de estado:
```
http://TU_IP_PUBLICA:11451/info
→ { "online": 1 }
```

---

## Probar la conexión

### Prueba local (solo vos)

1. Levantá el servidor Docker
2. Configurá la Switch con `localhost` o `192.168.1.100:11451`  
   *(tu IP local, no la pública)*
3. Verificá que aparezca `"online": 1` en `http://localhost:11451/info`

### Prueba con otro jugador

1. El servidor debe estar corriendo con el puerto abierto
2. Ambas Switches configuradas con `TU_IP_PUBLICA:11451`
3. En los juegos que soporten modo LAN:
   - **Mario Kart 8 Deluxe**: Menu Principal → Multijugador → LAN Play
   - **Splatoon 3**: LAN Test
   - **Super Smash Bros. Ultimate**: Versus → Red local
4. El endpoint `/info` debe mostrar `"online": 2`

### Checklist de conexión

```
☐ Docker está corriendo  (docker ps)
☐ Puerto 11451 UDP abierto en el router
☐ Switch con Atmosphere CFW
☐ Sysmodule copiado a la SD
☐ relay_addr configurado en la Switch
☐ Switch conectada al WiFi
☐ http://IP:11451/info responde
```

---

## Solución de problemas

| Síntoma | Causa probable | Solución |
|---------|---------------|----------|
| `docker ps` no muestra el contenedor | El contenedor no arrancó | `docker logs lanplay-server` para ver el error |
| `/info` no responde desde fuera | Puerto no abierto | Verificar port forwarding en el router |
| Switch muestra "sin servidor configurado" | Config no guardada | Abrir LanPlay Setup y configurar relay_addr |
| Switch conecta pero `online` no sube | Firewall de Windows | Ver sección siguiente |
| Latencia alta | Servidor lejos físicamente | Usar servidor más cercano geográficamente |
| IP pública cambia | IP dinámica | Usar servicio de DNS dinámico (No-IP, Duck DNS) |

### Firewall de Windows

Si el servidor corre pero las Switches no se conectan desde fuera, puede ser el Firewall de Windows:

```powershell
# Abrir puerto 11451 UDP en el Firewall de Windows
New-NetFirewallRule -DisplayName "LanPlay UDP 11451" `
  -Direction Inbound `
  -Protocol UDP `
  -LocalPort 11451 `
  -Action Allow

# Para TCP también (endpoint /info)
New-NetFirewallRule -DisplayName "LanPlay TCP 11451" `
  -Direction Inbound `
  -Protocol TCP `
  -LocalPort 11451 `
  -Action Allow
```

> Ejecutar PowerShell **como Administrador**.

### DNS dinámico con Duck DNS (gratuito)

Si tu IP pública cambia frecuentemente:

1. Registrate en [duckdns.org](https://www.duckdns.org) con tu cuenta de Google/GitHub
2. Creá un dominio: e.g. `milanplay.duckdns.org`
3. Instalá el cliente de Duck DNS en tu PC (actualiza la IP automáticamente)
4. Configurá las Switches con: `milanplay.duckdns.org:11451`

---

> [!NOTE]
> El servidor relay solo reenvía paquetes UDP — **no almacena** datos de juego. El servidor puede manejar simultáneamente múltiples "salas" ya que rutea por IP virtual.
