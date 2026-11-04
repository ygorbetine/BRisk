import { describe, expect, it } from "vitest";
import { resolveVideoSenderPolicy } from "./video-encoding";

describe("política de codificação de vídeo", () => {
  it("oferece bitrate apropriado para tela 1080p e limita o total do mesh", () => {
    const screen = { source: "screen" as const, maxBitrate: 9_000_000, maxFramerate: 60, targetWidth: 1920, targetHeight: 1080 };
    expect(resolveVideoSenderPolicy(screen, 1).maxBitrate).toBe(9_000_000);
    expect(resolveVideoSenderPolicy(screen, 2).maxBitrate).toBe(9_000_000);
    expect(resolveVideoSenderPolicy(screen, 5).maxBitrate).toBe(3_600_000);
  });

  it("equilibra fluidez e resolução de conteúdo em movimento e limita FPS no sender", () => {
    const policy = resolveVideoSenderPolicy({ source: "screen", maxFramerate: 30, degradationPreference: "balanced" }, 1);
    expect(policy).toMatchObject({
      maxFramerate: 30,
      degradationPreference: "balanced",
      priority: "high",
    });
  });

  it("reduz no sender uma captura que ignorou a resolução solicitada", () => {
    const policy = resolveVideoSenderPolicy(
      { source: "screen", targetWidth: 1920, targetHeight: 1080 },
      1,
      { width: 3840, height: 2160 },
    );
    expect(policy.scaleResolutionDownBy).toBe(2);
  });

  it("reduz prioridade da câmera enquanto a tela está publicada", () => {
    expect(resolveVideoSenderPolicy({ source: "camera" }, 1, {}, true)).toMatchObject({
      maxBitrate: 600_000,
      degradationPreference: "balanced",
      priority: "low",
    });
  });

  it("sem bitrate forçado mantém o teto de 12 Mbps e a divisão da malha", () => {
    const screen = { source: "screen" as const, maxBitrate: 30_000_000 };
    expect(resolveVideoSenderPolicy(screen, 1).maxBitrate).toBe(12_000_000);
  });

  it("bitrate forçado pelo usuário vai até 50 Mbps e ignora o teto da malha", () => {
    const screen = { source: "screen" as const, maxBitrate: 30_000_000, forceBitrate: true };
    expect(resolveVideoSenderPolicy(screen, 1).maxBitrate).toBe(30_000_000);
    expect(resolveVideoSenderPolicy(screen, 6).maxBitrate).toBe(30_000_000);
    expect(resolveVideoSenderPolicy({ ...screen, maxBitrate: 90_000_000 }, 1).maxBitrate).toBe(50_000_000);
  });
});
