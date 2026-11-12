# Briks 1.4.0 — app nativo

Segunda versão do app nativo. Traz a paridade com o Briks antigo (Electron) e o servidor de chamadas opcional.

## O que há
- **Conta e amigos:** entrar com Google, Discord ou e-mail (com @nick e data de nascimento no cadastro); foto, nome e @nick; abas Todos/Online/Pendentes/Bloqueados/Adicionar/Atividade; busca; adicionar amigo por código; fotos dos amigos.
- **Grupos:** canais de texto e salas de voz, cargos (dono/administrador), membros, convite por código, personalizar, sair e apagar.
- **Mensagens:** reações, resposta, edição, exclusão, fixadas, anexos (arrastar, colar imagem, galeria), prévia de link, texto formatado, busca (Ctrl+F) e troca rápida (Ctrl+K), estados de entrega, "digitando", histórico sincronizado entre os aparelhos da conversa, menu de contexto e seleção de texto.
- **Chamada:** grade/foco, tela cheia (F11), chat da chamada, pedir que alguém transmita, volume por pessoa, histórico de chamadas real, painel "Voz conectada", ajustes de áudio.
- **Voz e atalhos:** escolha de microfone e saída, teste de microfone, porta de ruído, cancelamento de eco, atalhos globais e apertar para falar; avisos em português quando o microfone está negado (com atalho para a Privacidade do Windows), ausente ou em uso; o áudio reabre sozinho ao trocar de dispositivo.
- **Câmera (BETA):** ligar/desligar na chamada, câmera dos outros e prévia em Configurações; segunda tela e zoom na transmissão.
- **Sistema:** bandeja (fechar mantém a chamada), instância única, lembrar a janela, notificações e sons, iniciar com o Windows, links `briks://mensagem`.
- **Acessibilidade:** tamanho do texto ajustável (Ctrl + / Ctrl −), aviso de offline, licenças de terceiros dentro do app.
- **Transmissão de tela (BETA)** e **servidor de chamadas SFU (BETA)**: ver abaixo.

## Conexão
- **Padrão:** conexão direta (P2P) com STUN públicos. Funciona na maioria das redes; em redes restritivas (CGNAT, empresa, 4G) pode não conectar.
- **TURN (opcional):** se o administrador publicar a função `turn-credentials` e subir um Coturn, cada conta recebe o relay sozinha depois do login (Configurações > Rede mostra "TURN da sua conta"). Também dá para preencher um TURN próprio na mesma tela. Guia: `docs/ativar-turn.md`.
- **SFU LiveKit (opcional, BETA):** com um servidor LiveKit e a função `livekit-token`, as chamadas passam pelo servidor (melhor com muita gente). Hoje é preciso o arquivo `livekit.json` na pasta de dados de cada usuário; ele ainda não vai no instalador. Guia: `docs/ativar-sfu.md`.
- Sem TURN e sem SFU configurados, nada muda em relação à 1.3.0.

## Beta: o que esperar
- **Transmissão:** pode falhar ou perder qualidade em redes ruins; sem GPU compatível usa codificação por software.
- **SFU:** se o token não sair (sem conta, Supabase ou LiveKit fora do ar) a chamada não abre e mostra o motivo; não cai em P2P sozinha.

## O que NÃO há
- Quem está em cada sala de voz do grupo sem entrar nela; modo da chamada e preferência de conexão em Configurações > Rede ainda sem efeito completo.
- Tema claro, outros idiomas, leitor de tela.
- `livekit.json` embutido no instalador; convites `briks://friend` e `briks://group`.
- Identidade P2P do Briks antigo (Electron) não é importada.
- Instalador sem assinatura de código (o SmartScreen pode avisar).

## Limites conhecidos
- **Transmissão:** em queda brusca de banda a imagem pode congelar por mais de 3 s antes de se recuperar.
- **Câmera em perda de pacotes:** com rede muito ruim o bitrate da câmera cai menos do que deveria (o estimador tolera perda aleatória); a imagem continua chegando e nunca passa de 360p. O teste `camera_adapts_to_loss_and_never_exceeds_360p` está ignorado até a afinação (1.4.1).
- **TURN na interface** (preencher TURN em Configurações > Rede): fica para depois; por enquanto só pela função `turn-credentials` ou arquivo.
- **Microfone e troca de dispositivo (hot-plug):** só testados com dispositivo falso (`BRIKS_FAKE_MIC`), não com hardware real.
- A dica "Sair" do rodapé foi removida (o botão não tem mais tooltip).
- Em diálogos com campo de texto, o primeiro Esc tira o foco do campo e o segundo fecha.
- Ao desligar pelo painel "Voz conectada", o mesmo clique pode abrir a conversa que aparece sob o cursor.

## Como testar
- **Login:** "Continuar com Google" ou "Discord"; conferir foto e @nick no rodapé.
- **Chamada entre redes:** dois computadores em redes diferentes na mesma sala; em Configurações > Rede ver "Somente STUN" ou "TURN da sua conta".
- **Transmissão:** numa sala com outra pessoa, "Compartilhar (beta)", escolher tela ou janela; a outra pessoa abre "Transmissão".
- **SFU:** com `livekit.json` criado, o diagnóstico de Rede deve mostrar "Modo da chamada: SFU".
- Ao relatar problema: versão (Configurações > Sobre), o que aparece em Rede > Diagnóstico e o que aconteceu.

## Não testado antes desta versão
SFU em rede real, medições de transmissão, 4+ espectadores e a paridade completa contra o Electron devem ser testados depois da publicação. SFU e transmissão com servidor LiveKit, Coturn e GPU reais; login e funções contra o projeto Supabase real nesta integração; instalação em computador limpo; atualização 1.3.0 → 1.4.0 pelo feed real.
