import { Fragment } from 'react';
import { Check } from 'lucide-react';

const STEPS = ['Connect', 'Name', 'WiFi', 'Settings', 'Fingerprint', 'Accounts', 'Confirm'];

export default function StepBar({ current }) {
  return (
    <div className="flex items-center">
      {STEPS.map((label, i) => {
        const done   = i < current;
        const active = i === current;
        const last   = i === STEPS.length - 1;

        return (
          <Fragment key={i}>
            <div className="flex flex-col items-center gap-1">
              <div className={`w-8 h-8 rounded-full flex items-center justify-center text-sm font-semibold transition-all
                ${done   ? 'bg-blue-600 text-white'
                : active ? 'bg-blue-600 text-white ring-4 ring-blue-600/20'
                :          'bg-zinc-800 text-zinc-500'}`}>
                {done ? <Check size={14} strokeWidth={3} /> : i + 1}
              </div>
              <span className={`text-[10px] font-medium whitespace-nowrap
                ${active ? 'text-white' : done ? 'text-zinc-400' : 'text-zinc-600'}`}>
                {label}
              </span>
            </div>

            {!last && (
              <div className={`flex-1 h-px mx-3 mb-4 transition-colors
                ${i < current ? 'bg-blue-600' : 'bg-zinc-800'}`} />
            )}
          </Fragment>
        );
      })}
    </div>
  );
}
