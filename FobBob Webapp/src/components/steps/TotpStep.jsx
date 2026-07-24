import { useState, useRef, useEffect, useCallback } from 'react';
import { ShieldCheck, Camera, CameraOff, Plus, Trash2, ChevronLeft, ChevronUp, ChevronDown, Download } from 'lucide-react';
import jsQR from 'jsqr';
import { decodeMigrationQR } from '../../lib/qr';
import ExportModal from '../ExportModal';
import Button from '../ui/Button';
import Input from '../ui/Input';

export default function TotpStep({ accounts, onChange, onNext, onBack }) {
  const [scanning, setScanning]     = useState(false);
  const [camError, setCamError]     = useState('');
  const [scanStatus, setScanStatus] = useState('');
  const [showManual, setShowManual]   = useState(false);
  const [manual, setManual]           = useState({ name: '', secret: '' });
  const [manualErr, setManualErr]     = useState({});
  const [showExport, setShowExport]   = useState(false);

  // Single video + canvas — always mounted, never conditionally rendered
  const videoRef  = useRef(null);
  const canvasRef = useRef(null);
  const streamRef = useRef(null);
  const animRef   = useRef(null);

  const stopCamera = useCallback(() => {
    if (animRef.current)  { cancelAnimationFrame(animRef.current); animRef.current = null; }
    if (streamRef.current){ streamRef.current.getTracks().forEach(t => t.stop()); streamRef.current = null; }
    if (videoRef.current)   videoRef.current.srcObject = null;
    setScanning(false);
    setScanStatus('');
  }, []);

  useEffect(() => () => stopCamera(), [stopCamera]);

  const scanLoop = useCallback(() => {
    const video  = videoRef.current;
    const canvas = canvasRef.current;
    if (!video || !canvas || !streamRef.current) return;

    if (video.readyState >= video.HAVE_ENOUGH_DATA) {
      canvas.width  = video.videoWidth;
      canvas.height = video.videoHeight;
      const ctx = canvas.getContext('2d');
      ctx.drawImage(video, 0, 0);
      const img  = ctx.getImageData(0, 0, canvas.width, canvas.height);
      const code = jsQR(img.data, img.width, img.height, { inversionAttempts: 'dontInvert' });

      if (code?.data) {
        if (code.data.startsWith('otpauth-migration')) {
          const found = decodeMigrationQR(code.data);
          if (found.length > 0) {
            onChange(prev => {
              const seen = new Set(prev.map(a => a.secret_b32));
              return [...prev, ...found.filter(a => !seen.has(a.secret_b32))];
            });
            stopCamera();
            setScanStatus(`Added ${found.length} account${found.length !== 1 ? 's' : ''}`);
            return;
          }
        } else if (code.data.startsWith('otpauth://totp/')) {
          setScanStatus('Single-account QR — use "Export accounts" in Google Authenticator');
        }
      }
    }
    animRef.current = requestAnimationFrame(scanLoop);
  }, [onChange, stopCamera]);

  const startCamera = async () => {
    setCamError('');
    setScanStatus('');
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: 'environment', width: { ideal: 1280 } },
      });
      streamRef.current         = stream;
      videoRef.current.srcObject = stream;   // video is already in DOM
      await videoRef.current.play();
      setScanning(true);                     // now show the container
      animRef.current = requestAnimationFrame(scanLoop);
    } catch (e) {
      setCamError('Camera access denied. Allow camera permissions and try again.');
    }
  };

  const moveAccount = (i, dir) => {
    const j = i + dir;
    onChange(prev => {
      const next = [...prev];
      [next[i], next[j]] = [next[j], next[i]];
      return next;
    });
  };


  const addManual = () => {
    const e = {};
    if (!manual.name.trim())   e.name   = 'Name is required';
    if (!manual.secret.trim()) e.secret = 'Secret is required';
    else if (!/^[A-Z2-7]+=*$/i.test(manual.secret.trim()))
      e.secret = 'Must be a valid base32 string (A–Z, 2–7)';
    setManualErr(e);
    if (Object.keys(e).length) return;
    onChange(prev => [...prev, {
      name:       manual.name.trim(),
      secret_b32: manual.secret.trim().toUpperCase().replace(/=/g, ''),
    }]);
    setManual({ name: '', secret: '' });
    setShowManual(false);
  };

  return (
    <div className="space-y-4">
      {showExport && <ExportModal accounts={accounts} onClose={() => setShowExport(false)} />}
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-4">
        <div className="flex items-center gap-3">
          <div className="w-10 h-10 rounded-xl bg-blue-600/15 flex items-center justify-center">
            <ShieldCheck size={20} className="text-blue-400" />
          </div>
          <div>
            <h2 className="text-xl font-bold text-white">Authenticator Accounts</h2>
            <p className="text-zinc-500 text-sm">Scan your Google Authenticator export QR</p>
          </div>
        </div>

        {/* ── canvas: always hidden, used for jsQR frame capture ── */}
        <canvas ref={canvasRef} className="hidden" />

        {/* ── Camera view: container hidden via CSS, video never unmounts ── */}
        <div className={scanning ? 'space-y-3' : 'hidden'}>
          <div className="relative rounded-xl overflow-hidden bg-zinc-950 aspect-square">
            <video
              ref={videoRef}
              className="w-full h-full object-cover"
              playsInline
              muted
              autoPlay
            />
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
              <div className="w-52 h-52 border-2 border-white/50 rounded-2xl" />
            </div>
            <div className="absolute bottom-3 inset-x-0 flex justify-center">
              <span className="bg-black/60 backdrop-blur-sm text-white text-xs px-3 py-1.5 rounded-full">
                Point at the export QR code
              </span>
            </div>
          </div>
          <Button variant="secondary" onClick={stopCamera}>
            <CameraOff size={16} /> Stop Scanning
          </Button>
        </div>

        {/* ── Scan button (shown when not scanning) ── */}
        {!scanning && (
          <div className="space-y-2">
            <Button onClick={startCamera}>
              <Camera size={16} /> Scan QR Code
            </Button>
            {camError   && <p className="text-red-400 text-sm">{camError}</p>}
            {scanStatus && <p className="text-emerald-400 text-sm font-medium">{scanStatus}</p>}
            <p className="text-zinc-600 text-xs text-center">
              Google Authenticator → ⋮ → Transfer accounts → Export accounts
            </p>
          </div>
        )}
      </div>

      {/* Account list */}
      {accounts.length > 0 && (
        <div className="bg-zinc-900 border border-zinc-800 rounded-2xl overflow-hidden">
          <div className="px-5 py-3 border-b border-zinc-800 flex items-center justify-between">
            <span className="text-sm font-medium text-zinc-400">
              {accounts.length} account{accounts.length !== 1 ? 's' : ''}
            </span>
            <button
              onClick={() => setShowExport(true)}
              className="flex items-center gap-1.5 text-xs text-zinc-500 hover:text-zinc-300 transition-colors"
            >
              <Download size={13} /> Export
            </button>
          </div>
          <ul className="divide-y divide-zinc-800">
            {accounts.map((a, i) => (
              <li key={i} className="flex items-center gap-2 px-4 py-3">
                {/* Reorder buttons */}
                <div className="flex flex-col shrink-0">
                  <button
                    onClick={() => moveAccount(i, -1)}
                    disabled={i === 0}
                    className="text-zinc-600 hover:text-zinc-300 disabled:opacity-20 disabled:cursor-default p-0.5 transition-colors"
                  >
                    <ChevronUp size={14} />
                  </button>
                  <button
                    onClick={() => moveAccount(i, 1)}
                    disabled={i === accounts.length - 1}
                    className="text-zinc-600 hover:text-zinc-300 disabled:opacity-20 disabled:cursor-default p-0.5 transition-colors"
                  >
                    <ChevronDown size={14} />
                  </button>
                </div>

                <div className="w-7 h-7 rounded-lg bg-zinc-800 flex items-center justify-center text-xs font-bold text-zinc-300 shrink-0">
                  {a.name.charAt(0).toUpperCase()}
                </div>

                <div className="flex-1 min-w-0">
                  <input
                    value={a.name}
                    onChange={e => onChange(prev => prev.map((x, j) => j === i ? { ...x, name: e.target.value } : x))}
                    className="w-full bg-transparent border-b border-zinc-700 focus:border-blue-500 pb-0.5 text-white text-sm font-medium outline-none transition-colors"
                  />
                  <p className="text-zinc-600 text-xs font-mono truncate mt-0.5">{a.secret_b32.slice(0, 16)}…</p>
                </div>

                <button
                  onClick={() => onChange(prev => prev.filter((_, j) => j !== i))}
                  className="text-zinc-600 hover:text-red-400 p-1 transition-colors shrink-0"
                >
                  <Trash2 size={15} />
                </button>
              </li>
            ))}
          </ul>
        </div>
      )}

      {/* Manual entry */}
      {showManual ? (
        <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-5 space-y-3">
          <h3 className="font-semibold text-white">Add Manually</h3>
          <Input label="Account Name" placeholder="GitHub"
            value={manual.name} error={manualErr.name}
            onChange={e => setManual(m => ({ ...m, name: e.target.value }))} />
          <Input label="Base32 Secret" placeholder="JBSWY3DPEHPK3PXP"
            value={manual.secret} error={manualErr.secret}
            autoCapitalize="none" autoCorrect="off"
            onChange={e => setManual(m => ({ ...m, secret: e.target.value }))} />
          <div className="flex gap-2">
            <Button variant="secondary" onClick={() => { setShowManual(false); setManualErr({}); }}>Cancel</Button>
            <Button onClick={addManual}><Plus size={16} /> Add</Button>
          </div>
        </div>
      ) : (
        <button
          onClick={() => setShowManual(true)}
          className="w-full text-blue-400 hover:text-blue-300 text-sm font-medium py-2 transition-colors"
        >
          + Add account manually
        </button>
      )}

      <div className="flex gap-3">
        <Button variant="secondary" onClick={onBack} className="w-auto px-5">
          <ChevronLeft size={18} />
        </Button>
        <Button onClick={onNext}>
          {accounts.length === 0
            ? 'Skip'
            : `Continue with ${accounts.length} account${accounts.length !== 1 ? 's' : ''}`}
        </Button>
      </div>
    </div>
  );
}
