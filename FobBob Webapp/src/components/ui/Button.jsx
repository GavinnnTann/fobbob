export default function Button({ children, onClick, disabled, variant = 'primary', className = '' }) {
  const base = 'w-full py-3.5 px-6 rounded-xl font-semibold text-[15px] transition-all duration-150 flex items-center justify-center gap-2 select-none';

  const variants = {
    primary:   'bg-blue-600 hover:bg-blue-500 active:bg-blue-700 text-white disabled:bg-zinc-800 disabled:text-zinc-500',
    secondary: 'bg-zinc-800 hover:bg-zinc-700 active:bg-zinc-900 text-white disabled:opacity-40',
    danger:    'bg-red-600 hover:bg-red-500 active:bg-red-700 text-white disabled:opacity-40',
    ghost:     'text-blue-400 hover:text-blue-300 disabled:text-zinc-600',
  };

  return (
    <button
      onClick={onClick}
      disabled={disabled}
      className={`${base} ${variants[variant]} ${className}`}
    >
      {children}
    </button>
  );
}
