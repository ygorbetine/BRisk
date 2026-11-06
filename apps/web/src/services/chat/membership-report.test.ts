import { describe, expect, it } from "vitest";
import type { MembershipReport } from "../../chat";
import { describeMembershipResync } from "./membership-report";

const base: MembershipReport = {
  status: "ready", openPeers: 1, authorizedOnline: true, localIsAuthorized: false,
  received: 0, accepted: 0, rejected: 0, members: 3,
};

describe("describeMembershipResync", () => {
  it("avisa quando ninguém está conectado", () => {
    expect(describeMembershipResync(base, { ...base, openPeers: 0, authorizedOnline: false })).toContain("Ninguém está conectado");
  });
  it("avisa quando o dono/admin não está online", () => {
    expect(describeMembershipResync(base, { ...base, authorizedOnline: false })).toContain("dono/admin não está entre elas");
  });
  it("confirma lista nova aceita", () => {
    expect(describeMembershipResync(base, { ...base, received: 1, accepted: 1 })).toBe("Lista atualizada (3 membros).");
  });
  it("explica lista recusada", () => {
    expect(describeMembershipResync(base, { ...base, received: 1, rejected: 1 })).toContain("recusada");
  });
  it("explica lista que não chegou", () => {
    expect(describeMembershipResync(base, base)).toContain("não chegou");
  });
  it("dono/admin vê o reenvio", () => {
    expect(describeMembershipResync(base, { ...base, localIsAuthorized: true, openPeers: 2 })).toContain("2 pessoas");
  });
});
