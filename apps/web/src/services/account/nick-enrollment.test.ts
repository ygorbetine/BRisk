import { describe, expect, it } from "vitest";
import type { PublicPeerIdentity } from "../offline/social-storage";
import type { KeyOwner } from "./account-service";
import { planAdminPromotions, planNickEnrollments, planNickInvite } from "./nick-enrollment";

const key = (seed: string): JsonWebKey => ({ kty: "EC", crv: "P-256", x: `${seed}-x`.padEnd(43, "x"), y: `${seed}-y`.padEnd(43, "y") });
const member = (peerId: string, seed: string, displayName = peerId): PublicPeerIdentity => ({ peerId, publicKey: key(seed), displayName });
const owner = (peerId: string, nick: string, seed: string): [string, KeyOwner] => [peerId, { peerId, nick, publicKey: key(seed) }];

const base = { maxMembers: 48 };

describe("planNickEnrollments", () => {
  it("inscreve instalação nova do mesmo @nick de um membro verificado", () => {
    const plan = planNickEnrollments({
      ...base,
      members: [member("dono-0001", "a"), member("veslei-old", "v1", "Veslei")],
      candidatePeerIds: ["veslei-new"],
      owners: new Map([owner("dono-0001", "becine", "a"), owner("veslei-old", "veslei", "v1"), owner("veslei-new", "veslei", "v2")]),
    });
    expect(plan).toHaveLength(1);
    expect(plan[0]).toMatchObject({ peerId: "veslei-new", displayName: "Veslei" });
    expect(plan[0]!.publicKey.x).toBe(key("v2").x);
  });

  it("ignora @nick que não é de nenhum membro", () => {
    const plan = planNickEnrollments({
      ...base,
      members: [member("dono-0001", "a")],
      candidatePeerIds: ["estranho"],
      owners: new Map([owner("dono-0001", "becine", "a"), owner("estranho", "intruso", "z")]),
    });
    expect(plan).toEqual([]);
  });

  it("não confia no @nick de um membro cuja chave local difere da registrada", () => {
    const plan = planNickEnrollments({
      ...base,
      members: [member("veslei-old", "v1")],
      candidatePeerIds: ["veslei-new"],
      owners: new Map([owner("veslei-old", "veslei", "OUTRA"), owner("veslei-new", "veslei", "v2")]),
    });
    expect(plan).toEqual([]);
  });

  it("não traz de volta quem foi removido", () => {
    const removed = member("veslei-bad", "v3");
    const plan = planNickEnrollments({
      ...base,
      members: [member("veslei-old", "v1")],
      removedPeerIds: ["veslei-bad"],
      removedMembers: [removed],
      candidatePeerIds: ["veslei-bad", "veslei-clone"],
      owners: new Map([owner("veslei-old", "veslei", "v1"), owner("veslei-bad", "veslei", "v3"), owner("veslei-clone", "veslei", "v3")]),
    });
    expect(plan).toEqual([]);
  });

  it("respeita o limite de membros e ignora quem já é membro", () => {
    const plan = planNickEnrollments({
      maxMembers: 3,
      members: [member("m1-00000", "a"), member("m2-00000", "b")],
      candidatePeerIds: ["m2-00000", "n1-00000", "n2-00000"],
      owners: new Map([owner("m1-00000", "um", "a"), owner("m2-00000", "dois", "b"), owner("n1-00000", "um", "c"), owner("n2-00000", "dois", "d")]),
    });
    expect(plan.map((item) => item.peerId)).toEqual(["n1-00000"]);
  });

  it("recusa chave do banco malformada", () => {
    const bad: KeyOwner = { peerId: "veslei-new", nick: "veslei", publicKey: { kty: "RSA" } };
    const plan = planNickEnrollments({
      ...base,
      members: [member("veslei-old", "v1")],
      candidatePeerIds: ["veslei-new"],
      owners: new Map([owner("veslei-old", "veslei", "v1"), ["veslei-new", bad]]),
    });
    expect(plan).toEqual([]);
  });
});

