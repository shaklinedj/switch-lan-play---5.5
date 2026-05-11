/**
 * pc-peer.ts
 * Conecta tu PC al relay SLP como peer completo en la red 10.13.0.0/16.
 * La Switch verá tu PC como otro jugador en la sala.
 *
 * Uso:
 *   npx ts-node tools/pc-peer.ts [relay] [port] [myVirtualIP]
 * Ejemplo:
 *   npx ts-node tools/pc-peer.ts tekn0.net 11451 10.13.1.100
 */

import * as dgram from 'dgram'
import * as dns from 'dns'

// ─── Configuración ────────────────────────────────────────────────────────────
const RELAY_HOST   = process.argv[2] ?? 'tekn0.net'
const RELAY_PORT   = parseInt(process.argv[3] ?? '11451')
const MY_VIRTUAL_IP = process.argv[4] ?? '10.13.1.100'

const MY_MAC = Buffer.from([0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE])  // MAC virtual (local admin)
const BROADCAST_MAC = Buffer.from([0xff,0xff,0xff,0xff,0xff,0xff])
const SUBNET_NET   = [10, 13, 0, 0]
const SUBNET_MASK  = [255, 255, 0, 0]

// ─── Constantes de protocolo SLP ──────────────────────────────────────────────
const TYPE_KEEPALIVE   = 0x00
const TYPE_IPV4        = 0x01
const TYPE_PING        = 0x02
const TYPE_IPV4_FRAG   = 0x03
const TYPE_AUTH_ME     = 0x04
const TYPE_INFO        = 0x10

// ─── Constantes Ethernet/IP/ARP ───────────────────────────────────────────────
const ETHER_TYPE_IPV4  = 0x0800
const ETHER_TYPE_ARP   = 0x0806
const ARP_REQUEST      = 1
const ARP_REPLY        = 2

// ─── Helpers ──────────────────────────────────────────────────────────────────
function ip2buf(ip: string): Buffer {
  return Buffer.from(ip.split('.').map(Number))
}
function buf2ip(b: Buffer, offset = 0): string {
  return `${b[offset]}.${b[offset+1]}.${b[offset+2]}.${b[offset+3]}`
}
function buf2mac(b: Buffer, offset = 0): string {
  return Array.from(b.slice(offset, offset+6)).map((x: number) => x.toString(16).padStart(2,'0')).join(':')
}
function isBroadcast(dstBuf: Buffer, offset = 0): boolean {
  const ip = dstBuf.slice(offset, offset+4)
  // 255.255.255.255 ó dirección broadcast de subred 10.13.255.255
  if (ip[0]===255 && ip[1]===255 && ip[2]===255 && ip[3]===255) return true
  if (ip[0]===SUBNET_NET[0] && ip[1]===SUBNET_NET[1] && ip[2]===255 && ip[3]===255) return true
  return false
}

// ─── Tabla ARP local ──────────────────────────────────────────────────────────
const arpTable = new Map<string, Buffer>() // ip -> mac

// ─── Construcción de paquetes Ethernet / ARP ──────────────────────────────────

/**
 * Construye un frame Ethernet completo a partir de un payload IPv4 o ARP.
 */
function buildEtherFrame(dstMac: Buffer, srcMac: Buffer, etherType: number, payload: Buffer): Buffer {
  const frame = Buffer.alloc(14 + payload.length)
  dstMac.copy(frame, 0)
  srcMac.copy(frame, 6)
  frame.writeUInt16BE(etherType, 12)
  payload.copy(frame, 14)
  return frame
}

/**
 * Construye un paquete ARP reply: "myIP tiene myMac, en respuesta a quién pregunta"
 */
function buildArpReply(targetMac: Buffer, targetIp: Buffer, senderMac: Buffer, senderIp: Buffer): Buffer {
  const arp = Buffer.alloc(28)
  arp.writeUInt16BE(0x0001, 0)   // hardware type: Ethernet
  arp.writeUInt16BE(0x0800, 2)   // protocol: IPv4
  arp[4] = 6                     // hardware size
  arp[5] = 4                     // protocol size
  arp.writeUInt16BE(ARP_REPLY, 6)
  senderMac.copy(arp, 8)         // sender mac (nosotros)
  senderIp.copy(arp, 14)         // sender ip  (nosotros)
  targetMac.copy(arp, 18)        // target mac
  targetIp.copy(arp, 24)         // target ip
  return arp
}

