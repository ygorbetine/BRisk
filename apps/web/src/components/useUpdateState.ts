import { useEffect, useState } from "react";

/** Estado da atualização automática do app desktop (idle quando não há desktop). */
export function useUpdateState(): BriksUpdateState {
  const [state, setState] = useState<BriksUpdateState>({ phase: "idle" });
  useEffect(() => {
    const desktop = window.desktop;
    if (!desktop?.onUpdateState) return;
    let active = true;
    void desktop.getUpdateState?.().then((current) => { if (active) setState(current); }).catch(() => undefined);
    const off = desktop.onUpdateState((next) => setState(next));
    return () => { active = false; off(); };
  }, []);
  return state;
}
