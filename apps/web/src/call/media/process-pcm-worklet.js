// AudioWorklet que toca PCM f32 estéreo recebido do sidecar (captura por processo).
// Mantém uma fila curta: se acumular demais, descarta o excesso para ficar ao vivo.
class ProcessPcmProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.chunks = [];
    this.offset = 0; // frames já consumidos do primeiro chunk
    this.queued = 0; // frames na fila
    this.maxQueued = Math.round(sampleRate * 0.25);
    this.keepQueued = Math.round(sampleRate * 0.08);
    this.port.onmessage = (event) => {
      const samples = event.data;
      if (!(samples instanceof Float32Array)) return;
      this.chunks.push(samples);
      this.queued += samples.length / 2;
      while (this.queued > this.maxQueued && this.chunks.length > 1) {
        const dropped = this.chunks.shift();
        const droppedFrames = dropped.length / 2 - this.offset;
        this.offset = 0;
        this.queued -= droppedFrames;
        if (this.queued <= this.keepQueued) break;
      }
    };
  }

  process(_inputs, outputs) {
    const output = outputs[0];
    const left = output[0];
    const right = output[1] ?? output[0];
    for (let i = 0; i < left.length; i += 1) {
      const chunk = this.chunks[0];
      if (!chunk) {
        left[i] = 0;
        right[i] = 0;
        continue;
      }
      left[i] = chunk[this.offset * 2];
      right[i] = chunk[this.offset * 2 + 1];
      this.offset += 1;
      this.queued -= 1;
      if (this.offset * 2 >= chunk.length) {
        this.chunks.shift();
        this.offset = 0;
      }
    }
    return true;
  }
}

registerProcessor("process-pcm", ProcessPcmProcessor);
