import { describe, expect, it } from "vitest";
import { defaultNotifySettings, friendEvents, STARTUP_GRACE_MS, toggleMuted } from "./friend-notifications";

const set = (...ids: string[]) => new Set(ids);
const empty = { online: set(), streaming: set() };
const base = { settings: defaultNotifySettings(), friends: set("a", "b"), inCall: false, sinceStartMs: STARTUP_GRACE_MS + 1 };

describe("avisos de amigos", () => {
  it("avisa quando um amigo fica online", () => {
    expect(friendEvents(empty, { online: set("a"), streaming: set() }, base)).toEqual([{ peerId: "a", kind: "online" }]);
  });
  it("não avisa sobre quem não é amigo", () => {
    expect(friendEvents(empty, { online: set("z"), streaming: set("z") }, base)).toEqual([]);
  });
  it("ficar online já transmitindo gera só o aviso de transmissão", () => {
    expect(friendEvents(empty, { online: set("a"), streaming: set("a") }, base)).toEqual([{ peerId: "a", kind: "streaming" }]);
  });
  it("com aviso de transmissão desligado, ainda avisa que ficou online", () => {
    const settings = { ...defaultNotifySettings(), streaming: false };
    expect(friendEvents(empty, { online: set("a"), streaming: set("a") }, { ...base, settings })).toEqual([{ peerId: "a", kind: "online" }]);
  });
  it("silencia durante a chamada, na abertura e por amigo", () => {
    const next = { online: set("a", "b"), streaming: set() };
    expect(friendEvents(empty, next, { ...base, inCall: true })).toEqual([]);
    expect(friendEvents(empty, next, { ...base, sinceStartMs: 1_000 })).toEqual([]);
    expect(friendEvents(empty, next, { ...base, settings: toggleMuted(defaultNotifySettings(), "a") })).toEqual([{ peerId: "b", kind: "online" }]);
  });
  it("não repete para quem já estava online", () => {
    const online = { online: set("a"), streaming: set() };
    expect(friendEvents(online, online, base)).toEqual([]);
  });
  it("alternar silêncio adiciona e remove", () => {
    const once = toggleMuted(defaultNotifySettings(), "a");
    expect(once.muted).toEqual(["a"]);
    expect(toggleMuted(once, "a").muted).toEqual([]);
  });
});
