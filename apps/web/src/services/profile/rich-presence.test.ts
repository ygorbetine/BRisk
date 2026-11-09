import { describe, expect, it } from "vitest";
import { isIgnored, parseRemoteApp, validIcon } from "./rich-presence";

describe("rich presence", () => {
  it("valida o ícone: só PNG pequeno em base64", () => {
    expect(validIcon("data:image/png;base64,AAAA")).toBe(true);
    expect(validIcon("data:image/svg+xml;base64,AAAA")).toBe(false);
    expect(validIcon("https://x.test/a.png")).toBe(false);
    expect(validIcon("data:image/png;base64," + "A".repeat(20_000))).toBe(false);
  });
  it("valida o app vindo da rede", () => {
    expect(parseRemoteApp(null)).toBeNull();
    expect(parseRemoteApp({ name: "", label: "x" })).toBeNull();
    expect(parseRemoteApp({ name: "Code.exe", label: "Code", icon: "javascript:alert(1)" })).toEqual({ name: "code.exe", label: "Code" });
    expect(parseRemoteApp({ name: "a.exe", label: "A‮B", icon: "data:image/png;base64,AAAA" })).toEqual({ name: "a.exe", label: "AB", icon: "data:image/png;base64,AAAA" });
  });
  it("respeita a lista de ignorados sem diferenciar maiúsculas", () => {
    expect(isIgnored({ enabled: true, ignored: ["chrome.exe"] }, "Chrome.exe")).toBe(true);
    expect(isIgnored({ enabled: true, ignored: [] }, "chrome.exe")).toBe(false);
  });
});
