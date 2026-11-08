import { useState, type CSSProperties } from "react";
import { normalizeHex } from "../services/theme/color";
import {
  DEFAULT_THEME,
  MAX_THEME_NAME,
  THEME_PRESETS,
  applyTheme,
  cleanThemeName,
  deleteCustomTheme,
  loadCustomThemes,
  loadTheme,
  sameColors,
  saveCustomTheme,
  saveTheme,
  type ThemeColors,
} from "../services/theme/theme";
import { TrianglePicker } from "./TrianglePicker";
import "./theme-panel.css";
import "./settings-ui.css";

const SLOTS: Array<{ key: keyof ThemeColors; label: string; position: number }> = [
  { key: "c1", label: "Cor 1 · início", position: 0 },
  { key: "c2", label: "Cor 2 · meio", position: 55 },
  { key: "c3", label: "Cor 3 · destaque", position: 100 },
];

const gradient = (colors: ThemeColors, angle = 135) => `linear-gradient(${angle}deg, ${colors.c1}, ${colors.c2} 55%, ${colors.c3})`;

export function ThemePanel() {
  const [colors, setColors] = useState<ThemeColors>(loadTheme);
  const [custom, setCustom] = useState(loadCustomThemes);
  const [slot, setSlot] = useState<keyof ThemeColors>("c1");
  const [hexDraft, setHexDraft] = useState<string | null>(null);
  const [editing, setEditing] = useState(false);
  const [name, setName] = useState("");
  const [savedName, setSavedName] = useState<string | null>(null);

  const commit = (next: ThemeColors) => {
    setColors(next);
    applyTheme(next);
    saveTheme(next);
  };
  const setColor = (hex: string) => commit({ ...colors, [slot]: hex });
  const currentSlot = SLOTS.find((item) => item.key === slot)!;
  const cleanName = cleanThemeName(name);

  function saveNamed(): void {
    if (!cleanName) return;
    setCustom(saveCustomTheme(cleanName, colors, custom));
    setSavedName(cleanName);
    setName("");
  }

  const preview = { "--p1": colors.c1, "--p2": colors.c2, "--p3": colors.c3 } as CSSProperties;

  return <section className="sx-stack theme-panel" aria-label="Temas">
    <div className="theme-live" style={preview} aria-hidden="true">
      <div className="theme-live-bar" style={{ background: gradient(colors, 90) }}/>
      <div className="theme-live-body">
        <span className="theme-live-line"/><span className="theme-live-line short"/>
        <span className="theme-live-button" style={{ background: gradient(colors) }}>Entrar na call</span>
      </div>
    </div>

    <section className="sx-card">
      <span className="sx-title">Predefinidos</span>
      <div className="theme-presets" role="group" aria-label="Predefinições">
        {THEME_PRESETS.map((preset) => <button
          key={preset.id}
          type="button"
          className={`theme-preset${sameColors(colors, preset.colors) ? " active" : ""}`}
          aria-pressed={sameColors(colors, preset.colors)}
          onClick={() => commit(preset.colors)}
          title={preset.name}
        >
          <span className="theme-swatch" style={{ background: gradient(preset.colors) }}/>
          <span>{preset.name}</span>
        </button>)}
      </div>
    </section>

    <section className="sx-card">
      <span className="sx-title">Personalizados</span>
      {custom.length === 0
        ? <p className="sx-sub">Você ainda não salvou nenhum tema. Monte o seu em Personalizar cores e dê um nome.</p>
        : <div className="theme-presets" role="group" aria-label="Temas personalizados">
          {custom.map((theme) => <span key={theme.id} className={`theme-preset has-remove${sameColors(colors, theme.colors) ? " active" : ""}`}>
            <button type="button" className="theme-preset-main" aria-pressed={sameColors(colors, theme.colors)} onClick={() => commit(theme.colors)} title={theme.name}>
              <span className="theme-swatch" style={{ background: gradient(theme.colors) }}/>
              <span>{theme.name}</span>
            </button>
            <button type="button" className="theme-remove" aria-label={`Apagar o tema ${theme.name}`} title="Apagar tema" onClick={() => setCustom(deleteCustomTheme(theme.id, custom))}>×</button>
          </span>)}
        </div>}
    </section>

    <section className="sx-card sx-fold">
      <button type="button" className="sx-fold-head" aria-expanded={editing} onClick={() => setEditing((open) => !open)}>
        <span>Personalizar cores <small>gradiente completo, com três cores</small></span>
        <span aria-hidden="true">{editing ? "⌃" : "⌄"}</span>
      </button>
      {editing && <>
        <div className="theme-gradient" role="group" aria-label="Gradiente do tema">
          <div className="theme-gradient-bar" style={{ background: gradient(colors, 90) }}>
            {SLOTS.map((item) => <button
              key={item.key}
              type="button"
              className={`theme-handle${slot === item.key ? " active" : ""}`}
              style={{ left: `${item.position}%`, background: colors[item.key] }}
              aria-label={item.label}
              aria-pressed={slot === item.key}
              onClick={() => { setSlot(item.key); setHexDraft(null); }}
            />)}
          </div>
          <div className="theme-gradient-labels"><span>Cor 1</span><span>Cor 2</span><span>Cor 3</span></div>
        </div>

        <div className="theme-editor">
          <TrianglePicker value={colors[slot]} onChange={setColor} label={currentSlot.label}/>
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
              Código hexadecimal de {(currentSlot.label.split(" · ")[0] ?? "cor").toLowerCase()}
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
          </div>
        </div>

        <div className="sx-divider"/>
        <form className="theme-save" onSubmit={(event) => { event.preventDefault(); saveNamed(); }}>
          <input className="sx-input" value={name} maxLength={MAX_THEME_NAME} placeholder="Nome do tema, por exemplo Noite" aria-label="Nome do tema" onChange={(event) => { setName(event.target.value); setSavedName(null); }}/>
          <button type="submit" className="sx-btn primary" disabled={!cleanName}>Salvar tema</button>
        </form>
        {savedName && <p className="sx-note" role="status"><b>Tema salvo</b> em Personalizados: {savedName}.</p>}
        <button type="button" className="sx-btn" onClick={() => commit(DEFAULT_THEME)}>Restaurar tema Briks</button>
      </>}
    </section>

    <p className="sx-note">A mudança vale na hora e fica salva neste computador.</p>
  </section>;
}
