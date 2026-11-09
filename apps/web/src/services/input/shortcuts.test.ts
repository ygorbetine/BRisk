import { describe, expect, it } from "vitest";
import { acceleratorFromKey, formatAccelerator, isAllowedVk, isValidAccelerator, pttKeyFromKeyboard, pttKeyFromMouse } from "./shortcuts";

const key = (code: string, mods: Partial<{ ctrlKey: boolean; altKey: boolean; shiftKey: boolean; metaKey: boolean }> = {}) => ({ code, ctrlKey: false, altKey: false, shiftKey: false, metaKey: false, ...mods });

describe("atalhos globais", () => {
  it("monta o acelerador com modificadores", () => {
    expect(acceleratorFromKey(key("KeyM", { ctrlKey: true, shiftKey: true }))).toBe("Control+Shift+M");
    expect(acceleratorFromKey(key("Digit1", { altKey: true }))).toBe("Alt+1");
    expect(acceleratorFromKey(key("Numpad5", { ctrlKey: true }))).toBe("Control+num5");
  });
  it("recusa letra sem Ctrl/Alt/Win, mas aceita F1–F24 sozinhas", () => {
    expect(acceleratorFromKey(key("KeyM"))).toBeNull();
    expect(acceleratorFromKey(key("KeyM", { shiftKey: true }))).toBeNull();
    expect(acceleratorFromKey(key("F9"))).toBe("F9");
  });
  it("ignora tecla modificadora sozinha e teclas desconhecidas", () => {
    expect(acceleratorFromKey(key("ControlLeft", { ctrlKey: true }))).toBeNull();
    expect(acceleratorFromKey(key("MediaPlayPause", { ctrlKey: true }))).toBeNull();
  });
  it("valida aceleradores vindos do armazenamento", () => {
    expect(isValidAccelerator("Control+Shift+M")).toBe(true);
    expect(isValidAccelerator("Control+Shift+M; rm -rf")).toBe(false);
    expect(isValidAccelerator(42)).toBe(false);
  });
  it("formata para exibir", () => {
    expect(formatAccelerator("Control+Super+num3")).toBe("Ctrl + Win + Num 3");
    expect(formatAccelerator(null)).toBe("Sem atalho");
  });
});

describe("push-to-talk", () => {
  it("converte teclas no código do Windows", () => {
    expect(pttKeyFromKeyboard({ code: "KeyV" })).toEqual({ vk: 0x56, label: "V" });
    expect(pttKeyFromKeyboard({ code: "CapsLock" })).toEqual({ vk: 0x14, label: "Caps Lock" });
    expect(pttKeyFromKeyboard({ code: "F13" })).toEqual({ vk: 0x7c, label: "F13" });
    expect(pttKeyFromKeyboard({ code: "Numpad0" })?.vk).toBe(0x60);
    expect(pttKeyFromKeyboard({ code: "MetaLeft" })).toBeNull();
  });
  it("aceita só os botões laterais e do meio do mouse", () => {
    expect(pttKeyFromMouse(3)).toEqual({ vk: 0x05, label: "Mouse 4" });
    expect(pttKeyFromMouse(0)).toBeNull();
    expect(pttKeyFromMouse(2)).toBeNull();
  });
  it("recusa os botões esquerdo/direito e as teclas Windows", () => {
    expect(isAllowedVk(0x01)).toBe(false);
    expect(isAllowedVk(0x02)).toBe(false);
    expect(isAllowedVk(0x5b)).toBe(false);
    expect(isAllowedVk(0x41)).toBe(true);
  });
});
