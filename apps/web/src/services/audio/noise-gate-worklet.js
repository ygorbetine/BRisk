// AudioWorklet do noise gate: roda na thread de áudio (não sofre com a aba em segundo plano).
// Abre quando o nível passa do limiar, segura um tempo e fecha suavemente. Nunca fica travado:
// se a entrada some ou o limiar é inválido, o ganho volta a 1.
class NoiseGateProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.threshold = Math.pow(10, -45 / 20);
    this.gain = 1;
    this.holdLeft = 0;
    this.holdSamples = Math.round(sampleRate * 0.2);
    this.attack = 1 - Math.exp(-1 / (sampleRate * 0.004));
    this.release = 1 - Math.exp(-1 / (sampleRate * 0.08));
    this.port.onmessage = (event) => {
      const db = event.data && event.data.thresholdDb;
      if (typeof db === "number" && db <= 0 && db >= -100) this.threshold = Math.pow(10, db / 20);
    };
  }

  process(inputs, outputs) {
    const input = inputs[0];
    const output = outputs[0];
    if (!input || input.length === 0) { this.gain = 1; return true; }
    const frames = input[0].length;
    let peak = 0;
    for (let c = 0; c < input.length; c += 1) {
      const data = input[c];
      for (let i = 0; i < frames; i += 1) { const v = Math.abs(data[i]); if (v > peak) peak = v; }
    }
    const open = peak >= this.threshold;
    if (open) this.holdLeft = this.holdSamples;
    else this.holdLeft = Math.max(0, this.holdLeft - frames);
    const target = open || this.holdLeft > 0 ? 1 : 0;
    const rate = target > this.gain ? this.attack : this.release;
    for (let i = 0; i < frames; i += 1) {
      this.gain += (target - this.gain) * rate;
      for (let c = 0; c < output.length; c += 1) {
        const src = input[c] ?? input[0];
        output[c][i] = src[i] * this.gain;
      }
    }
    return true;
  }
}
registerProcessor("briks-noise-gate", NoiseGateProcessor);
