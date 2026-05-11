// Script para decodificar un payload ScanResp de LDN
// Pega aquí tu payload en formato hex (sin espacios)


// Pega aquí tu payload hex (sin espacios)
const payloadHex = "00002002000020150000010007526c41a96ad1c5a8509e6e1548785a06020000e602a8c020313233343536373831323334353637383132333435363738313233";

function hexToBytes(hex) {
    const bytes = [];
    for (let c = 0; c < hex.length; c += 2)
        bytes.push(parseInt(hex.substr(c, 2), 16));
    return bytes;
}

function bytesToIp(bytes) {
    return bytes.join('.');
}

function bytesToAscii(bytes) {
    return String.fromCharCode(...bytes).replace(/\0.*/, '');
}

const buf = hexToBytes(payloadHex);

// Hex + ASCII dump for visual inspection
function hexAsciiDump(bytes, width = 16) {
    for (let i = 0; i < bytes.length; i += width) {
        const hex = bytes.slice(i, i + width).map(b => b.toString(16).padStart(2, '0')).join(' ');
        const ascii = bytes.slice(i, i + width).map(b => (b >= 32 && b <= 126) ? String.fromCharCode(b) : '.').join('');
        console.log(i.toString().padStart(3, '0'), hex.padEnd(width * 3), ascii);
    }
}

console.log("\n--- HEX + ASCII DUMP ---");
hexAsciiDump(buf);
console.log("--- END DUMP ---\n");


// BSSID (primeros 6 bytes)
const bssid = buf.slice(0, 6).map(b => b.toString(16).padStart(2, '0')).join(':');

// Buscar y mostrar todos los strings ASCII legibles de longitud >=6
function findAsciiStrings(bytes, minLen = 6) {
    let results = [];
    let current = [];
    for (let b of bytes) {
        if (b >= 32 && b <= 126) {
            current.push(b);
        } else {
            if (current.length >= minLen) results.push(bytesToAscii(current));
            current = [];
        }
    }
    if (current.length >= minLen) results.push(bytesToAscii(current));
    return results;
}

console.log("BSSID:", bssid);
console.log("Strings legibles en el payload:");
findAsciiStrings(buf).forEach((s, i) => console.log(`  [${i+1}] ${s}`));
