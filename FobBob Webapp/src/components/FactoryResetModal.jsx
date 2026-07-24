import { useState, useEffect } from 'react';
import { AlertTriangle, Download, X, CheckCircle2, Loader2 } from 'lucide-react';
import { factoryReset } from '../lib/ble';
import ExportModal from './ExportModal';
import Button from './ui/Button';
import Input from './ui/Input';

const CONFIRM_PHRASE = 'WIPE ALL';

// Factory reset safety flow: prompt the user to export their accounts (QR →
// Google Authenticator) before wiping, then require them to type "WIPE ALL"
// to confirm. On confirm the device erases NVS + the fingerprint sensor and
// reboots.
export default function FactoryResetModal({ chars, accounts, onClose }) {
  const [showExport, setShowExport] = useState(false);
  const [confirmText, setConfirmText] = useState('');
  const [phase, setPhase] = useState('confirm'); // 'confirm' | 'wiping' | 'done' | 'error'
  const [error, setError] = useState('');

  const matches = confirmText.trim().toUpperCase() === CONFIRM_PHRASE;

  // Once the wipe succeeds the device reboots into first-boot provisioning mode.
  // Reload the app so the user starts fresh at the connect step. Give the device
  // a couple of seconds to reboot and begin advertising before refreshing.
  useEffect(() => {
    if (phase !== 'done') return;
    const t = setTimeout(() => window.location.reload(), 3000);
    return () => clearTimeout(t);
  }, [phase]);

  const doReset = async () => {
    if (!matches) return;
    setPhase('wiping');
    setError('');
    try {
      await factoryReset(chars);
      setPhase('done');
    } catch (e) {
      setError(e?.message || 'Factory reset failed.');
      setPhase('error');
    }
  };

  return (
    <div className="fixed inset-0 bg-black/70 backdrop-blur-sm flex items-center justify-center z-50 p-4">
      {showExport && <ExportModal accounts={accounts} onClose={() => setShowExport(false)} />}

      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl w-full max-w-sm overflow-hidden">
        <div className="flex items-center justify-between px-5 py-4 border-b border-zinc-800">
          <h3 className="font-bold text-white flex items-center gap-2">
            <AlertTriangle size={18} className="text-red-400" /> Factory Reset
          </h3>
          {phase !== 'wiping' && (
            <button onClick={onClose} className="text-zinc-500 hover:text-white transition-colors">
              <X size={18} />
            </button>
          )}
        </div>

        {phase === 'done' ? (
          <div className="p-6 flex flex-col items-center text-center gap-3">
            <CheckCircle2 size={44} className="text-emerald-400" />
            <h4 className="text-white font-semibold">Device wiped</h4>
            <p className="text-zinc-500 text-sm">
              The device has been factory reset and is rebooting into setup mode.
              Refreshing so you can provision it again…
            </p>
            <Button onClick={() => window.location.reload()} className="mt-2">
              Refresh now
            </Button>
          </div>
        ) : phase === 'wiping' ? (
          <div className="p-6 flex flex-col items-center text-center gap-3">
            <Loader2 size={40} className="text-red-400 animate-spin" />
            <p className="text-zinc-400 text-sm">Wiping device…</p>
          </div>
        ) : (
          <div className="p-5 space-y-4">
            <div className="bg-red-950/40 border border-red-900/50 rounded-xl p-4">
              <p className="text-red-200 text-sm leading-relaxed">
                This permanently erases <strong>all accounts, fingerprints, and settings</strong> from
                the device. This cannot be undone.
              </p>
            </div>

            {/* Export safety net */}
            <div className="space-y-2">
              <p className="text-zinc-400 text-sm">
                Export your accounts to Google Authenticator first so you don't lose them.
              </p>
              <Button
                variant="secondary"
                onClick={() => setShowExport(true)}
                disabled={accounts.length === 0}
              >
                <Download size={16} />
                {accounts.length > 0
                  ? `Export ${accounts.length} account${accounts.length !== 1 ? 's' : ''}`
                  : 'No accounts to export'}
              </Button>
            </div>

            {/* Type-to-confirm */}
            <div className="space-y-2 pt-1">
              <Input
                label={`Type "${CONFIRM_PHRASE}" to confirm`}
                placeholder={CONFIRM_PHRASE}
                value={confirmText}
                onChange={e => setConfirmText(e.target.value)}
                autoCapitalize="characters"
                autoCorrect="off"
                spellCheck={false}
              />
            </div>

            {phase === 'error' && <p className="text-red-400 text-sm">{error}</p>}

            <div className="flex gap-3 pt-1">
              <Button variant="secondary" onClick={onClose}>Cancel</Button>
              <Button variant="danger" onClick={doReset} disabled={!matches}>
                Factory Reset
              </Button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
