import { useEffect, useState } from "react";
import { HardDrive } from "lucide-react";

export function LocalStoragePanel() {
  const [usage, setUsage] = useState<number>();
  const [quota, setQuota] = useState<number>();
  useEffect(() => {
    let alive = true;
    void navigator.storage?.estimate().then((estimate) => {
      if (!alive) return;
      setUsage(estimate.usage);
      setQuota(estimate.quota);
    });
    return () => { alive = false; };
  }, []);
  return <section className="local-storage-panel"><h3><HardDrive/> Armazenamento local</h3><p>Mensagens, grupos e anexos permanecem somente neste dispositivo.</p><div><span>Em uso</span><strong>{formatBytes(usage)}</strong></div><div><span>Disponível para o BRisk</span><strong>{formatBytes(quota)}</strong></div></section>;
}

function formatBytes(value?: number): string {
  if (value === undefined) return "Calculando…";
  if (value < 1024 ** 2) return `${Math.round(value / 1024)} KiB`;
  if (value < 1024 ** 3) return `${(value / 1024 ** 2).toFixed(1)} MiB`;
  return `${(value / 1024 ** 3).toFixed(1)} GiB`;
}
