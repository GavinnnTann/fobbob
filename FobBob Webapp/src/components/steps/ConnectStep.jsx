import { useState } from 'react';
import { AlertCircle, Clock, CheckCircle, Loader2 } from 'lucide-react';
import { connectDevice, readDeviceInfo, sendPin, syncTimeOnly } from '../../lib/ble';
import Button from '../ui/Button';
import Input from '../ui/Input';
import LottieIcon from '../ui/LottieIcon';
import bluetoothAnim from '../../assets/lottie/bluetooth.json';
import tickAnim from '../../assets/lottie/tick.json';

export default function ConnectStep({ onConnect }) {
  const [phase, setPhase]       = useState('idle');    // idle | connecting | pin | verifying | done | syncing | synced | error
  const [chars, setChars]       = useState(null);
  const [deviceInfo, setInfo]   = useState(null);
  const [pin, setPin]           = useState('');
  const [error, setError]       = useState('');

  const handleSyncTime = async () => {
    setPhase('syncing');
    setError('');
    try {
      await syncTimeOnly();
      setPhase('synced');
    } catch (e) {
      if (e.name !== 'NotFoundError') setError(e.message);
      setPhase('error');
    }
  };

  const handleConnect = async () => {
    setPhase('connecting');
    setError('');
    try {
      const { chars: c } = await connectDevice();
      const info = await readDeviceInfo(c);
      setChars(c);
      setInfo(info);
      setPhase('pin');
    } catch (e) {
      if (e.name === 'NotFoundError') {
        setError('No device selected. Make sure the device is powered on and in provisioning mode.');
      } else {
        setError(e.message);
      }
      setPhase('error');
    }
  };

  const handleVerifyPin = async () => {
    if (pin.length !== 6 || !/^\d{6}$/.test(pin)) {
      setError('Enter the 6-digit PIN shown in the device display.');
      return;
    }
    setPhase('verifying');
    setError('');
    try {
      await sendPin(chars, pin);
      setPhase('done');
      setTimeout(() => onConnect(chars, deviceInfo), 600);
    } catch (e) {
      setError('Failed to send PIN: ' + e.message);
      setPhase('pin');
    }
  };

  if (phase === 'idle' || phase === 'error') return (
    <div className="space-y-6">
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-8 flex flex-col items-center gap-4 text-center">
        <LottieIcon animationData={bluetoothAnim} size={80} loop={true} />
        <div>
          <h2 className="text-xl font-bold text-white">Connect Device</h2>
          <p className="text-zinc-500 text-sm mt-1">Make sure FobBob is in provisioning mode</p>
        </div>

        {deviceInfo?.provisioned && (
          <div className="bg-amber-500/10 border border-amber-500/20 rounded-xl px-4 py-3 text-amber-400 text-sm text-left w-full">
            ⚠️ This device is already provisioned. Continuing will overwrite its settings.
          </div>
        )}

        {error && (
          <div className="flex items-start gap-2 text-red-400 text-sm text-left w-full">
            <AlertCircle size={16} className="mt-0.5 shrink-0" />
            <span>{error}</span>
          </div>
        )}

        <Button onClick={handleConnect}>
          Connect to FobBob
        </Button>

        <div className="w-full border-t border-zinc-800 pt-4 space-y-2">
          <button
            onClick={handleSyncTime}
            className="w-full flex items-center justify-center gap-2 text-zinc-400 hover:text-white text-sm py-1.5 transition-colors"
          >
            <Clock size={14} /> Sync time only
          </button>
          <p className="text-zinc-600 text-xs text-center">
            No PIN needed — device must be on and within 30 s of boot
          </p>
        </div>

        <p className="text-zinc-600 text-xs">Requires Chrome or Edge — Web Bluetooth is not supported in Safari or Firefox.</p>
      </div>
    </div>
  );

  if (phase === 'connecting') return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-8 flex flex-col items-center gap-4 text-center">
      <LottieIcon animationData={bluetoothAnim} size={80} loop={true} />
      <div>
        <h2 className="text-xl font-bold text-white">Connecting…</h2>
        <p className="text-zinc-500 text-sm mt-1">Select FobBob in the browser popup</p>
      </div>
    </div>
  );

  if (phase === 'pin' || phase === 'verifying') return (
    <div className="space-y-4">
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-1">
        <div className="flex items-center gap-2 text-emerald-400 text-sm font-medium">
          <CheckCircle size={16} />
          Connected to FobBob
        </div>
        {deviceInfo && (
          <p className="text-zinc-500 text-xs pl-6">
            {deviceInfo.provisioned
              ? `Provisioned — ${deviceInfo.totp_count} accounts, ${deviceInfo.fp_count} fingerprints`
              : 'Not yet provisioned'}
          </p>
        )}
      </div>

      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-4">
        <div>
          <h2 className="text-xl font-bold text-white">Enter PIN</h2>
          <p className="text-zinc-500 text-sm mt-1">
            Check your device display for the 6-digit PIN (shown after <code className="text-zinc-400">[BLE] PIN:</code>)
          </p>
        </div>

        <Input
          label="6-digit PIN"
          type="number"
          inputMode="numeric"
          placeholder="123456"
          maxLength={6}
          value={pin}
          onChange={e => setPin(e.target.value.slice(0, 6))}
          onKeyDown={e => e.key === 'Enter' && handleVerifyPin()}
          error={error}
          autoFocus
        />

        <Button onClick={handleVerifyPin} disabled={phase === 'verifying'}>
          {phase === 'verifying' ? <Loader2 size={16} className="animate-spin" /> : null}
          {phase === 'verifying' ? 'Verifying…' : 'Verify PIN'}
        </Button>
      </div>
    </div>
  );

  if (phase === 'syncing') return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-8 flex flex-col items-center gap-4 text-center">
      <LottieIcon animationData={bluetoothAnim} size={80} loop={true} />
      <div>
        <h2 className="text-xl font-bold text-white">Syncing Time…</h2>
        <p className="text-zinc-500 text-sm mt-1">Connecting to FobBob</p>
      </div>
    </div>
  );

  if (phase === 'synced') return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-8 flex flex-col items-center gap-4 text-center">
      <LottieIcon animationData={tickAnim} size={80} loop={false} />
      <div>
        <h2 className="text-xl font-bold text-white">Time Synced</h2>
        <p className="text-zinc-500 text-sm mt-1">Device clock is now up to date</p>
      </div>
      <Button onClick={() => setPhase('idle')}>Done</Button>
    </div>
  );

  if (phase === 'done') return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-8 flex flex-col items-center gap-3 text-center">
      <LottieIcon animationData={tickAnim} size={80} loop={false} />
      <h2 className="text-xl font-bold text-white">PIN Verified</h2>
      <p className="text-zinc-500 text-sm">Moving to next step…</p>
    </div>
  );
}
