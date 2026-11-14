---
title: Política de privacidade do Briks
---

# Política de privacidade do Briks

Última atualização: 13 de novembro de 2026.

O Briks é um aplicativo de código aberto (AGPL-3.0) para Windows, de chamadas de voz e transmissão de tela, **para maiores de 18 anos**. Esta página diz quais dados o app trata, para quê, com quem são compartilhados e como você exerce seus direitos (LGPD, Lei 13.709/2018). O código é público: cada afirmação aqui pode ser conferida em [github.com/ygorbetine/Briks](https://github.com/ygorbetine/Briks).

**Controlador dos dados:** o mantenedor do projeto Briks (GitHub: [ygorbetine](https://github.com/ygorbetine)).
**Contato e solicitações de privacidade (exclusão, acesso, correção):** [relato privado no GitHub](https://github.com/ygorbetine/Briks/security/advisories/new).

## Dados que tratamos

| Dado | Para quê | Onde fica |
| --- | --- | --- |
| E-mail, nome e foto da conta Google ou Discord | Identificar você, mostrar seu nome e foto aos amigos | Supabase (servidor de contas do Briks) |
| @nick, amigos, grupos, status que você escreve | Funcionamento social do app | Supabase |
| Confirmação de que você é maior de 18 anos | Cumprir o limite de idade do serviço | Só no seu computador (`age-ok.txt`, guarda o identificador da conta, não a data) |
| Data de nascimento da conta Google (só quando a conferência pelo Google estiver ligada) | Saber se você tem 18 anos ou mais, uma única vez no login | **Não é guardada.** O token do Google é descartado logo depois |
| Áudio, vídeo e tela das chamadas | Fazer a chamada | Não são gravados nem guardados; passam em tempo real, direto entre amigos (P2P) ou por um servidor LiveKit que só repassa |
| Mensagens | Conversa entre amigos | Ficam nos computadores das pessoas envolvidas |
| Contadores anônimos de conexão (opcional) | Melhorar a qualidade das chamadas | Só números diários (conectou ou não, direto ou por relay). Pode ser desligado nas Configurações |

Não há anúncios, rastreamento nem venda de dados.

## Dados da sua conta Google (API do Google)

Ao entrar com Google, o Briks recebe seu e-mail, nome e foto. Quando a conferência de idade está ligada, ele também usa a permissão `https://www.googleapis.com/auth/user.birthday.read` para ler a data de nascimento da sua conta **uma única vez**, calcular se você tem 18 anos ou mais e descartar a data: guardamos apenas "maior de 18" no seu computador. Não usamos a data para publicidade, não a compartilhamos e não a transferimos a ninguém.

O uso das informações recebidas das APIs do Google pelo Briks segue a [Política de Dados de Usuário dos Serviços de API do Google](https://developers.google.com/terms/api-services-user-data-policy), incluindo os requisitos de Uso Limitado.

## Base legal

Execução do serviço que você pediu ao criar a conta (art. 7º, V, da LGPD), legítimo interesse para segurança e qualidade das chamadas (art. 7º, IX) e cumprimento de obrigação legal quanto ao limite de idade (art. 7º, II).

## Com quem compartilhamos (operadores)

- **Supabase**: contas, amigos e grupos.
- **LiveKit Cloud**: servidor que repassa áudio e vídeo das chamadas no modo servidor (sem gravar).
- **Cloudflare**: relay TURN, que repassa tráfego cifrado quando a conexão direta não é possível.
- **Google e Discord**: provedores de login.
- **GitHub**: o app consulta a página de versões do projeto para saber se há atualização.

Alguns desses provedores podem tratar dados fora do Brasil. Nesses casos a transferência internacional se apoia em cláusulas contratuais dos próprios provedores e se limita ao necessário para o serviço.

## Por quanto tempo guardamos

Os dados da conta ficam enquanto a conta existir. Ao pedir a exclusão, apagamos os dados da conta no Supabase, salvo o que a lei obrigue a manter. Dados locais ficam em `%APPDATA%\Briks-CUI` no seu computador e você pode apagá-los quando quiser.

## Seus direitos

Você pode pedir confirmação do tratamento, acesso, correção, anonimização, portabilidade, eliminação e informação sobre compartilhamento, e revogar consentimentos, usando o contato acima. Também pode reclamar à Autoridade Nacional de Proteção de Dados (ANPD).

## Crianças e adolescentes

O Briks é **só para maiores de 18 anos**. Não coletamos dados de menores de propósito. Se descobrirmos que uma conta é de menor, ela é bloqueada e os dados são apagados.

## Alterações

Mudanças nesta política são feitas por commits neste repositório e a data de atualização acima muda.
