import type { BriksGateway } from "./contracts";

/** Coordena restauração e encerramento sem espalhar estado global pela UI. */
export class SessionService {
  private restoreInFlight?: Promise<string | null>;
  private restoreSuppressed = false;
  private restoreGeneration = 0;

  constructor(private readonly gateway: Pick<BriksGateway, "refresh" | "logout">) {}

  get isRestoreSuppressed(): boolean { return this.restoreSuppressed; }

  restore(): Promise<string | null> {
    if (this.restoreSuppressed) return Promise.resolve(null);
    if (!this.restoreInFlight) {
      const generation = this.restoreGeneration;
      const restore = this.gateway.refresh()
        .then((result) => (
          !this.restoreSuppressed && generation === this.restoreGeneration
            ? result.accessToken
            : null
        ))
        .catch(() => null)
        .finally(() => {
          if (this.restoreInFlight === restore) this.restoreInFlight = undefined;
        });
      this.restoreInFlight = restore;
    }
    return this.restoreInFlight;
  }

  allowRestore(): void {
    this.restoreGeneration += 1;
    this.restoreSuppressed = false;
  }

  suppressRestore(): void {
    this.restoreGeneration += 1;
    this.restoreSuppressed = true;
    this.restoreInFlight = undefined;
  }

  async logout(): Promise<void> {
    this.suppressRestore();
    await this.gateway.logout();
  }
}
