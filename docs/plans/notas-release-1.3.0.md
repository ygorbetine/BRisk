# Briks 1.3.0 — app nativo

Esta versão troca o app Electron pelo app nativo (C + Rust, sem Chromium). Quem está no 1.2.x recebe este instalador como atualização.

## O que há
- Entrar com Google, Discord ou e-mail (Supabase Auth); perfil com foto, nome e @nick; cópia cifrada da identidade recuperável por frase de 12 palavras.
- Amigos, grupos e chat P2P assinado, com reações, resposta, edição, exclusão e mensagens fixadas.
- Chamadas de voz P2P com mudo, ensurdecer, expulsar (dono da sala) e presença do programa em uso.
- **Compartilhar tela/janela (BETA):** escolha de fonte, áudio por aplicativo, qualidade e adaptação à rede. Pode ter falhas; avise o que encontrar.
- Temas e gradientes, modos de desempenho, painéis arrastáveis, Configurações em 13 seções.
- Atualização automática ligada (verifica a cada 4 h; instala ao fechar). Desligue com `BRIKS_DISABLE_UPDATE=1`.

## Conexão (importante)
- Por padrão usa STUN públicos (Google e Cloudflare). **Sem TURN, redes restritivas (NAT simétrico/CGNAT) podem não conectar.**
- TURN por configuração: variável `BRIKS_ICE_SERVERS_JSON` ou arquivo `ice.json` na pasta de dados (`%APPDATA%\Briks-CUI`), por exemplo `[{"urls":"turn:seu-servidor:3478","username":"u","credential":"c"}]`. Um servidor TURN próprio precisa existir (há um compose em `infrastructure/coturn`). Nenhuma credencial vem embutida.

## O que NÃO existe nesta versão
- Câmera (botão "em breve"); vários controles de Configurações salvam mas ainda não têm efeito (marcados "em breve").
- Identidade P2P do Electron não é importada (nova identidade); mensagens e contatos locais do Briks antigo podem ser importados pela aba Conta. Os dados do Electron em `%APPDATA%\@briks` não são apagados.
- Instalador sem assinatura de código: o Windows SmartScreen pode avisar.

## Como testar
- **Login:** abra o Briks, "Continuar com Google" ou "Discord"; o navegador abre e volta sozinho. Confira foto e @nick no rodapé.
- **Transmissão:** entre numa sala de voz com outra pessoa, clique em "Compartilhar (beta)", escolha tela ou janela, confirme; a outra pessoa abre a aba "Transmissão".
- Problemas: informe versão (Configurações > Sobre), rede (Rede > Diagnóstico) e o que aconteceu.