/**
 * Construye un ARP request: "¿quién tiene targetIp?"
 */
function buildArpRequest(senderMac: Buffer, senderIp: Buffer, targetIp: Buffer): Buffer {
  const arp = Buffer.alloc(28)
  arp.writeUInt16BE(0x0001, 0)
  arp.writeUInt16BE(0x0800, 2)
  arp[4] = 6
  arp[5] = 4
  arp.writeUInt16BE(ARP_REQUEST, 6)
  senderMac.copy(arp, 8)
  senderIp.copy(arp, 14)
  BROADCAST_MAC.copy(arp, 18)
  targetIp.copy(arp, 24)
  return arp
}

/**
 * Construye un paquete IPv4 UDP simple (para anuncios o test).
 * checksum en 0 (algunos stacks lo aceptan, para debug es suficiente)
 */
function buildIPv4UDP(
  srcIp: Buffer, dstIp: Buffer,
  srcPort: number, dstPort: number,
  data: Buffer
): Buffer {
  const udpLen = 8 + data.length
  const udp = Buffer.alloc(udpLen)
  udp.writeUInt16BE(srcPort, 0)
  udp.writeUInt16BE(dstPort, 2)
  udp.writeUInt16BE(udpLen, 4)
  udp.writeUInt16BE(0, 6)        // checksum (0 = ignorar)
  data.copy(udp, 8)

  const ipLen = 20 + udpLen
  const ip = Buffer.alloc(ipLen)
  ip[0] = 0x45                   // version 4, IHL 5
  ip[1] = 0                      // DSCP
  ip.writeUInt16BE(ipLen, 2)     // total length
  ip.writeUInt16BE(0x1234, 4)    // identification
  ip.writeUInt16BE(0x4000, 6)    // flags: don't fragment
  ip[8] = 64                     // TTL
  ip[9] = 17                     // protocol: UDP
  ip.writeUInt16BE(0, 10)        // checksum (0 por ahora)
  srcIp.copy(ip, 12)
  dstIp.copy(ip, 16)
  udp.copy(ip, 20)

  // Calcular checksum IPv4
  let sum = 0
  for (let i = 0; i < 20; i += 2) sum += ip.readUInt16BE(i)
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16)
  ip.writeUInt16BE(~sum & 0xffff, 10)

  return ip
}

// ─── Cliente SLP ──────────────────────────────────────────────────────────────

class SLPPeerClient {
  private socket = dgram.createSocket('udp4')
  private relayAddr!: string
  private myVirtualIp: Buffer
  private keepaliveTimer!: NodeJS.Timeout
  private pktCount = { rx: 0, tx: 0 }

  constructor() {
    this.myVirtualIp = ip2buf(MY_VIRTUAL_IP)
  }

  async start() {
    this.relayAddr = await this.resolve(RELAY_HOST)
    console.log(`[SLP-Peer] Relay: ${RELAY_HOST} → ${this.relayAddr}:${RELAY_PORT}`)
    console.log(`[SLP-Peer] IP virtual: ${MY_VIRTUAL_IP}  MAC virtual: ${buf2mac(MY_MAC)}`)
    console.log(`[SLP-Peer] Red virtual: ${SUBNET_NET.join('.')}/16`)
    console.log(`─────────────────────────────────────────────`)

    this.socket.on('message', (msg: Buffer) => this.onMessage(msg))
    this.socket.bind(0, () => {
      this.sendKeepalive()
      this.keepaliveTimer = setInterval(() => this.sendKeepalive(), 10_000)
      // Anunciar nuestra presencia con un ARP gratuito
      setTimeout(() => this.sendGratuitousArp(), 500)
    })
  }

  private resolve(host: string): Promise<string> {
    return new Promise((res, rej) => {
      dns.lookup(host, 4, (err: NodeJS.ErrnoException | null, addr: string) => err ? rej(err) : res(addr))
    })
  }

