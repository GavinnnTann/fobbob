import { useState } from 'react';
import fobBobIcon from './assets/fobbob-icon.png';
import { isSupported, readTotpAccounts, readDisplaySec, readImuSettings, IMU_DEFAULTS } from './lib/ble';
import StepBar from './components/StepBar';
import ConnectStep      from './components/steps/ConnectStep';
import NameStep         from './components/steps/NameStep';
import WifiStep         from './components/steps/WifiStep';
import SettingsStep     from './components/steps/SettingsStep';
import FingerprintStep  from './components/steps/FingerprintStep';
import TotpStep         from './components/steps/TotpStep';
import ConfirmStep      from './components/steps/ConfirmStep';

export default function App() {
  const [step, setStep]               = useState(0);
  const [chars, setChars]             = useState(null);
  const [deviceInfo, setDeviceInfo]   = useState(null);
  const [deviceName, setDeviceName]   = useState('');
  const [displaySec, setDisplaySec]   = useState(30);
  const [imuSettings, setImuSettings] = useState({ ...IMU_DEFAULTS });
  const [accounts, setAccounts]       = useState([]);
  const [fingerprints, setFingerprints] = useState([]);
  const [wifi, setWifi]               = useState({ ssid: '', password: '' });

  const next = () => setStep(s => s + 1);
  const back = () => setStep(s => s - 1);

  const handleConnect = async (c, info) => {
    setChars(c);
    setDeviceInfo(info);
    setDeviceName(info?.device_name || '');
    const [currentSec, currentImu, existing] = await Promise.all([
      readDisplaySec(c),
      readImuSettings(c),
      readTotpAccounts(c),
    ]);
    setDisplaySec(currentSec);
    setImuSettings(currentImu);
    setAccounts(existing);
    setFingerprints((info?.fp_names || []).map((f, i) => ({
      slot_id: f.slot_id ?? i,
      name: f.name || `Finger ${i + 1}`,
    })));
    next();
  };

  if (!isSupported()) return (
    <div className="min-h-screen bg-black flex items-center justify-center px-6 py-10">
      <div className="max-w-sm space-y-5">
        <div className="text-center space-y-3">
          <div className="text-5xl">⚠️</div>
          <h1 className="text-xl font-bold text-white">Bluetooth setup isn't available</h1>
          <p className="text-zinc-400 text-sm leading-relaxed">
            Web Bluetooth requires <strong className="text-white">Chrome or Edge</strong> on desktop or Android.
            This usually means you're on iOS or a browser without Web Bluetooth.
          </p>
        </div>

        <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-5 space-y-3">
          <p className="text-white font-semibold text-sm">Set up over WiFi instead</p>
          <ol className="text-zinc-400 text-sm leading-relaxed space-y-2 list-decimal list-inside">
            <li>Put your FobBob in setup mode (it shows a <code className="text-blue-400">FobBob-Setup-</code> network).</li>
            <li>On your phone, join that WiFi network (no password).</li>
            <li>A setup page should open automatically — if not, visit{' '}
              <code className="text-blue-400">192.168.4.1</code> in your browser.</li>
          </ol>
        </div>
      </div>
    </div>
  );

  return (
    <div className="h-screen bg-black overflow-hidden">
      <div className="max-w-md mx-auto px-4 pt-6 pb-4 h-full flex flex-col gap-4">
        {/* Header */}
        <div className="flex items-center gap-3">
          <img src={fobBobIcon} alt="FobBob icon" className="w-14 object-contain" />
          <div>
            <h1 className="text-5xl font-bold text-white tracking-tight">
              Fob<span className="text-blue-500">Bob</span>
            </h1>
            <p className="text-xs tracking-widest font-medium">
              <span className="text-zinc-400">SECURE. SIMPLE. </span><span className="text-blue-500">YOURS.</span>
            </p>
          </div>
        </div>

        <StepBar current={step} />

        <div key={step} className="animate-fade-in flex-1 overflow-y-auto">
          {step === 0 && (
            <ConnectStep onConnect={handleConnect} />
          )}
          {step === 1 && (
            <NameStep
              chars={chars}
              accounts={accounts}
              deviceInfo={deviceInfo}
              deviceName={deviceName}
              onChange={setDeviceName}
              onNext={next}
              onBack={back}
            />
          )}
          {step === 2 && (
            <WifiStep
              wifi={wifi}
              onChange={setWifi}
              onNext={next}
              onSkip={() => { setWifi({ ssid: '', password: '' }); next(); }}
              onBack={back}
            />
          )}
          {step === 3 && (
            <SettingsStep
              displaySec={displaySec}
              onDisplaySecChange={setDisplaySec}
              imuSettings={imuSettings}
              onImuChange={setImuSettings}
              onNext={next}
              onBack={back}
            />
          )}
          {step === 4 && (
            <FingerprintStep
              chars={chars}
              deviceInfo={deviceInfo}
              fingerprints={fingerprints}
              onChange={setFingerprints}
              onNext={next}
              onBack={back}
            />
          )}
          {step === 5 && (
            <TotpStep accounts={accounts} onChange={setAccounts} onNext={next} onBack={back} />
          )}
          {step === 6 && (
            <ConfirmStep
              chars={chars}
              accounts={accounts}
              fingerprints={fingerprints}
              deviceName={deviceName}
              displaySec={displaySec}
              imuSettings={imuSettings}
              wifi={wifi}
              deviceInfo={deviceInfo}
              onBack={back}
            />
          )}
        </div>
      </div>
    </div>
  );
}
