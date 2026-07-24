import { useState } from 'react';
import { Rocket, ShieldCheck, Clock, Timer, CheckCircle, Loader2, AlertCircle, ChevronLeft, Tag, Fingerprint, Zap } from 'lucide-react';
import { sendTotpSecrets, sendNtpTime, sendProvisionDone, sendDeviceName, sendFpNames, sendDisplaySec, sendImuSettings, sendWifi } from '../../lib/ble';
import Button from '../ui/Button';
import LottieIcon from '../ui/LottieIcon';
import tickAnim from '../../assets/lottie/tick.json';

const STEPS = [
  { key: 'fp',      label: 'Sending fingerprint names' },
  { key: 'name',    label: 'Setting device name' },
  { key: 'timeout', label: 'Setting screen timeout' },
  { key: 'imu',     label: 'Applying motion settings' },
  { key: 'wifi',    label: 'Saving WiFi network' },
  { key: 'totp',    label: 'Sending TOTP secrets' },
  { key: 'ntp',     label: 'Syncing time' },
  { key: 'done',    label: 'Finalising' },
];

function formatSec(s) {
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  const r = s % 60;
  return r === 0 ? `${m} min` : `${m}m ${r}s`;
}

function SummaryRow({ icon: Icon, label, value, dim }) {
  return (
    <div className="flex items-center gap-3 py-3">
      <div className="w-8 h-8 rounded-lg bg-zinc-800 flex items-center justify-center">
        <Icon size={16} className="text-zinc-400" />
      </div>
      <div className="flex-1">
        <p className="text-xs text-zinc-500">{label}</p>
        <p className={`text-sm font-medium ${dim ? 'text-zinc-500' : 'text-white'}`}>{value}</p>
      </div>
    </div>
  );
}

export default function ConfirmStep({ chars, accounts, fingerprints, deviceName, displaySec, imuSettings, wifi, deviceInfo, onBack }) {
  const [phase, setPhase]         = useState('idle');    // idle | provisioning | success | error
  const [stepIndex, setStepIndex] = useState(-1);
  const [error, setError]         = useState('');

  const provision = async () => {
    setPhase('provisioning');
    setError('');
    try {
      // Fingerprint names (always send to sync names/count)
      setStepIndex(0);
      await sendFpNames(chars, fingerprints);

      // Device name (skip if blank)
      setStepIndex(1);
      if (deviceName.trim()) {
        await sendDeviceName(chars, deviceName.trim());
      }

      // Screen timeout
      setStepIndex(2);
      await sendDisplaySec(chars, displaySec);

      // IMU / motion settings
      setStepIndex(3);
      await sendImuSettings(chars, imuSettings);

      // WiFi (skip if no SSID — keeps any existing credentials untouched)
      setStepIndex(4);
      if (wifi?.ssid?.trim() && chars.WIFI_CREDS) {
        await sendWifi(chars, wifi.ssid.trim(), wifi.password || '');
      }

      // TOTP (skip if none)
      setStepIndex(5);
      if (accounts.length > 0) {
        await sendTotpSecrets(chars, accounts);
      }

      // NTP time
      setStepIndex(6);
      await sendNtpTime(chars);

      // Provision done
      setStepIndex(7);
      await sendProvisionDone(chars);

      setPhase('success');
    } catch (e) {
      setError(e.message);
      setPhase('error');
    }
  };

  if (phase === 'success') return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-10 flex flex-col items-center gap-4 text-center">
      <LottieIcon animationData={tickAnim} size={100} loop={false} />
      <div>
        <h2 className="text-2xl font-bold text-white">All Done!</h2>
        <p className="text-zinc-400 text-sm mt-2">
          FobBob is provisioned. The device will restart automatically — touch the fingerprint sensor to wake it.
        </p>
      </div>
      <p className="text-zinc-600 text-xs">You can close this page.</p>
    </div>
  );

  if (phase === 'provisioning') return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-5">
      <div className="text-center">
        <h2 className="text-xl font-bold text-white">Provisioning…</h2>
        <p className="text-zinc-500 text-sm mt-1">Keep this page open</p>
      </div>

      <div className="space-y-2">
        {STEPS.map((s, i) => {
          const done    = i < stepIndex;
          const active  = i === stepIndex;
          return (
            <div key={s.key} className={`flex items-center gap-3 p-3 rounded-xl transition-colors
              ${active ? 'bg-zinc-800' : ''}`}>
              <div className="w-6 h-6 flex items-center justify-center shrink-0">
                {done
                  ? <CheckCircle size={18} className="text-emerald-400" />
                  : active
                    ? <Loader2 size={18} className="text-blue-400 animate-spin" />
                    : <div className="w-4 h-4 rounded-full border border-zinc-700" />}
              </div>
              <span className={`text-sm ${done ? 'text-zinc-400' : active ? 'text-white font-medium' : 'text-zinc-600'}`}>
                {s.label}
              </span>
            </div>
          );
        })}
      </div>
    </div>
  );

  return (
    <div className="space-y-4">
      {/* Summary card */}
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl px-5 divide-y divide-zinc-800">
        <SummaryRow icon={Tag} label="Device name"
          value={deviceName.trim() || 'Not set'}
          dim={!deviceName.trim()} />
        <SummaryRow icon={Fingerprint} label="Fingerprints"
          value={
            fingerprints.length > 0
              ? `${fingerprints.length} finger${fingerprints.length !== 1 ? 's' : ''}`
              : deviceInfo?.debug_mode ? 'Skipped (debug mode)' : 'None'
          }
          dim={fingerprints.length === 0} />
        <SummaryRow icon={ShieldCheck} label="Accounts"
          value={accounts.length > 0 ? `${accounts.length} account${accounts.length !== 1 ? 's' : ''}` : 'None (skipped)'}
          dim={accounts.length === 0} />
        <SummaryRow icon={Timer} label="Screen timeout" value={formatSec(displaySec ?? 30)} />
        {(() => {
          const n = imuSettings ? Object.values(imuSettings).filter(Boolean).length : 0;
          return (
            <SummaryRow icon={Zap} label="Motion features"
              value={n > 0 ? `${n} feature${n !== 1 ? 's' : ''} enabled` : 'All off'}
              dim={n === 0} />
          );
        })()}
        <SummaryRow icon={Clock} label="Time" value="Browser time (sent on confirm)" />
      </div>

      {phase === 'error' && (
        <div className="bg-zinc-900 border border-red-500/30 rounded-2xl p-4 flex items-start gap-3">
          <AlertCircle size={18} className="text-red-400 mt-0.5 shrink-0" />
          <div>
            <p className="text-red-400 font-medium text-sm">Provisioning failed</p>
            <p className="text-zinc-500 text-xs mt-1">{error}</p>
          </div>
        </div>
      )}

      <div className="bg-zinc-800/50 rounded-2xl p-4">
        <p className="text-zinc-500 text-xs leading-relaxed">
          If the PIN was wrong, the device will silently ignore the data and won't commit. Re-flash or hold both buttons to re-enter provisioning mode.
        </p>
      </div>

      <div className="flex gap-3">
        <Button variant="secondary" onClick={onBack} className="w-auto px-5">
          <ChevronLeft size={18} />
        </Button>
        <Button onClick={provision}>
          <Rocket size={16} />
          {phase === 'error' ? 'Retry Provisioning' : 'Provision Device'}
        </Button>
      </div>
    </div>
  );
}
