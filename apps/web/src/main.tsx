import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { BriksApplicationProvider } from "./application/ApplicationContext";
import { briksApplication } from "./application/runtime";
import { AppErrorBoundary } from "./components/AppErrorBoundary";
import { VersionBadge } from "./components/VersionBadge";
import { DesktopRecoveryNotice } from "./components/DesktopRecoveryNotice";
import { applyStoredTheme } from "./services/theme/theme";
import "./styles.css";
import "./components/modal-layout.css";
import "./glass-layout.css";

applyStoredTheme();

createRoot(document.getElementById("root")!).render(<React.StrictMode>
  <BriksApplicationProvider application={briksApplication}>
    <AppErrorBoundary><App/></AppErrorBoundary>
    <DesktopRecoveryNotice/>
    <VersionBadge/>
  </BriksApplicationProvider>
</React.StrictMode>);
