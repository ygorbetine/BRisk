import { describe, expect, it } from "vitest";
import { filterFriends } from "./friends-filter";

const friends = [{ id: "a", displayName: "Zeca" }, { id: "b", displayName: "Ana" }, { id: "c", displayName: "Beto" }];

describe("filterFriends", () => {
  it("online mostra só quem está online", () => {
    expect(filterFriends(friends, new Set(["c"]), "online", "").map((f) => f.id)).toEqual(["c"]);
  });
  it("todos coloca online primeiro e ordena por nome", () => {
    expect(filterFriends(friends, new Set(["a"]), "all", "").map((f) => f.id)).toEqual(["a", "b", "c"]);
  });
  it("busca por nome e pendentes não lista amigos", () => {
    expect(filterFriends(friends, undefined, "all", "an").map((f) => f.id)).toEqual(["b"]);
    expect(filterFriends(friends, undefined, "pending", "")).toEqual([]);
  });
});
