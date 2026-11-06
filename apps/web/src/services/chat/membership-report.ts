import type { MembershipReport } from "../../chat";

/** Explica em português o resultado de "Recarregar lista", comparando o antes e o depois. */
export function describeMembershipResync(before: MembershipReport, after: MembershipReport): string {
  const plural = (count: number, one: string, many: string) => `${count} ${count === 1 ? one : many}`;
  if (after.localIsAuthorized) {
    if (after.openPeers === 0) return "Você é dono/admin, mas ninguém está conectado a você neste grupo agora.";
    return `Lista reenviada para ${plural(after.openPeers, "pessoa", "pessoas")} (${plural(after.members, "membro", "membros")}).`;
  }
  if (after.openPeers === 0) {
    return "Ninguém está conectado a você neste grupo agora. O dono/admin precisa estar com o Briks aberto.";
  }
  if (!after.authorizedOnline) {
    return `Você está conectado a ${plural(after.openPeers, "pessoa", "pessoas")}, mas o dono/admin não está entre elas. Só ele pode enviar a lista.`;
  }
  const gotNew = after.received > before.received;
  if (gotNew && after.accepted > before.accepted) return `Lista atualizada (${plural(after.members, "membro", "membros")}).`;
  if (gotNew) return "A lista chegou mas foi recusada (versão ou assinatura diferente). Peça para todos usarem a mesma versão do Briks.";
  if (after.accepted > 0) return `Já estava atualizada (${plural(after.members, "membro", "membros")}).`;
  return "O dono/admin está conectado, mas a lista não chegou. Tente de novo em instantes.";
}