  // ── Envío al relay ──────────────────────────────────────────────────────────

  private sendRaw(type: number, payload: Buffer) {
    const pkt = Buffer.alloc(1 + payload.length)
    pkt[0] = type
    payload.copy(pkt, 1)
    this.socket.send(pkt, RELAY_PORT, this.relayAddr)
    this.pktCount.tx++
  }

  /** Envía un paquete IPv4 crudo al relay (el relay lo reenvía por IP destino o broadcast) */
  sendIPv4(ipv4Packet: Buffer) {
    this.sendRaw(TYPE_IPV4, ipv4Packet)
  }

  sendKeepalive() {
    this.sendRaw(TYPE_KEEPALIVE, Buffer.alloc(0))
  }

  /** ARP gratuito: anuncia nuestra IP/MAC a todos los peers */
  sendGratuitousArp() {
    const arp = buildArpRequest(MY_MAC, this.myVirtualIp, this.myVirtualIp)
    const frame = buildEtherFrame(BROADCAST_MAC, MY_MAC, ETHER_TYPE_ARP, arp)
    // Empaquetamos el frame Ethernet como IPv4 tipo broadcast
    // El protocolo SLP envuelve directamente el IPv4, pero para ARP debemos usar broadcast
    // Usamos sendIPv4 con el payload siendo el frame ARP encapsulado en un "IPv4 falso"
    // En realidad en el protocolo lan-play el tipo IPV4 lleva el payload IPv4 puro.
    // Para ARP, se usan paquetes tipo IPV4 pero el payload es el frame desde la capa IPv4 del gateway.
    // En la práctica el gateway/pcap envía todo el frame Ethernet al relay como IPv4 del gateway.
    // Dado que somos un peer PC, enviamos un IPv4 UDP broadcast de anuncio.
    const bcIp = Buffer.from([10,13,255,255])
    const announceData = Buffer.from(`SLP-PC-PEER:${MY_VIRTUAL_IP}`)
    const ipv4 = buildIPv4UDP(this.myVirtualIp, bcIp, 11451, 11451, announceData)
    this.sendIPv4(ipv4)
    console.log(`[ARP/announce] Anuncio enviado → 10.13.255.255`)
  }

  // ── Recepción del relay ─────────────────────────────────────────────────────

  private onMessage(msg: Buffer) {
    if (msg.length === 0) return
    this.pktCount.rx++
    const type = msg[0] & 0x7f
    const payload = msg.slice(1)

    switch (type) {
      case TYPE_KEEPALIVE:
        break

      case TYPE_INFO:
        console.log(`[Servidor]: ${payload.toString()}`)
        break

      case TYPE_AUTH_ME:
        console.warn(`[Auth] El relay pide login (no configurado). Reinicia con usuario/contraseña.`)
        break

      case TYPE_IPV4:
        this.processIPv4(payload)
        break

      case TYPE_IPV4_FRAG:
        this.processIPv4Frag(payload)
        break
    }
  }