describe("planAdminPromotions", () => {
  const owners = (rows: Array<[string, string, string]>) => new Map(rows.map(([peerId, nick, seed]) => owner(peerId, nick, seed)));

  it("promove outra instalação do próprio dono (admin de reserva)", () => {
    const plan = planAdminPromotions({
      members: [member("dono-pc1-00", "a"), member("dono-pc2-00", "b")],
      ownerPeerId: "dono-pc1-00",
      administratorPeerIds: [],
      owners: owners([["dono-pc1-00", "becine", "a"], ["dono-pc2-00", "becine", "b"]]),
    });
    expect(plan).toEqual(["dono-pc2-00"]);
  });

  it("promove outra instalação de um admin existente", () => {
    const plan = planAdminPromotions({
      members: [member("dono-00000", "a"), member("adm-pc1-000", "b"), member("adm-pc2-000", "c")],
      ownerPeerId: "dono-00000",
      administratorPeerIds: ["adm-pc1-000"],
      owners: owners([["dono-00000", "becine", "a"], ["adm-pc1-000", "sticker", "b"], ["adm-pc2-000", "sticker", "c"]]),
    });
    expect(plan).toEqual(["adm-pc2-000"]);
  });

  it("não promove membro comum, removido, sem @nick ou com chave que não bate", () => {
    const plan = planAdminPromotions({
      members: [member("dono-00000", "a"), member("comum-0000", "b"), member("fake-00000", "c"), member("sem-nick-00", "d"), member("fora-00000", "e")],
      ownerPeerId: "dono-00000",
      administratorPeerIds: [],
      removedPeerIds: ["fora-00000"],
      owners: owners([["dono-00000", "becine", "a"], ["comum-0000", "veslei", "b"], ["fake-00000", "becine", "OUTRA"], ["fora-00000", "becine", "e"]]),
    });
    expect(plan).toEqual([]);
  });

  it("não promove ninguém se o @nick do dono não está verificado", () => {
    const plan = planAdminPromotions({
      members: [member("dono-pc1-00", "a"), member("dono-pc2-00", "b")],
      ownerPeerId: "dono-pc1-00",
      administratorPeerIds: [],
      owners: owners([["dono-pc1-00", "becine", "CHAVE-DIFERENTE"], ["dono-pc2-00", "becine", "b"]]),
    });
    expect(plan).toEqual([]);
  });
});

describe("planNickInvite", () => {
  const profile = { nick: "veslei", keys: [{ peerId: "veslei-pc1", publicKey: key("v1") }, { peerId: "veslei-pc2", publicKey: key("v2") }] };
  it("adiciona todas as instalações do @nick com o nick como nome", () => {
    const plan = planNickInvite({ profile, members: [member("dono-0001", "a")], maxMembers: 48 });
    expect(plan.add.map((item) => item.peerId)).toEqual(["veslei-pc1", "veslei-pc2"]);
    expect(plan.add[0]!.displayName).toBe("veslei");
  });
  it("pula quem já é membro, quem foi removido e respeita o limite", () => {
    const plan = planNickInvite({ profile, members: [member("veslei-pc1", "v1")], removedPeerIds: ["veslei-pc2"], maxMembers: 48 });
    expect(plan.add).toEqual([]);
    expect(plan.skipped).toBe(2);
    const limited = planNickInvite({ profile, members: [member("m1-00000", "a")], maxMembers: 2 });
    expect(limited.add).toHaveLength(1);
  });
  it("recusa chave malformada", () => {
    const bad = { nick: "x", keys: [{ peerId: "bad-peer-1", publicKey: { kty: "RSA" } as JsonWebKey }] };
    expect(planNickInvite({ profile: bad, members: [], maxMembers: 48 }).add).toEqual([]);
  });
});
