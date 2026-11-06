import { useState } from "react";
import { normalizeHex } from "../services/theme/color";
import { DEFAULT_THEME, THEME_PRESETS, applyTheme, loadTheme, saveTheme, type ThemeColors } from "../services/theme/theme";
import { TrianglePicker } from "./TrianglePicker";
import "./theme-panel.css";

const SLOTS: Array<{ key: keyof ThemeColors; label: string }> = [
  { key: "c1", label: "Cor 1 · início do gradiente" },
  { key: "c2", label: "Cor 2 · meio" },
  { key: "c3", label: "Cor 3 · destaque" },
];

export function ThemePanel() {
  const [colors, setColors] = useState<ThemeColors>(loadTheme);
  const [slot, setSlot] = useState<keyof ThemeColors>("c1");
  const [hexDraft, setHexDraft] = useState<string | null>(null);

  const commit = (next: ThemeColors) => {
    setColors(next);
    applyTheme(next);
    saveTheme(next);
  };
  const setColor = (hex: string) => commit({ ...colors, [slot]: hex });

  return <section className="theme-panel" aria-label="Temas">
    <h2>Temas</h2>
    <p className="theme-hint">Escolha as cores do gradiente do Briks. A mudança vale na hora e fica salva neste computador.</p>

    <div className="theme-presets" role="group" aria-label="Predefinições">
      {THEME_PRESETS.map((preset) => <button
        key={preset.id}
        type="button"
        className="theme-preset"
        onClick={() => commit(preset.colors)}
        title={preset.name}
      >
        <span className="theme-swatch" style={{ background: `linear-gradient(135deg, ${preset.colors.c1}, ${preset.colors.c2} 55%, ${preset.colors.c3})` }}/>
        <span>{preset.name}</span>
      </button>)}
    </div>

    <div className="theme-editor">
      <TrianglePicker value={colors[slot]} onChange={setColor} label={SLOTS.find((item) => item.key === slot)!.label}/>
      <div className="theme-slots">
        {SLOTS.map((item) => <button
          key={item.key}
          type="button"
          className={`theme-slot${slot === item.key ? " active" : ""}`}
          onClick={() => { setSlot(item.key); setHexDraft(null); }}
        >
          <span className="theme-dot" style={{ background: colors[item.key] }}/>
          <span>{item.label}</span>
          <code>{colors[item.key]}</code>
        </button>)}
        <label className="theme-hex">
          Código hexadecimal
          <input
            value={hexDraft ?? colors[slot]}
            maxLength={7}
            spellCheck={false}
            onChange={(event) => {
              setHexDraft(event.target.value);
              const hex = normalizeHex(event.target.value);
              if (hex) commit({ ...colors, [slot]: hex });
            }}
            onBlur={() => setHexDraft(null)}
          />
        </label>
        <button type="button" className="theme-reset" onClick={() => commit(DEFAULT_THEME)}>Restaurar tema Briks</button>
      </div>
    </div>

    <div className="theme-preview" style={{ background: `linear-gradient(135deg, ${colors.c1}, ${colors.c2} 55%, ${colors.c3})` }}>Pré-visualização do gradiente</div>
  </section>;
}
