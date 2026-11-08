import gateWorkletUrl from "./noise-gate-worklet.js?url";

export type NoiseGate = {
  track: MediaStreamTrack;
  stop(): Promise<void>;
};

export const DEFAULT_GATE_THRESHOLD_DB = -45;
export const MIN_GATE_THRESHOLD_DB = -70;
export const MAX_GATE_THRESHOLD_DB = -20;

export function clampGateThreshold(value: unknown): number {
  const n = typeof value === "number" && Number.isFinite(value) ? value : DEFAULT_GATE_THRESHOLD_DB;
  return Math.min(MAX_GATE_THRESHOLD_DB, Math.max(MIN_GATE_THRESHOLD_DB, Math.round(n)));
}

/** Passa a track por uma porta de ruído local. Devolve uma nova track mono. */
export async function createNoiseGate(inputTrack: MediaStreamTrack, thresholdDb: number): Promise<NoiseGate> {
  const context = new AudioContext({ latencyHint: "interactive" });
  try {
    await context.audioWorklet.addModule(gateWorkletUrl);
    const source = context.createMediaStreamSource(new MediaStream([inputTrack]));
    const node = new AudioWorkletNode(context, "briks-noise-gate", { channelCount: 1, channelCountMode: "explicit", numberOfOutputs: 1, outputChannelCount: [1] });
    node.port.postMessage({ thresholdDb: clampGateThreshold(thresholdDb) });
    const destination = context.createMediaStreamDestination();
    destination.channelCount = 1;
    destination.channelCountMode = "explicit";
    source.connect(node).connect(destination);
    await context.resume();
    const track = destination.stream.getAudioTracks()[0];
    if (!track || context.state !== "running") throw new Error("Noise gate não iniciou.");
    track.contentHint = "speech";
    let stopped = false;
    return {
      track,
      async stop() {
        if (stopped) return;
        stopped = true;
        track.stop();
        source.disconnect();
        node.disconnect();
        await context.close().catch(() => undefined);
      },
    };
  } catch (error) {
    await context.close().catch(() => undefined);
    throw error;
  }
}
