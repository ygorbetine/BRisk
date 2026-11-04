import { app } from "electron";
import { accessSync, constants as fsConstants, existsSync, readFileSync } from "node:fs";

export function configurePlatformRuntime(): void {
  if (process.platform === "linux") {
    // pipewire-pulse pode expor o PID do daemon em vez do PID do cliente.
    process.env["PULSE_PROP_application.name"] = "BRisk";
    process.env["PULSE_PROP_application.id"] = "com.brisk.calls";
  }

  if (shouldUseLinuxSoftwareRendering()) {
    app.disableHardwareAcceleration();
    app.commandLine.appendSwitch("disable-features", "VaapiVideoDecoder,VaapiVideoEncoder");
    console.info("[desktop] GPU Linux incompatível ou inacessível; usando renderização por software.");
  }

  if (process.platform === "win32" && process.env.BRISK_DISABLE_GPU_ENCODE !== "1") {
    // Prefere a codificação/decodificação por hardware (NVENC/AMF/QuickSync via Media Foundation)
    // para não competir com o jogo pela CPU durante a transmissão.
    app.commandLine.appendSwitch("ignore-gpu-blocklist");
    app.commandLine.appendSwitch("enable-gpu-rasterization");
    app.commandLine.appendSwitch("enable-zero-copy");
  }

  app.commandLine.appendSwitch("autoplay-policy", "no-user-gesture-required");
}

export function shouldUseLinuxSoftwareRendering(): boolean {
  if (process.platform !== "linux" || process.env.RISK_FORCE_GPU === "1") return false;
  if (process.env.RISK_DISABLE_GPU === "1") return true;

  const gbmDrivers = [
    "/usr/lib/gbm/dri_gbm.so",
    "/usr/lib64/gbm/dri_gbm.so",
    "/usr/lib/x86_64-linux-gnu/gbm/dri_gbm.so",
  ];
  const hasUnreadableGbmDriver = gbmDrivers.some((file) => {
    if (!existsSync(file)) return false;
    try {
      accessSync(file, fsConstants.R_OK);
      return false;
    } catch {
      return true;
    }
  });
  if (hasUnreadableGbmDriver) return true;

  const identity = [
    "/sys/class/dmi/id/sys_vendor",
    "/sys/class/dmi/id/product_name",
    "/sys/class/dmi/id/board_vendor",
  ].map((file) => {
    try { return readFileSync(file, "utf8"); }
    catch { return ""; }
  }).join(" ").toLocaleLowerCase().replace(/\s+/g, " ");

  return [
    "vmware",
    "virtualbox",
    "qemu",
    "kvm",
    "hyper-v",
    "microsoft corporation virtual machine",
    "parallels",
    "bochs",
    "xen",
  ].some((marker) => identity.includes(marker));
}
