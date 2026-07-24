import { useState } from 'react';
import { Wifi, Eye, EyeOff, ChevronLeft } from 'lucide-react';
import Button from '../ui/Button';
import Input from '../ui/Input';

export default function WifiStep({ wifi, onChange, onNext, onSkip, onBack }) {
  const [showPass, setShowPass] = useState(false);
  const [errors, setErrors]     = useState({});

  const validate = () => {
    const e = {};
    // SSID is required to save; password is optional (open networks).
    if (!wifi.ssid.trim()) e.ssid = 'Network name is required';
    setErrors(e);
    return Object.keys(e).length === 0;
  };

  const handleNext = () => {
    if (validate()) onNext();
  };

  return (
    <div className="space-y-4">
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-5">
        <div className="flex items-center gap-3">
          <div className="w-10 h-10 rounded-xl bg-blue-600/15 flex items-center justify-center">
            <Wifi size={20} className="text-blue-400" />
          </div>
          <div>
            <h2 className="text-xl font-bold text-white">WiFi Network <span className="text-zinc-500 font-normal text-sm">(optional)</span></h2>
            <p className="text-zinc-500 text-sm">Enables automatic time sync. You can skip and add it later in device settings.</p>
          </div>
        </div>

        <Input
          label="Network Name (SSID)"
          type="text"
          placeholder="My Network"
          value={wifi.ssid}
          onChange={e => onChange(w => ({ ...w, ssid: e.target.value }))}
          error={errors.ssid}
          autoFocus
          autoCapitalize="none"
          autoCorrect="off"
        />

        <div className="space-y-1.5">
          <label className="block text-sm font-medium text-zinc-400">Password</label>
          <div className="relative">
            <input
              type={showPass ? 'text' : 'password'}
              placeholder="••••••••"
              value={wifi.password}
              onChange={e => onChange(w => ({ ...w, password: e.target.value }))}
              onKeyDown={e => e.key === 'Enter' && handleNext()}
              autoComplete="current-password"
              className={`w-full bg-zinc-800 border ${errors.password ? 'border-red-500' : 'border-zinc-700'} rounded-xl px-4 py-3 pr-12 text-white placeholder-zinc-500 focus:outline-none focus:border-blue-500 transition-colors text-[15px]`}
            />
            <button
              type="button"
              onClick={() => setShowPass(s => !s)}
              className="absolute right-3 top-1/2 -translate-y-1/2 text-zinc-500 hover:text-zinc-300 p-1"
            >
              {showPass ? <EyeOff size={18} /> : <Eye size={18} />}
            </button>
          </div>
          {errors.password && <p className="text-sm text-red-400">{errors.password}</p>}
        </div>
      </div>

      <div className="flex gap-3">
        <Button variant="secondary" onClick={onBack} className="w-auto px-5">
          <ChevronLeft size={18} />
        </Button>
        <Button onClick={handleNext}>
          Save &amp; Continue
        </Button>
      </div>
      <button
        type="button"
        onClick={onSkip}
        className="w-full text-center text-sm text-zinc-500 hover:text-zinc-300 py-2 transition-colors"
      >
        Skip for now
      </button>
    </div>
  );
}
