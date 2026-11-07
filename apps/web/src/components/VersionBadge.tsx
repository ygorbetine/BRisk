const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

/** Versão discreta no canto, visível em todas as telas. */
export function VersionBadge() {
  return <div className="version-badge" aria-hidden="true">v{version}</div>;
}