  private processIPv4(payload: Buffer) {
    if (payload.length < 20) return
    const protocol  = payload[9]
    const srcIp     = buf2ip(payload, 12)
    const dstIp     = buf2ip(payload, 16)
    const isBc      = isBroadcast(payload, 16)

    if (protocol === 17) {
      // UDP
      if (payload.length < 28) return
      const srcPort = payload.readUInt16BE(20)
      const dstPort = payload.readUInt16BE(22)
      const udpData = payload.slice(28)

      // Detectar paquetes LDN (puerto 11452, magic 0x11451400)
      if ((srcPort === 11452 || dstPort === 11452) && udpData.length >= 12) {
        const magic = udpData.readUInt32LE(0)
        if (magic === 0x11451400) {
          const ldnType = udpData[4]
          const ldnCompressed = udpData[5]
          const ldnBodyLen = udpData.readUInt16LE(6)
          const ldnTypeNames: Record<number, string> = { 0: 'Scan', 1: 'ScanResp', 2: 'Connect', 3: 'SyncNetwork' }
          const typeName = ldnTypeNames[ldnType] ?? `Unknown(${ldnType})`
          console.log(`[LDN] ${srcIp} → ${dstIp}  type=${typeName}  bodyLen=${ldnBodyLen}  total=${udpData.length}`)
          if (ldnType === 1 && udpData.length > 20) {
            // ScanResp — contiene NetworkInfo (puede estar comprimido con RLE de 0x00)
            const rawBody = udpData.slice(12) // saltar LDN header de 12 bytes

            // Descomprimir si compressed=1 (RLE: byte 0x00 seguido de count expande a count+1 ceros)
            let body = rawBody
            if (ldnCompressed) {
              const decompLen = udpData.readUInt16LE(8) // decompress_length en header bytes 8-9
              const out = Buffer.alloc(decompLen > 0 ? decompLen : 2048, 0)
              let r = 0, w = 0
              while (r < rawBody.length && w < out.length) {
                const c = rawBody[r++]
                out[w++] = c
                if (c === 0 && r < rawBody.length) {
                  const cnt = rawBody[r++]
                  for (let x = 0; x < cnt && w < out.length; x++) out[w++] = 0
                }
              }
              body = out.slice(0, w)
            }

            // NetworkInfo layout (little-endian):
            //   networkId.intentId.localCommunicationId  u64 LE  @ offset 0
            //   networkId.intentId._unk1[2]              @ offset 8
            //   networkId.intentId.sceneId               u16 LE  @ offset 10
            //   ... (networkId total 32 bytes)
            //   CommonNetworkInfo (48 bytes)             @ offset 32
            //   LdnNetworkInfo.unkRandom[16]             @ offset 80
            //   ... nodeCountMax u8                      @ offset 102
            //   ... nodeCount u8                         @ offset 103
            //   nodes[0].ipv4Address u32 LE              @ offset 104
            //   nodes[0].macAddress[6]                   @ offset 108
            //   nodes[0].nodeId s8                       @ offset 114
            //   nodes[0].isConnected s8                  @ offset 115
            //   nodes[0].userName[33]                    @ offset 116
            const lcWord0 = body.readUInt32LE(0)
            const lcWord1 = body.readUInt32LE(4)
            const lcIdHex = `0x${lcWord1.toString(16).padStart(8,'0')}${lcWord0.toString(16).padStart(8,'0')}`

            const sceneId = body.readUInt16LE(10)

            // Nombre de usuario del nodo 0
            let userName = ''
            if (body.length >= 150) {
              userName = body.slice(116, 149).toString('utf8').replace(/\0.*/, '').trim()
            }

            // Intentar leer ASCII del payload para debug
            const asciiParts: string[] = []
            for (let i = 0; i < Math.min(body.length, 256); i++) {
              const c = body[i]
              if (c >= 0x20 && c < 0x7F) asciiParts.push(String.fromCharCode(c))
              else if (asciiParts.length > 0 && asciiParts[asciiParts.length-1] !== '|') asciiParts.push('|')
            }
            const asciiStr = asciiParts.join('').split('|').filter(s => s.length >= 3).join(' | ')

            // Lookup de juegos conocidos por localCommunicationId
            const KNOWN_GAMES: Record<string, string> = {
              '0x1520000002200000': 'Mario Kart 8 Deluxe',   // verificado en vivo
              '0x0100152000022000': 'Mario Kart 8 Deluxe',
              '0x01006a800016e000': 'Super Smash Bros. Ultimate',
              '0x01006f8002326000': 'Animal Crossing: New Horizons',
              '0x01003bc0000a0000': 'Splatoon 2',
              '0x0100c25003e6a000': 'Splatoon 3',
              '0x0100abf008968000': 'Pokemon Sword',
              '0x01008db008c2c000': 'Pokemon Shield',
              '0x0102c6000fd20000': 'Pokemon Scarlet',
              '0x0102c6000fd22000': 'Pokemon Violet',
              '0x0100d71004694000': 'Minecraft',
              '0x0100b04011742000': 'Monster Hunter Rise',
              '0x0100770008dd8000': 'Luigi\'s Mansion 3',
              '0x0100f2c0115b6000': 'Mario Party Superstars',
              '0x01006fe013472000': 'Kirby\'s Return to Dream Land Deluxe',
              '0x0100904005c52000': 'Mario Tennis Aces',
            }
            const gameName = KNOWN_GAMES[lcIdHex.toLowerCase()] ?? `Juego desconocido`

            console.log(`[LDN/ScanResp] *** SALA ENCONTRADA desde ${srcIp} ***`)
            console.log(`[LDN/ScanResp]   Juego       : ${gameName}`)
            console.log(`[LDN/ScanResp]   CommId      : ${lcIdHex}`)
            console.log(`[LDN/ScanResp]   SceneId     : ${sceneId}`)
            if (userName) console.log(`[LDN/ScanResp]   Usuario     : ${userName}`)
            if (asciiStr) console.log(`[LDN/ScanResp]   Texto ASCII : ${asciiStr}`)
          }
          return
        }
      }

      const text    = udpData.length > 0 && udpData.every(b => b >= 32 && b < 127)
                      ? `"${udpData.toString()}"` : `[${udpData.length} bytes]`
      console.log(`[IPv4/UDP] ${srcIp}:${srcPort} → ${dstIp}:${dstPort}  data: ${text}`)

      // Responder ARP si nos preguntan (encapsulado en UDP discovery)
    } else if (protocol === 6) {
      // TCP
      if (payload.length < 40) return
      const srcPort = payload.readUInt16BE(20)
      const dstPort = payload.readUInt16BE(22)
      console.log(`[IPv4/TCP] ${srcIp}:${srcPort} → ${dstIp}:${dstPort}`)
    } else if (protocol === 1) {
      // ICMP
      const icmpType = payload[20]
      console.log(`[IPv4/ICMP] ${srcIp} → ${dstIp}  icmp_type=${icmpType}`)
    } else {
      console.log(`[IPv4/proto=${protocol}] ${srcIp} → ${dstIp}  len=${payload.length}`)
    }

    // Actualizar tabla ARP con la IP origen
    arpTable.set(srcIp, payload.slice(12, 16))
  }

