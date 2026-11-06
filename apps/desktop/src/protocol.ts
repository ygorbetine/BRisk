import { net, protocol } from "electron";
import path from "node:path";
import { pathToFileURL } from "node:url";

export const PACKAGED_SCHEME = "risk";
export const PACKAGED_HOST = "app";
export const PACKAGED_ORIGIN = `${PACKAGED_SCHEME}://${PACKAGED_HOST}`;
export const PACKAGED_ENTRY_URL = `${PACKAGED_ORIGIN}/index.html`;

export function registerBriksScheme(): void {
  // Uma origem estável preserva IndexedDB/localStorage e a identidade ECDSA.
  protocol.registerSchemesAsPrivileged([
    {
      scheme: PACKAGED_SCHEME,
      privileges: {
        standard: true,
        secure: true,
        supportFetchAPI: true,
        corsEnabled: true,
        stream: true,
        codeCache: true,
      },
    },
  ]);
}

export async function registerPackagedProtocol(webRoot: string): Promise<void> {
  await protocol.handle(PACKAGED_SCHEME, async (request) => {
    try {
      const requestUrl = new URL(request.url);
      if (requestUrl.host !== PACKAGED_HOST) return new Response("Not found", { status: 404 });

      const decodedPath = decodeURIComponent(requestUrl.pathname);
      const relativePath = decodedPath === "/" ? "index.html" : decodedPath.replace(/^\/+/, "");
      const filePath = path.resolve(webRoot, relativePath);
      const relative = path.relative(webRoot, filePath);
      if (relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
        return new Response("Forbidden", { status: 403 });
      }

      return await net.fetch(pathToFileURL(filePath).toString());
    } catch (error) {
      console.warn("Falha ao servir recurso do bundle Briks", { url: request.url, error });
      return new Response("Not found", { status: 404 });
    }
  });
}
