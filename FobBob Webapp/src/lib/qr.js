const BASE32_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';

export function toBase32(bytes) {
  let bits = 0, value = 0, output = '';
  for (const byte of bytes) {
    value = (value << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      output += BASE32_CHARS[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) output += BASE32_CHARS[(value << (5 - bits)) & 31];
  return output; // no padding — ESP32 decoder ignores it anyway
}

// Minimal protobuf decoder — handles varint (wire 0) and length-delimited (wire 2)
function readVarint(buf, offset) {
  let result = 0, shift = 0;
  while (offset < buf.length) {
    const b = buf[offset++];
    result |= (b & 0x7f) << shift;
    shift += 7;
    if (!(b & 0x80)) break;
  }
  return { value: result, offset };
}

function parseMessage(buf) {
  const fields = {};
  let offset = 0;
  while (offset < buf.length) {
    const tag = readVarint(buf, offset);
    offset = tag.offset;
    if (offset >= buf.length && tag.value === 0) break;
    const fieldNum = tag.value >>> 3;
    const wireType = tag.value & 0x7;

    if (wireType === 0) {
      const v = readVarint(buf, offset);
      offset = v.offset;
      (fields[fieldNum] ??= []).push(v.value);
    } else if (wireType === 2) {
      const len = readVarint(buf, offset);
      offset = len.offset;
      const data = buf.slice(offset, offset + len.value);
      offset += len.value;
      (fields[fieldNum] ??= []).push(data);
    } else if (wireType === 1) {
      offset += 8;
    } else if (wireType === 5) {
      offset += 4;
    } else {
      break;
    }
  }
  return fields;
}

// ── Encoder (export) ──────────────────────────────────────────────────────

function fromBase32(str) {
  const cleaned = str.toUpperCase().replace(/[^A-Z2-7]/g, '');
  let bits = 0, value = 0;
  const out = [];
  for (const char of cleaned) {
    const idx = BASE32_CHARS.indexOf(char);
    if (idx === -1) continue;
    value = (value << 5) | idx;
    bits += 5;
    if (bits >= 8) { out.push((value >>> (bits - 8)) & 0xFF); bits -= 8; }
  }
  return new Uint8Array(out);
}

function encodeVarint(n) {
  const out = [];
  while (n > 127) { out.push((n & 0x7F) | 0x80); n >>>= 7; }
  out.push(n & 0x7F);
  return new Uint8Array(out);
}

function concat(...arrays) {
  const total = arrays.reduce((s, a) => s + a.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const a of arrays) { out.set(a, off); off += a.length; }
  return out;
}

const pb_ld  = (f, d) => concat(encodeVarint((f << 3) | 2), encodeVarint(d.length), d);
const pb_int = (f, v) => v === 0 ? new Uint8Array(0) : concat(encodeVarint((f << 3) | 0), encodeVarint(v));

function encodeOtpParams({ name, secret_b32 }) {
  const enc = new TextEncoder();
  return concat(
    pb_ld(1, fromBase32(secret_b32)),
    pb_ld(2, enc.encode(name)),
    pb_int(4, 1),  // algorithm = SHA1
    pb_int(5, 1),  // digits = SIX
    pb_int(6, 2),  // type = TOTP
  );
}

const EXPORT_BATCH = 10;

// Encodes accounts into Google Authenticator migration URI(s).
// Returns one URI per batch of up to EXPORT_BATCH accounts.
export function encodeMigrationQR(accounts) {
  const batchId = (Math.floor(Math.random() * 0xFFFFFF)) || 1;
  const total   = Math.ceil(accounts.length / EXPORT_BATCH);
  return Array.from({ length: total }, (_, b) => {
    const batch = accounts.slice(b * EXPORT_BATCH, (b + 1) * EXPORT_BATCH);
    const payload = concat(
      ...batch.map(a => pb_ld(1, encodeOtpParams(a))),
      pb_int(2, 1),
      pb_int(3, total),
      pb_int(4, b),
      pb_int(5, batchId),
    );
    const b64 = btoa(String.fromCharCode(...payload));
    return `otpauth-migration://offline?data=${encodeURIComponent(b64)}`;
  });
}

// ── Decoder (import) ──────────────────────────────────────────────────────

// Decodes a Google Authenticator "otpauth-migration://offline?data=..." QR string.
// Returns [{name, secret_b32}] for all TOTP entries found.
export function decodeMigrationQR(text) {
  const match = text.match(/[?&]data=([^&]+)/);
  if (!match) return [];

  let raw;
  try {
    raw = atob(decodeURIComponent(match[1]));
  } catch {
    return [];
  }

  const buf = Uint8Array.from(raw, c => c.charCodeAt(0));
  const payload = parseMessage(buf);

  const accounts = [];
  for (const paramBuf of (payload[1] ?? [])) {
    const param = parseMessage(paramBuf);
    const secretBytes = param[1]?.[0];
    const nameBytes   = param[2]?.[0];
    const issuerBytes = param[3]?.[0];
    const type        = param[6]?.[0] ?? 0; // 2 = TOTP

    if (!secretBytes || type !== 2) continue;

    const name = nameBytes
      ? new TextDecoder().decode(nameBytes)
      : issuerBytes
        ? new TextDecoder().decode(issuerBytes)
        : 'Unknown';

    accounts.push({ name, secret_b32: toBase32(secretBytes) });
  }
  return accounts;
}
