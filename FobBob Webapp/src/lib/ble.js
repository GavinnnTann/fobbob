const BLE_DEVICE_NAME = 'FobBob';
const SERVICE_UUID    = '7f4e9d2c-1b3a-4c8d-9e0f-2a3b4c5d6e7f';

const CHAR_UUIDS = {
  PIN_VERIFY:   '12345678-1234-1234-1234-123456789001',
  DEVICE_INFO:  '12345678-1234-1234-1234-123456789002',
  WIFI_CREDS:   '12345678-1234-1234-1234-123456789003',
  TOTP_SEC:     '12345678-1234-1234-1234-123456789004',
  NTP_TIME:     '12345678-1234-1234-1234-123456789005',
  FP_CMD:       '12345678-1234-1234-1234-123456789006',
  FP_STATUS:    '12345678-1234-1234-1234-123456789007',
  PROV_DONE:    '12345678-1234-1234-1234-123456789008',
  TOTP_READ:    '12345678-1234-1234-1234-123456789009',
  DEV_NAME:     '12345678-1234-1234-1234-12345678900a',
  FP_NAMES:     '12345678-1234-1234-1234-12345678900b',
  DISP_TIMEOUT: '12345678-1234-1234-1234-12345678900c',
  FACTORY_RESET:'12345678-1234-1234-1234-12345678900d',
  IMU_SETTINGS: '12345678-1234-1234-1234-12345678900e',
};

const enc = new TextEncoder();
const dec = new TextDecoder();

// The device requires an encrypted (paired) link on every characteristic.
// Chrome starts OS pairing automatically when a GATT op hits the ATT
// "insufficient encryption" error, but the op that triggered it can reject
// while the pairing dialog is still open — so retry the first operation a
// few times with a pause to let pairing complete.
async function withPairingRetry(fn, attempts = 3, delayMs = 1500) {
  let lastErr;
  for (let i = 0; i < attempts; i++) {
    try {
      return await fn();
    } catch (e) {
      lastErr = e;
      if (i < attempts - 1) await new Promise(r => setTimeout(r, delayMs));
    }
  }
  throw lastErr;
}

function write(char, value) {
  const encoded = typeof value === 'string' ? enc.encode(value) : value;
  return char.writeValueWithResponse(encoded);
}

// With bonding enabled the OS may already hold a link to the device
// (Windows auto-reconnects to bonded peripherals when they advertise), and a
// fresh connect attempt then rejects with "Connection already in progress".
// Reuse the existing link when there is one; otherwise retry briefly.
async function gattConnect(device) {
  if (device.gatt.connected) return device.gatt;
  return withPairingRetry(() => device.gatt.connect(), 3, 1000);
}

export async function connectDevice() {
  const device = await navigator.bluetooth.requestDevice({
    filters: [{ namePrefix: 'FobBob' }],
    optionalServices: [SERVICE_UUID],
  });

  const server = await gattConnect(device);
  const service = await server.getPrimaryService(SERVICE_UUID);

  // Discover all characteristics; set to null for any the device doesn't expose
  // so callers can guard with `if (chars.XYZ)` rather than crashing Chrome's
  // BLE stack with an unhandled DOMException from getCharacteristic().
  const chars = {};
  for (const [key, uuid] of Object.entries(CHAR_UUIDS)) {
    try {
      chars[key] = await service.getCharacteristic(uuid);
    } catch {
      chars[key] = null;
    }
  }

  return { device, server, chars };
}

export async function readDeviceInfo(chars) {
  // Usually the first encrypted op after connect — may trigger OS pairing.
  const val = await withPairingRetry(() => chars.DEVICE_INFO.readValue());
  return JSON.parse(dec.decode(val));
}

export async function sendPin(chars, pin) {
  await withPairingRetry(() => write(chars.PIN_VERIFY, pin.toString().padStart(6, '0')));
  const info = await chars.DEVICE_INFO.readValue();
  const data = JSON.parse(dec.decode(info));
  if (!data.pin_verified) throw new Error('Incorrect PIN');
}

export async function sendWifi(chars, ssid, password) {
  await write(chars.WIFI_CREDS, JSON.stringify({ ssid, password }));
}

