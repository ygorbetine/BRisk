import { describe, expect, it, vi } from "vitest";
import { LiveKitTransport, SCREEN_SIMULCAST_LAYERS } from "./sfu";

type PublishOptions = Record<string, unknown>;

function transportWithSpy() {
  const publishTrack = vi.fn(async (_track: unknown, _options: PublishOptions) => ({}));
  const transport = new LiveKitTransport("peer-a", {} as never, async () => ({ url: "wss://x", token: "t" }));
  (transport as unknown as { room: unknown }).room = { localParticipant: { publishTrack } };
  return { transport, publishTrack };
}

const stream = { id: "stream-1" } as MediaStream;
const videoTrack = () => ({ kind: "video", contentHint: "" }) as unknown as MediaStreamTrack;

describe("LiveKitTransport.publishTrack", () => {
  it("publica a tela em H.264 com camadas menores de 30 fps", async () => {
    const { transport, publishTrack } = transportWithSpy();
    await transport.publishTrack(videoTrack(), stream, { source: "screen" });
    const options = publishTrack.mock.calls[0]![1];
    expect(options.videoCodec).toBe("h264");
    expect(options.simulcast).toBe(true);
    expect(options.screenShareSimulcastLayers).toBe(SCREEN_SIMULCAST_LAYERS);
    for (const layer of SCREEN_SIMULCAST_LAYERS) expect(layer.encoding.maxFramerate).toBe(30);
  });

  it("mantém os bitrates das camadas iguais aos usados na estimativa de upload do painel", () => {
    expect(SCREEN_SIMULCAST_LAYERS.map((layer) => layer.encoding.maxBitrate)).toEqual([1_700_000, 600_000]);
  });

  it("usa motion como dica padrão na tela, sem sobrescrever a escolhida pelo usuário", async () => {
    const { transport } = transportWithSpy();
    const fallback = videoTrack();
    await transport.publishTrack(fallback, stream, { source: "screen" });
    expect(fallback.contentHint).toBe("motion");

    const chosen = { kind: "video", contentHint: "detail" } as unknown as MediaStreamTrack;
    await transport.publishTrack(chosen, stream, { source: "screen" });
    expect(chosen.contentHint).toBe("detail");

    const hinted = videoTrack();
    await transport.publishTrack(hinted, stream, { source: "screen", contentHint: "detail" });
    expect(hinted.contentHint).toBe("detail");
  });

  it("não altera o contentHint da câmera", async () => {
    const { transport } = transportWithSpy();
    const camera = videoTrack();
    await transport.publishTrack(camera, stream, { source: "camera" });
    expect(camera.contentHint).toBe("");
  });
});
