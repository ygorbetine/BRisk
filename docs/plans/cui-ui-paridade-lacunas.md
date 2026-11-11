# Lacunas de paridade da interface (CUI x Electron)

Este arquivo era citado em `plano-pos-1.3.0.md` como a lista de paridade, mas não existia no `brisk-dev`. A lista passou a ser gerada a partir do inventário completo:

- **Lista priorizada de lacunas (AUSENTE e SEM-EFEITO), com a adaptação ao layout novo de cada uma:** `docs/plans/paridade-electron-cui.md`.
- **Fonte da verdade (uma linha por item):** `apps/desktop-cui/scripts/paridade/itens.csv`.
- **Verificação automática oculta:** `scripts/paridade.ps1` (relatório PASSOU / FALHOU / AUSENTE por item).

Acompanhamento: ao entregar uma lacuna, mude a classe do item em `itens.csv`, acrescente o roteiro e a checagem em `apps/desktop-cui/scripts/paridade/`, rode `scripts/paridade.ps1` e regenere a página (a sonda do item acusa REVER enquanto a classe antiga estiver no CSV).