  private processIPv4Frag(payload: Buffer) {
    if (payload.length < 8) return
    const srcIp = buf2ip(payload, 0)
    const dstIp = buf2ip(payload, 4)
    const part     = payload[10]
    const total    = payload[11]
    console.log(`[Frag] ${srcIp} → ${dstIp}  parte ${part+1}/${total}`)
  }

  // ── API pública para enviar desde consola ───────────────────────────────────

  /** Envía un ping ICMP echo request a una IP virtual */
  ping(targetIp: string) {
    const dstIpBuf = ip2buf(targetIp)
    const icmp = Buffer.alloc(8)
    icmp[0] = 8    // echo request
    icmp[1] = 0
    icmp.writeUInt16BE(0, 2)  // checksum
    icmp.writeUInt16BE(1, 4)  // id
    icmp.writeUInt16BE(1, 6)  // seq

    let sum = 0
    for (let i = 0; i < icmp.length; i += 2) sum += icmp.readUInt16BE(i)
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16)
    icmp.writeUInt16BE(~sum & 0xffff, 2)

    const ipLen = 20 + icmp.length
    const ip = Buffer.alloc(ipLen)
    ip[0] = 0x45; ip[8] = 64; ip[9] = 1
    ip.writeUInt16BE(ipLen, 2)
    ip.writeUInt16BE(0x1235, 4)
    ip.writeUInt16BE(0x4000, 6)
    this.myVirtualIp.copy(ip, 12)
    dstIpBuf.copy(ip, 16)
    let s = 0
    for (let i = 0; i < 20; i += 2) s += ip.readUInt16BE(i)
    while (s >> 16) s = (s & 0xffff) + (s >> 16)
    ip.writeUInt16BE(~s & 0xffff, 10)
    icmp.copy(ip, 20)

