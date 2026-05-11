# ☁️ Servidor Switch LAN Play en la Nube — Gratis para siempre

## Comparativa rápida de opciones gratuitas

| Plataforma | ¿Gratis para siempre? | RAM | CPU | Notas |
|---|---|---|---|---|
| **Oracle Cloud** ✅ | **Sí** | 1 GB (AMD) o hasta 24 GB (ARM) | 1/8 OCPU | **Mejor opción** |
| **Google Cloud** ✅ | **Sí** (e2-micro) | 1 GB | 0.25 vCPU | Solo en regiones US |
| **AWS** ⚠️ | Solo 12 meses | 1 GB | 1 vCPU | Después cobra |
| **Fly.io** ✅ | Sí (con límites) | 256 MB | compartida | Fácil de usar |
| **Railway.app** ⚠️ | 5 USD de crédito/mes | variable | variable | Se agota rápido |

> [!IMPORTANT]
> **Recomendación: Oracle Cloud Free Tier.** Es gratis **para siempre** (no vence como AWS), da más recursos que Google, y tiene IPs públicas estáticas sin costo.

---

## 🥇 Opción 1 — Oracle Cloud Free Tier (Recomendada)

### Paso 1 — Crear cuenta

1. Ir a [oracle.com/cloud/free](https://www.oracle.com/cloud/free/)
2. Clic en **"Start for free"**
3. Completar el registro:
   - Requiere tarjeta de crédito para verificación (NO cobra nada)
   - Elegir región: **South America East (São Paulo)** o **US East (Ashburn)** — ambas tienen instancias gratuitas
4. Verificar el email y entrar al panel

> [!NOTE]
> Oracle **no cobra** mientras uses solo recursos del Free Tier. La tarjeta es únicamente para verificar identidad. Si querés más seguridad, podés poner un límite de gasto de $0 en configuración.

---

### Paso 2 — Crear la VM

1. En el panel, ir a: **Compute → Instances → Create Instance**

2. Configuración de la instancia:
   - **Name:** `lanplay-server`
   - **Image:** Ubuntu 22.04 Minimal (o la versión LTS más reciente disponible)
   - **Shape:**
     - Elegí **"Always Free-eligible"** en el filtro
     - Seleccioná **VM.Standard.A1.Flex** (ARM) o **VM.Standard.E2.1.Micro** (AMD)
     - Para A1: ponés 1 OCPU y 1 GB RAM → gratis siempre

3. **Networking:**
   - Crear nueva VCN o usar la por defecto
   - **Asignar IP pública:** Sí (anotar esta IP — es la que van a usar las Switches)

4. **SSH Keys:**
   - Clic en "Generate a key pair for me"
   - **Descargar la clave privada** (`ssh-key-xxxx.key`) — guardala bien, sin ella no podés entrar

5. **Create** → esperar ~2 minutos

---

### Paso 3 — Conectarse por SSH

En PowerShell en tu PC:

```powershell
# Darle permisos correctos a la clave (solo la primera vez)
icacls "C:\ruta\a\ssh-key-xxxx.key" /inheritance:r /grant:r "$($env:USERNAME):(R)"

# Conectarse (reemplazá IP_PUBLICA con la IP de Oracle)
ssh -i "C:\ruta\a\ssh-key-xxxx.key" ubuntu@IP_PUBLICA
```

Si usás PuTTY, convertí la `.key` a `.ppk` con PuTTYgen primero.

> [!TIP]
> También podés usar **Windows Terminal** o **VS Code Remote SSH** para conectarte más cómodamente.

---

### Paso 4 — Instalar Docker en la VM

Una vez conectado por SSH, ejecutar **en la VM**:

```bash
# Actualizar paquetes
sudo apt-get update && sudo apt-get upgrade -y

# Instalar dependencias
sudo apt-get install -y ca-certificates curl gnupg

# Agregar repositorio oficial de Docker
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
sudo chmod a+r /etc/apt/keyrings/docker.gpg

echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
  https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

# Instalar Docker
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

# Agregar tu usuario al grupo docker (para no usar sudo siempre)
sudo usermod -aG docker $USER

# Verificar instalación
sudo docker run hello-world
```

Cerrar y volver a conectar por SSH para que el grupo `docker` tome efecto:
```bash
exit
# luego volver a conectar con ssh
```

---

### Paso 5 — Levantar el servidor LAN Play

```bash
docker run -d \
  --name lanplay-server \
  --restart unless-stopped \
  -p 11451:11451/udp \
  -p 11451:11451/tcp \
  shaklinedj/switch-lan-play
```

Verificar que está corriendo:
```bash
docker ps
docker logs lanplay-server
```

Ver el endpoint de estado:
```bash
curl http://localhost:11451/info
# Respuesta: { "online": 0 }
```

---

### Paso 6 — Abrir el puerto 11451 en Oracle (¡Imprescindible!)

Oracle tiene **dos capas de firewall**: la de la nube (Security List) y la del sistema operativo (iptables). Hay que abrir ambas.

#### 6a — Security List (firewall de Oracle)

1. En el panel de Oracle, ir a: **Networking → Virtual Cloud Networks**
2. Click en tu VCN → **Security Lists** → **Default Security List**
3. Clic en **"Add Ingress Rules"**
4. Agregar estas dos reglas:

**Regla UDP (obligatoria):**
```
Source CIDR: 0.0.0.0/0
IP Protocol: UDP
Destination Port Range: 11451
```

**Regla TCP (para el endpoint /info):**
```
Source CIDR: 0.0.0.0/0
IP Protocol: TCP
Destination Port Range: 11451
```

5. Clic en **"Add Ingress Rules"** para guardar.

#### 6b — iptables (firewall del sistema operativo Ubuntu)

```bash
# Abrir puerto 11451 UDP y TCP
sudo iptables -I INPUT -p udp --dport 11451 -j ACCEPT
sudo iptables -I INPUT -p tcp --dport 11451 -j ACCEPT

# Guardar las reglas para que persistan al reiniciar
sudo apt-get install -y iptables-persistent
sudo netfilter-persistent save
```

---

### Paso 7 — Verificar desde internet

Desde tu PC en Windows:

```powershell
# Verificar el endpoint HTTP
Invoke-WebRequest -Uri "http://IP_PUBLICA_ORACLE:11451/info" -UseBasicParsing
```

Si responde `{ "online": 0 }`, el servidor está accesible desde internet. ✅

---

### Paso 8 — Configurar la Switch

En la app **LanPlay Setup** de la Switch, ingresá:

```
IP_PUBLICA_ORACLE:11451
```

La IP pública de Oracle es **estática** — nunca cambia, así que no necesitás DNS dinámico.

---

### Administración del servidor

```bash
# Ver jugadores conectados en tiempo real
watch -n 2 'curl -s http://localhost:11451/info'

# Logs en tiempo real
docker logs -f lanplay-server

# Reiniciar servidor
docker restart lanplay-server

# Actualizar a la última versión
docker pull shaklinedj/switch-lan-play
docker rm -f lanplay-server
docker run -d --name lanplay-server --restart unless-stopped \
  -p 11451:11451/udp -p 11451:11451/tcp shaklinedj/switch-lan-play
```

---

## 🥈 Opción 2 — Google Cloud Free Tier

> Gratis para siempre en regiones `us-central1`, `us-east1`, `us-west1`.

### Pasos resumidos

1. Ir a [cloud.google.com/free](https://cloud.google.com/free) → Crear cuenta (pide tarjeta, da $300 de crédito por 90 días + e2-micro gratis siempre)
2. **Compute Engine → VM Instances → Create Instance**
   - Series: **E2** → Machine type: **e2-micro**
   - Región: `us-central1` (Iowa)
   - Boot disk: Ubuntu 22.04 LTS, 30 GB
   - Firewall: activar "Allow HTTP traffic" (después abrimos el nuestro)
3. Conectar con SSH desde el navegador (botón SSH en el panel)
4. Instalar Docker con los mismos comandos de Oracle (Paso 4)
5. Levantar el contenedor (Paso 5)

#### Abrir puerto en Google Cloud

```bash
# Desde Cloud Shell o tu terminal local con gcloud instalado
gcloud compute firewall-rules create lanplay-udp \
  --allow udp:11451 \
  --source-ranges 0.0.0.0/0 \
  --description "Switch LAN Play relay"

gcloud compute firewall-rules create lanplay-tcp \
  --allow tcp:11451 \
  --source-ranges 0.0.0.0/0
```

O desde el panel: **VPC Network → Firewall → Create Firewall Rule**

> [!WARNING]
> En Google Cloud, la IP pública de una VM **por defecto es efímera** (cambia al reiniciar). Para fijarla, ir a **VPC Network → External IP addresses** y promoverla a "Static" (tiene un costo mínimo si no está adjunta a una VM encendida, pero gratis si siempre está en uso).

---

## 🥉 Opción 3 — AWS Free Tier (solo 12 meses)

> [!CAUTION]
> AWS Free Tier vence a los **12 meses** desde la creación de la cuenta. Después de eso, el t3.micro cobra ~$9 USD/mes. Configurá alertas de billing para no llevarte sorpresas.

### Pasos resumidos

1. Ir a [aws.amazon.com/free](https://aws.amazon.com/free/) → Crear cuenta
2. **EC2 → Launch Instance**
   - AMI: Ubuntu Server 22.04 LTS (Free tier eligible)
   - Instance type: **t3.micro** (Free tier eligible)
   - Key pair: crear nuevo, descargar `.pem`
   - Security Group: agregar regla Inbound: UDP 11451, TCP 11451, fuente 0.0.0.0/0
3. SSH:
   ```powershell
   ssh -i "tu-key.pem" ubuntu@IP_PUBLICA_AWS
   ```
4. Instalar Docker y levantar el contenedor (mismos comandos)

---

## 🏁 Resumen ejecutivo

### Si querés la opción más fácil y gratuita para siempre:

```
Oracle Cloud Free Tier → VM Ubuntu → Docker → shaklinedj/switch-lan-play
```

### Flujo completo en 5 minutos (una vez tenés la cuenta y la VM):

```bash
# Conectar a la VM
ssh -i key.key ubuntu@IP_ORACLE

# Instalar Docker (una sola vez)
curl -fsSL https://get.docker.com | sudo sh
sudo usermod -aG docker $USER && exit

# Reconectar y levantar el servidor
ssh -i key.key ubuntu@IP_ORACLE
docker run -d --name lanplay-server --restart unless-stopped \
  -p 11451:11451/udp -p 11451:11451/tcp shaklinedj/switch-lan-play

# Verificar
curl http://localhost:11451/info
```

### En la Switch

```
LanPlay Setup → A → escribir IP_ORACLE:11451 → + → reiniciar
```
