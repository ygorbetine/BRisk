/** Traduz falhas de login/cadastro em mensagens que dizem o que fazer. */
export function friendlyAuthError(cause: unknown, register: boolean): string {
  const status = typeof cause === "object" && cause && "status" in cause ? Number((cause as { status: unknown }).status) : 0;
  const message = cause instanceof Error ? cause.message : "";
  if (cause instanceof TypeError || /failed to fetch|network/i.test(message)) {
    return "Não consegui falar com o servidor. Verifique sua internet e tente de novo.";
  }
  if (status === 401 || /n[aã]o autorizado|unauthorized|credenciais/i.test(message)) {
    return register
      ? "Não foi possível criar a conta com esses dados."
      : "E-mail ou senha incorretos. Confira os dados; se a conta é de uma versão antiga do BRisk, crie uma conta nova.";
  }
  if (status === 409 || /j[aá] (existe|cadastrad)/i.test(message)) return "Já existe uma conta com esse e-mail. Entre com ela ou use outro e-mail.";
  if (status === 429) return "Muitas tentativas. Aguarde um minuto e tente de novo.";
  if (status >= 500) return "O servidor está com problema agora. Tente novamente em instantes.";
  return message || "Não foi possível entrar. Tente de novo.";
}
