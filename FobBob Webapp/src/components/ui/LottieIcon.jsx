import Lottie from 'lottie-react';

export default function LottieIcon({ animationData, size = 80, loop = true, autoplay = true, className = '' }) {
  return (
    <div style={{ width: size, height: size }} className={`shrink-0 ${className}`}>
      <Lottie animationData={animationData} loop={loop} autoplay={autoplay} />
    </div>
  );
}
