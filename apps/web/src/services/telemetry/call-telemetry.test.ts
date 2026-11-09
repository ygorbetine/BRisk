import { describe, expect, it } from "vitest";
import { CallOutcomeTracker, telemetryPath } from "./call-telemetry";

describe("telemetria de chamadas", () => {
  it("classifica o caminho", () => {
    expect(telemetryPath("turn-relay")).toBe("relay");
    expect(telemetryPath("internet-direct")).toBe("direct");
    expect(telemetryPath("lan-direct")).toBe("direct");
    expect(telemetryPath(undefined)).toBe("unknown");
  });
  it("conta uma vez por pessoa: conectou ganha de falhas posteriores", () => {
    const sent: string[] = [];
    const tracker = new CallOutcomeTracker((outcome, path) => sent.push(`${outcome}:${path}`), () => true);
    tracker.connected("a", "relay");
    tracker.failed("a");
    tracker.connected("a", "direct");
    tracker.failed("b");
    expect(sent).toEqual(["connected:relay", "failed:unknown"]);
    tracker.reset();
    tracker.connected("a", "direct");
    expect(sent).toHaveLength(3);
  });
  it("não envia nada quando desligada", () => {
    const sent: string[] = [];
    const tracker = new CallOutcomeTracker(() => sent.push("x"), () => false);
    tracker.connected("a", "direct");
    expect(sent).toEqual([]);
  });
});
