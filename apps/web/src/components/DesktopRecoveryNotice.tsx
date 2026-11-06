import { useEffect, useState } from "react";
import { useBriksApplication } from "../application/ApplicationContext";

export function DesktopRecoveryNotice() {
  const application = useBriksApplication();
  const [status, setStatus] = useState<BriksDesktopBackendStatus>();

  useEffect(() => window.desktop?.onBackendStatus?.((next) => {
    application.backendChanged();
    setStatus(next);
  }), [application]);

  useEffect(() => {
    if (status?.state !== "recovered") return undefined;
    const timer = window.setTimeout(() => setStatus(undefined), 6000);
    return () => window.clearTimeout(timer);
  }, [status]);

  if (!status) return null;
  return <button
    className={`desktop-recovery-notice ${status.state}`}
    onClick={() => setStatus(undefined)}
    title="Clique para fechar"
  >{status.message}</button>;
}
