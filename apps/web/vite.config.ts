import http from "node:http";
import { readFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { defineConfig, loadEnv, type Plugin } from "vite";
import react from "@vitejs/plugin-react";

const envDir = fileURLToPath(new URL("../../", import.meta.url));
const devBackendBridgeFile = fileURLToPath(new URL("../../.briks/dev-backend.json", import.meta.url));
const DEV_API_PREFIX = "/__briks-api";
const buildVersion = (() => {
  try { return String(JSON.parse(readFileSync(fileURLToPath(new URL("../desktop/package.json", import.meta.url)), "utf8")).version); } catch { return "dev"; }
})();
const webVersion = process.env.npm_package_version?.trim() || "0.2.1";

type DevBackendBridge = {
  baseUrl: string;
  token: string;
};

function isLoopback(address: string | undefined): boolean {
  return address === "127.0.0.1" || address === "::1" || address === "::ffff:127.0.0.1";
}

async function readDevBackendBridge(): Promise<DevBackendBridge> {
  const value = JSON.parse(await readFile(devBackendBridgeFile, "utf8")) as Partial<DevBackendBridge>;
  if (typeof value.baseUrl !== "string" || typeof value.token !== "string" || value.token.length < 32) {
    throw new Error("Configuração temporária do backend desktop inválida.");
  }
  const url = new URL(value.baseUrl);
  if (url.protocol !== "http:" || url.hostname !== "127.0.0.1") {
    throw new Error("O backend de desenvolvimento precisa estar no loopback.");
  }
  return { baseUrl: value.baseUrl.replace(/\/$/, ""), token: value.token };
}

function sendJson(response: import("node:http").ServerResponse, status: number, body: object): void {
  const payload = JSON.stringify(body);
  response.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(payload),
    "cache-control": "no-store",
  });
  response.end(payload);
}

function devBackendProxy(): Plugin {
  return {
    name: "briks-dev-backend-proxy",
    apply: "serve",
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const requestUrl = request.url ?? "";
        if (!(requestUrl === DEV_API_PREFIX || requestUrl.startsWith(`${DEV_API_PREFIX}/`) || requestUrl.startsWith(`${DEV_API_PREFIX}?`))) {
          next();
          return;
        }

        // O Vite fica exposto em 0.0.0.0 para testes de UI, mas o proxy autenticado
        // nunca deve transformar a máquina do desenvolvedor em uma ponte de LAN para o sidecar.
        if (!isLoopback(request.socket.remoteAddress)) {
          sendJson(response, 403, { message: "O proxy do backend desktop só está disponível no localhost." });
          return;
        }

        void (async () => {
          let bridge: DevBackendBridge;
          try {
            bridge = await readDevBackendBridge();
          } catch {
            sendJson(response, 503, {
              message: "Backend desktop ainda não está disponível. Inicie com `pnpm dev:desktop` e aguarde o readiness.",
            });
            return;
          }

          const suffix = requestUrl.slice(DEV_API_PREFIX.length) || "/";
          const target = new URL(suffix.startsWith("/") ? suffix : `/${suffix}`, `${bridge.baseUrl}/`);
          const headers = { ...request.headers };
          delete headers.host;
          delete headers.origin;
          delete headers.referer;
          delete headers.cookie;
          delete headers["x-briks-desktop-token"];
          headers.host = target.host;
          headers["x-briks-desktop-token"] = bridge.token;

          const upstream = http.request(target, {
            method: request.method,
            headers,
          }, (upstreamResponse) => {
            response.statusCode = upstreamResponse.statusCode ?? 502;
            for (const [name, value] of Object.entries(upstreamResponse.headers)) {
              if (value !== undefined) response.setHeader(name, value);
            }
            upstreamResponse.pipe(response);
          });

          upstream.on("error", () => {
            if (!response.headersSent) {
              sendJson(response, 502, { message: "Não foi possível alcançar o backend desktop local." });
            } else {
              response.destroy();
            }
          });
          request.on("aborted", () => upstream.destroy());
          request.pipe(upstream);
        })().catch(() => {
          if (!response.headersSent) sendJson(response, 500, { message: "Falha no proxy local de desenvolvimento." });
          else response.destroy();
        });
      });
    },
  };
}

