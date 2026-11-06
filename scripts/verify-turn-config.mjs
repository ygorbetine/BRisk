import { readFile } from "node:fs/promises";
import { resolve } from "node:path";

const root = process.cwd();
const config = await readFile(resolve(root, "infrastructure/coturn/turnserver.conf"), "utf8");
const developmentConfig = await readFile(resolve(root, "infrastructure/coturn/turnserver.dev.conf"), "utf8");
const compose = await readFile(resolve(root, "infrastructure/coturn/compose.production.yml"), "utf8");
const entrypoint = await readFile(resolve(root, "infrastructure/coturn/briks-entrypoint.sh"), "utf8");

function requireLine(contents, line, source) {
  const lines = contents.split(/\r?\n/).map((value) => value.trim());
  if (!lines.includes(line)) throw new Error(`${source} não contém ${line}`);
}

for (const line of [
  "listening-port=3478",
  "tls-listening-port=5349",
  "alt-tls-listening-port=443",
  "use-auth-secret",
  "min-port=49152",
  "max-port=65535",
  "no-tcp-relay",
  "no-multicast-peers",
  "unauthorized-ratelimit",
  "no-dynamic-realms",
]) {
  requireLine(config, line, "turnserver.conf");
}

for (const value of [
  "coturn/coturn:4.17.0-alpine@sha256:8f80ed9d6867319340ca612b9d005aaea2444755aa4ce3dd79ddfb5f31f6e1b5",
  "network_mode: host",
  "file: ${TURN_SECRET_FILE:?Defina TURN_SECRET_FILE}",
  "/etc/coturn/briks-entrypoint.sh",
]) {
  if (!compose.includes(value)) throw new Error(`compose.production.yml não contém ${value}`);
}

if (/^\s*static-auth-secret\s*=\s*\S+/m.test(config)) {
  throw new Error("turnserver.conf não pode armazenar TURN_SECRET.");
}
if (compose.includes("static-auth-secret") || compose.includes("TURN_SECRET:")) {
  throw new Error("compose.production.yml não pode expor TURN_SECRET em argv ou environment.");
}
for (const value of ["static-auth-secret=%s", "/run/secrets/turn_secret", "umask 077", "proc-user=nobody", "proc-group=nogroup"]) {
  if (!entrypoint.includes(value)) throw new Error(`briks-entrypoint.sh não contém ${value}`);
}
if (/^\s*(tls-listening-port|alt-tls-listening-port)\s*=/m.test(developmentConfig)) {
  throw new Error("turnserver.dev.conf não deve exigir TLS.");
}

console.log("Configuração Coturn validada: UDP/TCP 3478, TLS 5349/443 e segredo externo.");
