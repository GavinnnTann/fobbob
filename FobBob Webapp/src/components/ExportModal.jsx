import { useState, useEffect } from 'react';
import { ChevronLeft, ChevronRight, X } from 'lucide-react';
import { QRCodeSVG } from 'qrcode.react';
import { encodeMigrationQR } from '../lib/qr';
import Button from './ui/Button';

// Account export modal — selects accounts and renders Google Authenticator
// migration QR codes. Shared by the provisioning TOTP step and the factory
// reset safety flow.
export default function ExportModal({ accounts, onClose }) {
  const [checked, setChecked] = useState(() => accounts.map(() => true));
  const [page, setPage]       = useState(0);

  const selected   = accounts.filter((_, i) => checked[i]);
  const uris       = selected.length > 0 ? encodeMigrationQR(selected) : [];
  const currentPage = Math.min(page, Math.max(0, uris.length - 1));
  const allChecked  = checked.every(Boolean);

  useEffect(() => setPage(0), [checked]);

  const toggleAll = () => setChecked(checked.map(() => !allChecked));
  const toggle    = i  => setChecked(prev => prev.map((v, j) => j === i ? !v : v));

  return (
    <div className="fixed inset-0 bg-black/70 backdrop-blur-sm flex items-center justify-center z-[60] p-4">
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl w-full max-w-sm overflow-hidden">

        <div className="flex items-center justify-between px-5 py-4 border-b border-zinc-800">
          <h3 className="font-bold text-white">Export as QR</h3>
          <button onClick={onClose} className="text-zinc-500 hover:text-white transition-colors">
            <X size={18} />
          </button>
        </div>

        <div className="p-5 space-y-4">
          {/* Account selector */}
          <div>
            <button
              onClick={toggleAll}
              className="text-xs text-blue-400 hover:text-blue-300 transition-colors mb-2 block"
            >
              {allChecked ? 'Deselect all' : 'Select all'}
            </button>
            <div className="max-h-44 overflow-y-auto space-y-0.5 pr-1">
              {accounts.map((a, i) => (
                <label key={i} className="flex items-center gap-3 px-3 py-2 rounded-lg hover:bg-zinc-800 cursor-pointer transition-colors">
                  <input
                    type="checkbox"
                    checked={checked[i]}
                    onChange={() => toggle(i)}
                    className="accent-blue-500 w-4 h-4 shrink-0"
                  />
                  <div className="w-6 h-6 rounded bg-zinc-700 flex items-center justify-center text-xs font-bold text-zinc-300 shrink-0">
                    {a.name.charAt(0).toUpperCase()}
                  </div>
                  <span className="text-sm text-white truncate">{a.name}</span>
                </label>
              ))}
            </div>
          </div>

          {/* QR code */}
          {uris.length > 0 ? (
            <div className="flex flex-col items-center gap-3">
              <div className="bg-white p-3 rounded-xl">
                <QRCodeSVG value={uris[currentPage]} size={220} level="M" />
              </div>
              {uris.length > 1 && (
                <div className="flex items-center gap-4 text-sm text-zinc-400">
                  <button
                    onClick={() => setPage(p => Math.max(0, p - 1))}
                    disabled={currentPage === 0}
                    className="text-zinc-500 hover:text-white disabled:opacity-30 disabled:cursor-default transition-colors"
                  >
                    <ChevronLeft size={16} />
                  </button>
                  <span>QR {currentPage + 1} of {uris.length}</span>
                  <button
                    onClick={() => setPage(p => Math.min(uris.length - 1, p + 1))}
                    disabled={currentPage === uris.length - 1}
                    className="text-zinc-500 hover:text-white disabled:opacity-30 disabled:cursor-default transition-colors"
                  >
                    <ChevronRight size={16} />
                  </button>
                </div>
              )}
              <p className="text-zinc-600 text-xs text-center">
                Google Authenticator → ⋮ → Transfer accounts → Import accounts
              </p>
            </div>
          ) : (
            <div className="h-32 flex items-center justify-center">
              <p className="text-zinc-600 text-sm">No accounts selected</p>
            </div>
          )}
        </div>

        <div className="px-5 pb-5">
          <Button variant="secondary" onClick={onClose}>Done</Button>
        </div>
      </div>
    </div>
  );
}
