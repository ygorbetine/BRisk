import rnnoiseWorkletPath from "@sapphi-red/web-noise-suppressor/rnnoiseWorklet.js?url";
import rnnoiseWasmPath from "@sapphi-red/web-noise-suppressor/rnnoise.wasm?url";
import rnnoiseWasmSimdPath from "@sapphi-red/web-noise-suppressor/rnnoise_simd.wasm?url";

export type RnnoiseMicrophone = {
  track: MediaStreamTrack;
  stop(): Promise<void>;
};

type RnnoiseNode = AudioWorkletNode & { destroy(): void };
type RnnoiseModule = typeof import("@sapphi-red/web-noise-suppressor");

let modulePromise: Promise<RnnoiseModule> | undefined;
let wasmBinaryPromise: Promise<ArrayBuffer> | undefined;

function loadModule(): Promise<RnnoiseModule> {
  // Importação lazy: os testes Vitest rodam em Node, onde AudioWorkletNode não
  // existe. O módulo só deve ser avaliado no renderer quando RNNoise for usado.
  modulePromise ??= import("@sapphi-red/web-noise-suppressor");
  return modulePromise;
}

async function loadWasmBinary(): Promise<ArrayBuffer> {
  if (!wasmBinaryPromise) {
    wasmBinaryPromise = loadModule().then(({ loadRnnoise }) => loadRnnoise({
      url: rnnoiseWasmPath,
      simdUrl: rnnoiseWasmSimdPath,
    }));
  }
  return wasmBinaryPromise;
}

/**
 * Processa o microfone localmente com RNNoise em um AudioWorklet.
 *
 * O RNNoise usado pelo worklet opera a 48 kHz. O MediaStreamAudioSourceNode
 * faz a conversão da entrada para a taxa do AudioContext e o WebRTC recebe
 * apenas a track produzida pelo MediaStreamAudioDestinationNode.
 *
 * Toda a cadeia é explicitamente mono. MediaStreamAudioDestinationNode nasce
 * estéreo por padrão; sem este downmix, o RNNoise (maxChannels: 1) pode preencher
 * somente o primeiro canal e a voz chegar ao peer em apenas um lado do fone.
 */
export async function createRnnoiseMicrophone(inputStream: MediaStream): Promise<RnnoiseMicrophone> {
  const context = new AudioContext({ sampleRate: 48_000, latencyHint: "interactive" });
  let source: MediaStreamAudioSourceNode | undefined;
  let suppressor: RnnoiseNode | undefined;
  let destination: MediaStreamAudioDestinationNode | undefined;

  try {
    if (context.sampleRate !== 48_000) {
      throw new Error(`RNNoise requer AudioContext a 48 kHz; recebido ${context.sampleRate} Hz.`);
    }

    const [{ RnnoiseWorkletNode }, wasmBinary] = await Promise.all([
      loadModule(),
      loadWasmBinary(),
      context.audioWorklet.addModule(rnnoiseWorkletPath),
    ]);

    source = context.createMediaStreamSource(inputStream);
    suppressor = new RnnoiseWorkletNode(context, {
      wasmBinary,
      maxChannels: 1,
    });

    // O input pode ser entregue como estéreo por alguns drivers/Chromium mesmo
    // quando o microfone físico é mono. "speakers" faz o downmix correto para um
    // único canal antes de o RNNoise processar os frames de 480 amostras.
    suppressor.channelCount = 1;
    suppressor.channelCountMode = "explicit";
    suppressor.channelInterpretation = "speakers";

    destination = context.createMediaStreamDestination();
    destination.channelCount = 1;
    destination.channelCountMode = "explicit";
    destination.channelInterpretation = "speakers";

    source.connect(suppressor).connect(destination);

    await context.resume();
    if (context.state !== "running") {
      throw new Error(`RNNoise não conseguiu iniciar o AudioContext (${context.state}).`);
    }

    const outputTrack = destination.stream.getAudioTracks()[0];
    if (!outputTrack || outputTrack.readyState !== "live") {
      throw new Error("RNNoise não produziu uma track de áudio ativa.");
    }
    outputTrack.contentHint = "speech";

    const outputSettings = outputTrack.getSettings();
    console.info("Briks RNNoise microphone", {
      sampleRate: context.sampleRate,
      channelCount: outputSettings.channelCount ?? 1,
      contextState: context.state,
    });

    let stopped = false;
    return {
      track: outputTrack,
      async stop() {
        if (stopped) return;
        stopped = true;
        outputTrack.stop();
        source?.disconnect();
        suppressor?.disconnect();
        suppressor?.destroy();
        destination?.disconnect();
        await context.close().catch(() => undefined);
      },
    };
  } catch (error) {
    source?.disconnect();
    suppressor?.disconnect();
    suppressor?.destroy();
    destination?.disconnect();
    await context.close().catch(() => undefined);
    throw error;
  }
}