export async function sendTotpSecrets(chars, accounts) {
  const payload = JSON.stringify(accounts);
  if (payload.length > 2000) {
    throw new Error(`Too many accounts — payload is ${payload.length} bytes (max ~2000). Remove some accounts.`);
  }
  await write(chars.TOTP_SEC, payload);
}

export async function sendNtpTime(chars) {
  await write(chars.NTP_TIME, Date.now().toString());
}

export async function sendProvisionDone(chars) {
  await write(chars.PROV_DONE, JSON.stringify({ confirm: true }));
}

// Read current accounts stored on device (requires PIN verification first).
// Returns [] if not provisioned or on read error.
export async function readTotpAccounts(chars) {
  try {
    const val = await chars.TOTP_READ.readValue();
    return JSON.parse(dec.decode(val)) || [];
  } catch {
    return [];
  }
}

export async function sendDeviceName(chars, name) {
  await write(chars.DEV_NAME, name);
}

export async function sendFpCommand(chars, cmdObj) {
  if (!chars.FP_CMD) throw new Error('FP_CMD characteristic not available on this device.');
  await write(chars.FP_CMD, JSON.stringify(cmdObj));
}

export async function sendFpNames(chars, fingerprints) {
  if (!chars.FP_NAMES || fingerprints.length === 0) return;
  // Include slot_id so firmware knows which sensor pages are valid and can purge orphans
  const payload = JSON.stringify(fingerprints.map(f => ({ name: f.name, slot_id: f.slot_id })));
  await write(chars.FP_NAMES, payload);
}

export async function readDisplaySec(chars) {
  try {
    const val = await chars.DISP_TIMEOUT.readValue();
    const n = parseInt(dec.decode(val), 10);
    return Number.isFinite(n) ? Math.min(300, Math.max(5, n)) : 30;
  } catch {
    return 30;
  }
}

export async function sendDisplaySec(chars, sec) {
  if (!chars.DISP_TIMEOUT) return;
  const clamped = Math.min(300, Math.max(5, Math.round(sec)));
  await write(chars.DISP_TIMEOUT, clamped.toString());
}

export const IMU_DEFAULTS = {
  tap_sleep: false,
  adaptive_timeout: false,
  orient_flip: false,
};

export async function readImuSettings(chars) {
  try {
    if (!chars.IMU_SETTINGS) return { ...IMU_DEFAULTS };
    const val = await chars.IMU_SETTINGS.readValue();
    const parsed = JSON.parse(dec.decode(val));
    return { ...IMU_DEFAULTS, ...parsed };
  } catch {
    return { ...IMU_DEFAULTS };
  }
}

export async function sendImuSettings(chars, settings) {
  if (!chars.IMU_SETTINGS) return;
  await write(chars.IMU_SETTINGS, JSON.stringify(settings));
}

// Full factory reset (PIN must already be verified this session). The device
// wipes NVS + fingerprint sensor and immediately reboots, which drops the BLE
// link — so a disconnect/network error right after the write is expected and
// treated as success.
export async function factoryReset(chars) {
  if (!chars.FACTORY_RESET) throw new Error('Factory reset not supported by this device firmware.');
  try {
    await write(chars.FACTORY_RESET, JSON.stringify({ confirm: 'WIPE ALL' }));
  } catch (e) {
    // The device reboots mid-write; GATT may reject with a disconnect error.
    if (/disconnect|gatt|network/i.test(e?.message || '')) return;
    throw e;
  }
}

export async function syncTimeOnly() {
  const device = await navigator.bluetooth.requestDevice({
    filters: [{ namePrefix: BLE_DEVICE_NAME }],
    optionalServices: [SERVICE_UUID],
  });
  const server  = await gattConnect(device);
  const service = await server.getPrimaryService(SERVICE_UUID);
  const ntpChar = await service.getCharacteristic(CHAR_UUIDS.NTP_TIME);
  // First op on the timesync link — may trigger OS pairing on a new machine.
  // Re-read the clock inside the retry so a pairing pause never sends stale time.
  await withPairingRetry(() => write(ntpChar, Date.now().toString()));
  device.gatt.disconnect();
}

export function isSupported() {
  return typeof navigator !== 'undefined' && !!navigator.bluetooth;
}
