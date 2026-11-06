import { useState } from "react";
import { LocalStoragePanel } from "../components/LocalStoragePanel";
import { NetworkSettingsPanel } from "../components/NetworkSettingsPanel";
import { AboutPanel } from "../components/AboutPanel";
import { ThemePanel } from "../components/ThemePanel";
import { VoiceVideoSettingsPanel } from "../components/VoiceVideoSettingsPanel";

export function SettingsView() {
  const [tab, setTab] = useState<"voice" | "network" | "theme" | "storage" | "about">("voice");
  return <>
    <nav className="settings-tabs" aria-label="Seções das configurações">
      <button className={tab === "voice" ? "active" : ""} onClick={() => setTab("voice")}>Voz e vídeo</button>
      <button className={tab === "network" ? "active" : ""} onClick={() => setTab("network")}>Rede</button>
      <button className={tab === "theme" ? "active" : ""} onClick={() => setTab("theme")}>Temas</button>
      <button className={tab === "storage" ? "active" : ""} onClick={() => setTab("storage")}>Armazenamento</button>
      <button className={tab === "about" ? "active" : ""} onClick={() => setTab("about")}>Sobre</button>
    </nav>
    {tab === "voice" && <VoiceVideoSettingsPanel/>}
    {tab === "network" && <NetworkSettingsPanel/>}
    {tab === "theme" && <ThemePanel/>}
    {tab === "storage" && <LocalStoragePanel/>}
    {tab === "about" && <AboutPanel/>}
  </>;
}