export default defineConfig(({ command, mode }) => {
  const devApiUrl = process.env.BRIKS_DEV_API_URL?.trim() || DEV_API_PREFIX;
  const env = loadEnv(mode, envDir, "VITE_");
  const realtimeSources: string[] = [];
  const turnCredentialSources: string[] = [];
  try {
    const url = new URL(env.VITE_SUPABASE_URL ?? "");
    realtimeSources.push(url.origin, `${url.protocol === "https:" ? "wss:" : "ws:"}//${url.host}`);
  } catch { /* validação de configuração apresenta o erro amigável no app */ }
  try {
    // LiveKit (SFU): a CSP precisa liberar o websocket de sinalização e o HTTPS de validação.
    const sfu = new URL(env.VITE_LIVEKIT_URL ?? "");
    realtimeSources.push(`wss://${sfu.host}`, `https://${sfu.host}`);
  } catch { /* SFU não configurado */ }
  try {
    turnCredentialSources.push(new URL(env.VITE_TURN_CREDENTIALS_URL ?? "").origin);
  } catch { /* verify-build-env apresenta o erro antes do empacotamento */ }
  const connectSources = ["'self'", "http://127.0.0.1:*", ...(command === "serve" ? ["http://localhost:*", "ws://127.0.0.1:*", "ws://localhost:*"] : []), ...realtimeSources, ...turnCredentialSources].join(" ");
  return {
    base: "./",
    envDir,
    // Em desenvolvimento o Vite injeta CSS em <style>. Um nonce permite esses
    // estilos sem enfraquecer a CSP com style-src 'unsafe-inline'. O runtime do
    // Vite lê o mesmo nonce da meta csp-nonce gerada automaticamente.
    html: { cspNonce: "briks-vite-style" },
    plugins: [react(), devBackendProxy(), { name: "briks-csp", transformIndexHtml: (html) => html
      .replace("__BRIKS_CONNECT_SRC__", connectSources)
      .replace("__BRIKS_STYLE_SRC__", command === "serve" ? "'self' 'nonce-briks-vite-style'" : "'self'") }],
    // Durante `vite serve`, o navegador usa o proxy local por padrão mesmo que um
    // .env antigo ainda contenha VITE_API_URL=http://localhost:8080. Para testar
    // deliberadamente outra API em desenvolvimento, use BRIKS_DEV_API_URL.
    define: {
      "import.meta.env.VITE_BRIKS_APP_VERSION": JSON.stringify(webVersion),
      "import.meta.env.VITE_BRIKS_BUILD_VERSION": JSON.stringify(buildVersion),
      ...(command === "serve" ? { "import.meta.env.VITE_API_URL": JSON.stringify(devApiUrl) } : {}),
    },
    server: { port: 5173 },
    // Os testes não dependem do .env local (SFU/TURN configurados na máquina de quem desenvolve).
    ...({ test: { env: { VITE_LIVEKIT_URL: "", VITE_LIVEKIT_TOKEN_URL: "" } } } as object),
    build: {
      // O worklet de áudio por processo precisa ser um arquivo real: a CSP bloqueia worklets data: URI.
      assetsInlineLimit: (file) => (file.includes("process-pcm-worklet") ? false : undefined),
      rollupOptions: {
        output: {
          manualChunks(id) {
            if (!id.includes("node_modules")) return undefined;
            if (id.includes("@supabase")) return "vendor-supabase";
            if (id.includes("react") || id.includes("zustand")) return "vendor-react";
            if (id.includes("lucide-react")) return "vendor-icons";
            if (id.includes("noise-suppressor")) return "vendor-audio";
            return "vendor";
          },
        },
      },
    },
  };
});
