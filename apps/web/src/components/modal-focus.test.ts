import { readFile } from "node:fs/promises";
import { describe, expect, it } from "vitest";

describe("foco do Modal", () => {
  it("não refaz o efeito de foco a cada renderização do pai", async () => {
    const source = await readFile(new URL("./Modal.tsx", import.meta.url), "utf8");
    // onClose novo a cada render não pode ser dependência: devolveria o foco ao botão e impediria digitar.
    expect(source).not.toMatch(/\},\s*\[onClose\]\)/);
    expect(source).toContain("closeRef.current()");
  });

  it("foca o primeiro campo antes do botão de fechar", async () => {
    const source = await readFile(new URL("./Modal.tsx", import.meta.url), "utf8");
    const inputIndex = source.indexOf('"input:not(:disabled)');
    const buttonIndex = source.indexOf('"button,[tabindex]');
    expect(inputIndex).toBeGreaterThan(-1);
    expect(buttonIndex).toBeGreaterThan(inputIndex);
  });
});
