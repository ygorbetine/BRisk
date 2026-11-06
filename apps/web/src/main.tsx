import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { BriksApplicationProvider } from "./application/ApplicationContext";
import { briksApplication } from "./application/runtime";
import { AppErrorBoundary } from "./components/AppErrorBoundary";
import { DesktopRecoveryNotice } from "./components/DesktopRecoveryNotice";
import "./styles.css";
import "./components/modal-layout.css";

createRoot(document.getElementById("root")!).render(<React.StrictMode>
  <BriksApplicationProvider application={briksApplication}>
    <AppErrorBoundary><App/></AppErrorBoundary>
    <DesktopRecoveryNotice/>
  </BriksApplicationProvider>
</React.StrictMode>);
