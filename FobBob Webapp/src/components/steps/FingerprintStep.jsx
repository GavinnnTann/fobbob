import { useState, useEffect, useRef, useCallback } from 'react';
import { Plus, Trash2, ChevronLeft, AlertCircle, CheckCircle } from 'lucide-react';
import { sendFpCommand } from '../../lib/ble';
import Button from '../ui/Button';
import Input from '../ui/Input';
import LottieIcon from '../ui/LottieIcon';
import fingerprintAnim from '../../assets/lottie/fingerprint.json';
import tickAnim from '../../assets/lottie/tick.json';
import crossAnim from '../../assets/lottie/cross.json';

const MAX_FINGERS = 5;
const POLL_INTERVAL_MS = 400;  // poll FP_STATUS this often during enrollment

const STATUS_MESSAGES = {
  PLACE_FINGER: { text: 'Place your finger on the sensor', color: 'text-blue-400' },
  LIFT_FINGER:  { text: 'Lift your finger',                color: 'text-amber-400' },
  PLACE_AGAIN:  { text: 'Place your finger again',         color: 'text-blue-400' },
};

const dec = new TextDecoder();

export default function FingerprintStep({ chars, deviceInfo, fingerprints, onChange, onNext, onBack }) {
  // debug_mode field removed from firmware — always false unless device explicitly sends it
  const debugMode = deviceInfo?.debug_mode === true;

  const [newName, setNewName]       = useState('');
  const [enrolling, setEnrolling]   = useState(false);
  const [fpStatus, setFpStatus]     = useState('');
  const [deletingId, setDeletingId] = useState(null);
  const [error, setError]           = useState('');
  const [success, setSuccess]       = useState('');

  const pendingNameRef = useRef('');
  const deletingIdRef  = useRef(null);
  const pollRef        = useRef(null);   // setInterval handle for FP_STATUS polling
  const lastStatusRef  = useRef('');     // deduplicate repeated poll values

  // Stop polling (safe to call at any time).
  // lastStatusRef is intentionally NOT reset — its stale value acts as the dedup
  // seed for the next poll session, preventing a stale FP_STATUS characteristic
  // value from firing as a new event (e.g. ENROLLED_OK appearing during DELETE).
  const stopPoll = useCallback(() => {
    if (pollRef.current) {
      clearInterval(pollRef.current);
      pollRef.current = null;
    }
  }, []);

  // Cleanup on unmount
  useEffect(() => stopPoll, [stopPoll]);

  const handleFpStatus = useCallback((status, fp_id) => {
    // Include fp_id in the dedup key so ENROLLED_OK:0 and ENROLLED_OK:1 are
    // distinct events, while a stale ENROLLED_OK:0 is correctly suppressed
    // when the poll restarts for a DELETE on the same slot.
    const dedupKey = `${status}:${fp_id}`;
    if (dedupKey === lastStatusRef.current) return;
    lastStatusRef.current = dedupKey;

    setError('');
    setFpStatus(status);

    if (status === 'ENROLLED_OK') {
      stopPoll();
      onChange(prev => [...prev, { slot_id: fp_id, name: pendingNameRef.current }]);
      setEnrolling(false);
      setFpStatus('');
      setNewName('');
      setSuccess('Finger enrolled!');
      setTimeout(() => setSuccess(''), 3000);
    } else if (status === 'ENROLL_FAILED') {
      stopPoll();
      setEnrolling(false);
      setFpStatus('');
      setError('Enrollment failed. Please try again.');
    } else if (status === 'DELETE_OK') {
      stopPoll();
      onChange(prev => prev.filter(f => f.slot_id !== deletingIdRef.current));
      setDeletingId(null);
      deletingIdRef.current = null;
    } else if (status === 'DELETE_FAILED') {
      stopPoll();
      setDeletingId(null);
      deletingIdRef.current = null;
      setError('Failed to delete fingerprint from sensor. Please try again.');
    }
  }, [onChange, stopPoll]);

  // Poll FP_STATUS by reading the characteristic value — avoids startNotifications()
  // which crashes Chrome with NimBLE NOTIFY characteristics on some platforms.
  // The firmware calls setValue() before notify() so readValue() always returns
  // the current status.
  const startPoll = useCallback(() => {
    if (!chars?.FP_STATUS || pollRef.current) return;
    let consecutiveErrors = 0;
    pollRef.current = setInterval(async () => {
      try {
        const val = await chars.FP_STATUS.readValue();
        consecutiveErrors = 0;
        const data = JSON.parse(dec.decode(val));
        if (data?.status) handleFpStatus(data.status, data.fp_id ?? 0);
      } catch {
        // Transient BLE read errors are normal; sustained failures mean the
        // connection dropped (BLE supervision timeout). Stop the poll and
        // surface an error so the spinner doesn't hang forever.
        if (++consecutiveErrors >= 5) {
          stopPoll();
          setDeletingId(null);
          deletingIdRef.current = null;
          setEnrolling(false);
          setError('Device disconnected. Please refresh and reconnect.');
        }
      }
    }, POLL_INTERVAL_MS);
  }, [chars, handleFpStatus, stopPoll]);

  const startEnroll = async () => {
    if (fingerprints.length >= MAX_FINGERS || enrolling) return;
    const name = newName.trim() || `Finger ${fingerprints.length + 1}`;
    pendingNameRef.current = name;
    setEnrolling(true);
    setError('');
    setSuccess('');
    setFpStatus('');
    try {
      await sendFpCommand(chars, { cmd: 'START_ENROLL', name });
      startPoll();   // begin polling for status updates
    } catch (e) {
      setEnrolling(false);
      setError(e.message || 'Failed to start enrollment.');
    }
  };

  const deleteFinger = async (slot_id) => {
    if (deletingId !== null || enrolling) return;
    deletingIdRef.current = slot_id;
    setDeletingId(slot_id);
    setError('');
    setSuccess('');
    try {
      startPoll();  // poll for DELETE_OK — stopped in handleFpStatus when received
      await sendFpCommand(chars, { cmd: 'DELETE', id: slot_id });
    } catch (e) {
      stopPoll();
      setDeletingId(null);
      deletingIdRef.current = null;
      setError(e.message || 'Failed to delete finger.');
    }
  };

  const busy = enrolling || deletingId !== null;
  const canContinue = debugMode || fingerprints.length >= 1;
  const statusInfo = STATUS_MESSAGES[fpStatus];

  return (
    <div className="space-y-4">
      <div className="bg-zinc-900 border border-zinc-800 rounded-2xl p-6 space-y-4">
        {/* Header */}
        <div className="flex items-center gap-3">
          <LottieIcon animationData={fingerprintAnim} size={44} loop={enrolling} autoplay={enrolling} />
          <div>
            <h2 className="text-xl font-bold text-white">Fingerprints</h2>
            <p className="text-zinc-500 text-sm">Register up to {MAX_FINGERS} fingerprints (min. 1)</p>
          </div>
        </div>

        {/* Debug mode notice */}
        {debugMode && (
          <div className="bg-amber-500/10 border border-amber-500/30 rounded-xl p-3 text-amber-400 text-sm leading-relaxed">
            Device is in debug mode — fingerprint hardware is disabled. Fingerprint registration is skipped automatically.
          </div>
        )}

        {/* Enrolled fingers */}
        {fingerprints.length > 0 && (
          <div className="space-y-1.5">
            <p className="text-xs font-medium text-zinc-500 uppercase tracking-wider">
              Enrolled ({fingerprints.length}/{MAX_FINGERS})
            </p>
            <div className="bg-zinc-800 rounded-xl divide-y divide-zinc-700/60 overflow-hidden">
              {fingerprints.map((f, i) => (
                <div key={f.slot_id} className="flex items-center gap-3 px-4 py-3">
                  <div className="w-7 h-7 rounded-lg bg-blue-600/20 flex items-center justify-center text-xs font-bold text-blue-400 shrink-0">
                    {i + 1}
                  </div>
                  <input
                    value={f.name}
                    onChange={e => onChange(prev => prev.map((x, j) => j === i ? { ...x, name: e.target.value } : x))}
                    className="flex-1 min-w-0 bg-transparent text-white text-sm font-medium outline-none border-b border-transparent focus:border-blue-500 pb-0.5 transition-colors"
                    disabled={busy}
                    placeholder={`Finger ${i + 1}`}
                  />
                  {!debugMode && (
                    <button
                      onClick={() => deleteFinger(f.slot_id)}
                      disabled={busy}
                      className="text-zinc-600 hover:text-red-400 p-1 transition-colors shrink-0 disabled:opacity-30 disabled:cursor-default"
                    >
                      {deletingId === f.slot_id
                        ? <div className="w-4 h-4 border-2 border-zinc-500 border-t-transparent rounded-full animate-spin" />
                        : <Trash2 size={15} />}
                    </button>
                  )}
                </div>
              ))}
            </div>
          </div>
        )}

        {/* Enroll new finger form */}
        {!debugMode && fingerprints.length < MAX_FINGERS && (
          <div className="space-y-2">
            <Input
              label="New finger name (optional)"
              placeholder={`Finger ${fingerprints.length + 1}`}
              value={newName}
              onChange={e => setNewName(e.target.value)}
              disabled={busy}
            />
            <Button onClick={startEnroll} disabled={busy}>
              <Plus size={16} />
              {enrolling ? 'Enrolling…' : 'Enroll Finger'}
            </Button>
            {statusInfo && (
              <p className={`text-sm font-medium text-center ${statusInfo.color}`}>
                {statusInfo.text}
              </p>
            )}
          </div>
        )}

        {/* Feedback messages */}
        {error && (
          <div className="flex items-center gap-2 text-red-400 text-sm">
            <LottieIcon animationData={crossAnim} size={24} loop={false} />
            {error}
          </div>
        )}
        {success && (
          <div className="flex items-center gap-2 text-emerald-400 text-sm">
            <LottieIcon animationData={tickAnim} size={24} loop={false} />
            {success}
          </div>
        )}

        {!debugMode && fingerprints.length === 0 && (
          <p className="text-zinc-600 text-xs text-center">
            At least one fingerprint must be enrolled to continue
          </p>
        )}
      </div>

      <div className="flex gap-3">
        <Button variant="secondary" onClick={onBack} className="w-auto px-5" disabled={busy}>
          <ChevronLeft size={18} />
        </Button>
        <Button onClick={onNext} disabled={!canContinue || busy}>
          {debugMode
            ? 'Skip (Debug Mode)'
            : fingerprints.length === 0
              ? 'Continue'
              : `Continue with ${fingerprints.length} finger${fingerprints.length !== 1 ? 's' : ''}`}
        </Button>
      </div>
    </div>
  );
}
