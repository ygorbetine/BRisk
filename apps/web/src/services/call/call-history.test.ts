import { describe, expect, it } from "vitest";
import { CallHistoryRecorder, formatDuration, MAX_ENTRIES, parseEntries, prependEntry, type CallHistoryEntry } from "./call-history";

const entry = (id: string): CallHistoryEntry => ({ id, startedAt: 0, endedAt: 60_000, groupName: "G", channelName: "Geral", participants: [], streamed: false });

describe("histórico de chamadas", () => {
  it("monta um registro do começo ao fim da chamada", () => {
    let clock = 1_000;
    const finished: CallHistoryEntry[] = [];
    const recorder = new CallHistoryRecorder((item) => finished.push(item), () => clock);
    recorder.update({ roomId: "room-1", groupName: "Amigos", channelName: "Geral", participants: [{ peerId: "a", displayName: "Peer abc123" }], streaming: false });
    clock = 5_000;
    recorder.update({ roomId: "room-1", participants: [{ peerId: "a", displayName: "Ana" }, { peerId: "b", displayName: "Bia" }], streaming: true });
    recorder.update({ roomId: "room-1", participants: [{ peerId: "b", displayName: "Bia" }], streaming: false });
    clock = 61_000;
    recorder.update({ roomId: null, participants: [], streaming: false });
    expect(finished).toEqual([{ id: "1000-room-1", startedAt: 1_000, endedAt: 61_000, groupName: "Amigos", channelName: "Geral", participants: ["Ana", "Bia"], streamed: true }]);
  });
  it("trocar de sala fecha a anterior e abre outra", () => {
    const finished: CallHistoryEntry[] = [];
    const recorder = new CallHistoryRecorder((item) => finished.push(item), () => 0);
    recorder.update({ roomId: "r1", participants: [], streaming: false });
    recorder.update({ roomId: "r2", participants: [], streaming: false });
    expect(finished.map((item) => item.id)).toEqual(["0-r1"]);
  });
  it("mantém só os mais recentes", () => {
    let list: CallHistoryEntry[] = [];
    for (let index = 0; index < MAX_ENTRIES + 5; index += 1) list = prependEntry(list, entry(String(index)));
    expect(list).toHaveLength(MAX_ENTRIES);
    expect(list[0]!.id).toBe(String(MAX_ENTRIES + 4));
  });
  it("descarta registros corrompidos e limpa nomes", () => {
    expect(parseEntries("x")).toEqual([]);
    expect(parseEntries([{ id: "1", startedAt: 10, endedAt: 5 }])).toEqual([]);
    expect(parseEntries([{ id: "1", startedAt: 0, endedAt: 1, groupName: "a\u0007b", participants: ["", "Ana", 3] }])[0]).toMatchObject({ groupName: "ab", participants: ["Ana"], streamed: false });
  });
  it("formata a duração", () => {
    expect(formatDuration(20_000)).toBe("menos de 1 min");
    expect(formatDuration(25 * 60_000)).toBe("25 min");
    expect(formatDuration(125 * 60_000)).toBe("2 h 5 min");
    expect(formatDuration(120 * 60_000)).toBe("2 h");
  });
});
