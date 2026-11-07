import { useState } from "react";
import { useUpdateState } from "./useUpdateState";

/** Aviso discreto na tela principal quando uma atualização está pronta para instalar. */
export function UpdateBanner() {
  const state = useUpdateState();
  const [dismissedVersion, setDismissedVersion] = useState<string | undefined>();
  if (state.phase !== "ready" || dismissedVersion === state.version) return null;
  return <div className="update-banner" role="status">
    <span>Atualização {state.version} pronta para instalar.</span>
    <button type="button" onClick={() => void window.desktop?.installUpdate?.()}>Reiniciar e instalar</button>
    <button type="button" className="update-banner-later" aria-label="Dispensar" onClick={() => setDismissedVersion(state.version)}>Depois</button>
  </div>;
}