    this.sendIPv4(ip)
    console.log(`[PING] Enviado ICMP echo request → ${targetIp}`)
  }

  /** Envía un UDP a una IP/puerto virtual */
  sendUDP(targetIp: string, targetPort: number, message: string) {
    const dstIpBuf = ip2buf(targetIp)
    const data     = Buffer.from(message)
    const ipv4     = buildIPv4UDP(this.myVirtualIp, dstIpBuf, 11451, targetPort, data)
    this.sendIPv4(ipv4)
    console.log(`[UDP] Enviado → ${targetIp}:${targetPort}  "${message}"`)
  }

  stats() {
    console.log(`[Stats] RX: ${this.pktCount.rx}  TX: ${this.pktCount.tx}  ARP conocidos: ${arpTable.size}`)
    for (const [ip] of arpTable) {
      console.log(`  ${ip}`)
    }
  }

  /**
   * Envía un LDN Scan broadcast (12 bytes) a 10.13.255.255:11452
   * El host Switch responde con un Advertise unicast de vuelta a nosotros.
   */
  sendLDNScan() {
    // Formato real ldn_packet_header (12 bytes, packed):
    //   uint32_t magic;             // offset 0  (LE)
    //   uint8_t  type;              // offset 4
    //   uint8_t  compressed;        // offset 5
    //   uint16_t length;            // offset 6  (LE)
    //   uint16_t decompress_length; // offset 8  (LE)
    //   uint8_t  reserved[2];       // offset 10
    const ldnPayload = Buffer.alloc(12)
    ldnPayload.writeUInt32LE(0x11451400, 0)  // magic
    ldnPayload[4] = 0x00                      // type = Scan (0)
    ldnPayload[5] = 0x00                      // compressed = 0
    ldnPayload.writeUInt16LE(0, 6)            // length = body size (Scan has NO body)
    ldnPayload.writeUInt16LE(0, 8)            // decompress_length = 0
    // bytes 10-11 = 0x00 (reserved)

    const bcIp = Buffer.from([10, 13, 255, 255])
    const ipv4 = buildIPv4UDP(this.myVirtualIp, bcIp, 11452, 11452, ldnPayload)
    this.sendIPv4(ipv4)
    console.log(`[LDN/Scan] Broadcast enviado → 10.13.255.255:11452  [12 bytes]`)
  }

  private autoscanTimer: NodeJS.Timeout | null = null

  startAutoScan(intervalMs = 2000) {
    if (this.autoscanTimer) {
      console.log('[AutoScan] Ya está activo.')
      return
    }
    this.sendLDNScan()
    this.autoscanTimer = setInterval(() => this.sendLDNScan(), intervalMs)
    console.log(`[AutoScan] Iniciado cada ${intervalMs}ms — esperando Advertise de la Switch...`)
  }

  stopAutoScan() {
    if (this.autoscanTimer) {
      clearInterval(this.autoscanTimer)
      this.autoscanTimer = null
      console.log('[AutoScan] Detenido.')
    }
  }

  stop() {
    this.stopAutoScan()
    clearInterval(this.keepaliveTimer)
    this.socket.close()
    console.log('[SLP-Peer] Desconectado.')
  }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

const client = new SLPPeerClient()
client.start()

// Interfaz de comandos por stdin
import * as readline from 'readline'
const rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: '> ' })
console.log(`Comandos: ping <ip>  |  udp <ip> <puerto> <mensaje>  |  scan  |  autoscan [ms]  |  stopscan  |  stats  |  quit`)
rl.prompt()
rl.on('line', (line: string) => {
  const parts = line.trim().split(/\s+/)
  switch (parts[0]) {
    case 'ping':
      if (parts[1]) client.ping(parts[1])
      else console.log('uso: ping <ip>')
      break
    case 'udp':
      if (parts[1] && parts[2] && parts[3])
        client.sendUDP(parts[1], parseInt(parts[2]), parts.slice(3).join(' '))
      else console.log('uso: udp <ip> <puerto> <mensaje>')
      break
    case 'scan':
      client.sendLDNScan()
      break
    case 'autoscan':
      client.startAutoScan(parts[1] ? parseInt(parts[1]) : 2000)
      break
    case 'stopscan':
      client.stopAutoScan()
      break
    case 'stats':
      client.stats()
      break
    case 'quit':
    case 'exit':
      client.stop()
      process.exit(0)
      break
    default:
      if (parts[0]) console.log('Comandos: ping <ip>  |  udp <ip> <puerto> <mensaje>  |  scan  |  autoscan [ms]  |  stopscan  |  stats  |  quit')
  }
  rl.prompt()
})
