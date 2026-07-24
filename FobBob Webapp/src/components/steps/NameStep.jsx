import { useState } from 'react';
import { Tag, ChevronLeft, AlertTriangle } from 'lucide-react';
import Button from '../ui/Button';
import Input from '../ui/Input';
import FactoryResetModal from '../FactoryResetModal';

export default function NameStep({ chars, accounts = [], deviceInfo, deviceName, onChange, onNext, onBack }) {
  const macSuffix = deviceInfo?.mac_suffix || '????';
  const bleId     = `FobBob-${macSuffix}`;
  const [showReset, setShowReset] = useState(false);

  return (
    <div className="space-y-4">
      {showReset && (
        <FactoryResetModal
          chars={chars}
          accounts={accounts}
          onClose={() => setShowReset(false)}
        />
      )}
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-4">
        <div className="flex items-center gap-3">
          <div className="w-10 h-10 rounded-xl bg-blue-600/15 flex items-center justify-center">
            <Tag size={20} className="text-blue-400" />
          </div>
          <div>
            <h2 className="text-xl font-bold text-white">Name Your Device</h2>
            <p className="text-zinc-500 text-sm">Shown on the display after unlock</p>
          </div>
        </div>

        <div className="bg-zinc-800/60 rounded-xl px-4 py-3 flex items-center justify-between">
          <span className="text-zinc-500 text-xs">Hardware ID</span>
          <span className="text-white font-mono text-sm font-medium">{bleId}</span>
        </div>

        <Input
          label="Display name"
          placeholder="e.g. Gavin's FobBob"
          value={deviceName}
          onChange={e => onChange(e.target.value)}
          autoFocus
          onKeyDown={e => e.key === 'Enter' && onNext()}
        />
        <p className="text-zinc-600 text-xs">
          The hardware ID is permanent and tied to this device's MAC address. The display name is yours to choose.
        </p>
      </div>

      <div className="flex gap-3">
        <Button variant="secondary" onClick={onBack} className="w-auto px-5">
          <ChevronLeft size={18} />
        </Button>
        <Button onClick={onNext}>
          {deviceName.trim() ? 'Continue' : 'Skip'}
        </Button>
      </div>

      {/* Danger zone */}
      <div className="pt-2 border-t border-zinc-800/80">
        <button
          onClick={() => setShowReset(true)}
          className="w-full flex items-center justify-center gap-2 text-red-400 hover:text-red-300 text-sm font-medium py-2.5 transition-colors"
        >
          <AlertTriangle size={15} /> Factory reset device
        </button>
      </div>
    </div>
  );
}
