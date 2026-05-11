# switch-lan-play — Relay Server

This is the UDP relay server that bridges LAN-play packets between Switch
consoles.  It's a simple Node.js/TypeScript app with minimal resource requirements
(one 512 MB VPS handles hundreds of simultaneous players).

---

## Option A — Free cloud hosting (recommended)

### Oracle Cloud Free Tier ⭐ (best option — forever free)

Oracle gives you **2 ARM VMs with 1 GB RAM each for free, forever**.

1. Sign up at <https://cloud.oracle.com/> (credit card required for verification,
   but you are **never charged** on Always Free resources)
2. Create an **Ampere A1** VM instance (ARM, 1 OCPU, 1 GB RAM, Ubuntu 22.04)
3. SSH into the VM and run:

```sh
# Install Docker
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
# Log out and back in, then:

# Clone the repo
git clone https://github.com/shaklinedj/switch-lan-play
cd switch-lan-play/server

# Start the server
docker compose up -d

# Open UDP port 11451 in the Oracle firewall
# (also open it in the VCN Security List in the Oracle web console)
sudo iptables -I INPUT -p udp --dport 11451 -j ACCEPT
```

4. Find your VM's **Public IP address** in the Oracle console.
5. Share `YOUR_PUBLIC_IP:11451` with your players.

---

### fly.io (free hobby tier)

```sh
# Install flyctl
curl -L https://fly.io/install.sh | sh

# From the server/ directory:
fly launch --no-deploy
# Edit fly.toml: set [[services]] internal_port = 11451, protocol = "udp"
fly deploy
```

---

### Railway.app / Render.com

Both have free tiers.  Deploy using the Dockerfile in this directory.
Set the exposed port to `11451/udp`.

---

## Option B — Your own VPS (Hostinger, DigitalOcean, Hetzner…)

Any VPS with Ubuntu and a public IP works.  Minimum specs: 512 MB RAM, 1 vCPU.

```sh
# Install Docker
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker

# Clone & start
git clone https://github.com/shaklinedj/switch-lan-play
cd switch-lan-play/server
docker compose up -d

# Open the firewall
sudo ufw allow 11451/udp
sudo ufw allow 11451/tcp   # for the monitor API (optional)
```

---

## Option C — Run locally (for testing or LAN parties)

```sh
cd server
npm install
npm run build
npm run server
```

The server binds to `0.0.0.0:11451` by default.

### Custom port

```sh
node dist/main.js --port 12345
```

---

## Verify the server is working

```sh
# From any machine with netcat:
echo -n '\x02' | nc -u YOUR_SERVER_IP 11451
# You should receive a 4-byte pong response.
```

Or open `http://YOUR_SERVER_IP:11451` in a browser — it shows a JSON
status page with the number of connected clients.

---

## Optional: password protection

Edit `server/users.json` (see `users_schema.json` for the format) and start with:

```sh
node dist/main.js --jsonAuth ./users.json
```

Players then set `username` and `password` in their `config.ini`.

---

## Port forwarding summary

| Port | Protocol | Purpose |
|------|----------|---------|
| 11451 | **UDP** | Relay (game packets — **required**) |
| 11451 | TCP | Monitor API (optional, for stats) |

---

## Updating

```sh
cd switch-lan-play/server
git pull
docker compose up -d --build
```
