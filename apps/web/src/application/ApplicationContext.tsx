import { createContext, useContext, type ReactNode } from "react";
import type { BriksApplication } from "./BriksApplication";

const Context = createContext<BriksApplication | null>(null);

export function BriksApplicationProvider({ application, children }: {
  application: BriksApplication;
  children: ReactNode;
}) {
  return <Context.Provider value={application}>{children}</Context.Provider>;
}

export function useBriksApplication(): BriksApplication {
  const application = useContext(Context);
  if (!application) throw new Error("BriksApplicationProvider não foi configurado.");
  return application;
}
