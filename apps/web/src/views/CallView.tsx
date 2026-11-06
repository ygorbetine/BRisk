import { useBriksApplication } from "../application/ApplicationContext";
import { CallWorkspace } from "../components/CallWorkspace";
import { useCallStore } from "../store";

export function CallView() {
  const { call, callChat } = useBriksApplication();
  const setCallWorkspaceOpen = useCallStore((state) => state.setCallWorkspaceOpen);
  return <CallWorkspace call={call} chat={callChat} onMinimize={() => setCallWorkspaceOpen(false)}/>;
}
