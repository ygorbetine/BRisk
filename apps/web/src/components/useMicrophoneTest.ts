import { useCallback, useEffect, useRef, useState } from "react";

/** Mede o nível do microfone escolhido por alguns segundos, sem enviar o áudio a lugar nenhum. */
export function useMicrophoneTest(deviceId: string, seconds = 12) {
  const [level, setLevel] = useState(0);
  const [active, setActive] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const cleanupRef = useRef<() => void>(() => undefined);

  const stop = useCallback(() => {
    cleanupRef.current();
    cleanupRef.current = () => undefined;
    setActive(false);
    setLevel(0);
  }, []);

  const start = useCallback(async () => {
    stop();
    setError(null);
    if (!navigator.mediaDevices?.getUserMedia) {
      setError("Este ambiente não permite testar o microfone.");
      return;
    }
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: deviceId ? { deviceId: { exact: deviceId } } : true,
        video: false,
      });
      const context = new AudioContext();
      const source = context.createMediaStreamSource(stream);
      const analyser = context.createAnalyser();
      analyser.fftSize = 512;
      source.connect(analyser);
      const data = new Uint8Array(analyser.fftSize);
      let frame = 0;
      const tick = () => {
        analyser.getByteTimeDomainData(data);
        let peak = 0;
        for (const value of data) peak = Math.max(peak, Math.abs(value - 128));
        setLevel(Math.min(1, (peak / 128) * 1.8));
        frame = requestAnimationFrame(tick);
      };
      tick();
      const timer = window.setTimeout(stop, seconds * 1000);
      cleanupRef.current = () => {
        cancelAnimationFrame(frame);
        window.clearTimeout(timer);
        stream.getTracks().forEach((track) => track.stop());
        void context.close().catch(() => undefined);
      };
      setActive(true);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível abrir o microfone.");
    }
  }, [deviceId, seconds, stop]);

  useEffect(() => () => cleanupRef.current(), []);
  return { level, active, error, start, stop };
}
