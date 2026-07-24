import { Timer, ChevronLeft, Hand, Activity, RotateCcw } from 'lucide-react';
import Button from '../ui/Button';

function formatSec(s) {
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  const r = s % 60;
  return r === 0 ? `${m} min` : `${m}m ${r}s`;
}

function Toggle({ checked, onChange }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      onClick={() => onChange(!checked)}
      className={`relative inline-flex h-6 w-11 shrink-0 cursor-pointer rounded-full border-2 border-transparent
        transition-colors duration-200 focus:outline-none
        ${checked ? 'bg-blue-600' : 'bg-zinc-700'}`}
    >
      <span
        className={`pointer-events-none inline-block h-5 w-5 transform rounded-full bg-white shadow
          transition-transform duration-200
          ${checked ? 'translate-x-5' : 'translate-x-0'}`}
      />
    </button>
  );
}

function ToggleRow({ icon: Icon, label, description, checked, onChange }) {
  return (
    <div className="flex items-center gap-4 py-3">
      <div className="w-9 h-9 rounded-xl bg-zinc-800 flex items-center justify-center shrink-0">
        <Icon size={17} className="text-zinc-400" />
      </div>
      <div className="flex-1 min-w-0">
        <p className="text-sm font-medium text-white">{label}</p>
        <p className="text-xs text-zinc-500 mt-0.5">{description}</p>
      </div>
      <Toggle checked={checked} onChange={onChange} />
    </div>
  );
}

export default function SettingsStep({
  displaySec, onDisplaySecChange,
  imuSettings, onImuChange,
  onNext, onBack,
}) {
  const set = (key) => (val) => onImuChange({ ...imuSettings, [key]: val });

  return (
    <div className="space-y-4">
      {/* Screen timeout */}
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-4">
        <div className="flex items-center gap-3">
          <div className="w-10 h-10 rounded-xl bg-blue-600/15 flex items-center justify-center">
            <Timer size={20} className="text-blue-400" />
          </div>
          <div>
            <h2 className="text-xl font-bold text-white">Screen Timeout</h2>
            <p className="text-zinc-500 text-sm">How long to show codes before sleeping</p>
          </div>
        </div>

        <div className="space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-zinc-400 text-sm">Duration</span>
            <span className="text-white font-mono text-sm font-semibold">{formatSec(displaySec)}</span>
          </div>
          <input
            type="range"
            min={5}
            max={300}
            step={5}
            value={displaySec}
            onChange={e => onDisplaySecChange(Number(e.target.value))}
            className="w-full h-2 rounded-full appearance-none cursor-pointer accent-blue-500 bg-zinc-700"
          />
          <div className="flex justify-between text-zinc-600 text-xs">
            <span>5s</span>
            <span>5 min</span>
          </div>
        </div>
      </div>

      {/* Motion & display */}
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl px-6 py-2 divide-y divide-zinc-800">
        <div className="pb-3 pt-4">
          <h2 className="text-base font-semibold text-white">Motion & Display</h2>
          <p className="text-zinc-500 text-xs mt-0.5">Uses the onboard QMI8658 sensor</p>
        </div>

        <ToggleRow
          icon={Hand}
          label="Double-tap to sleep"
          description="Double-tap the screen to put the device to sleep"
          checked={imuSettings.tap_sleep}
          onChange={set('tap_sleep')}
        />
        <ToggleRow
          icon={Activity}
          label="Adaptive screen timeout"
          description="Resets the sleep timer while you're actively holding the device"
          checked={imuSettings.adaptive_timeout}
          onChange={set('adaptive_timeout')}
        />
        <ToggleRow
          icon={RotateCcw}
          label="Auto-rotate display"
          description="Flips the UI and swaps buttons when held upside down"
          checked={imuSettings.orient_flip}
          onChange={set('orient_flip')}
        />
      </div>

      <div className="flex gap-3">
        <Button variant="secondary" onClick={onBack} className="w-auto px-5">
          <ChevronLeft size={18} />
        </Button>
        <Button onClick={onNext}>Continue</Button>
      </div>
    </div>
  );
}
