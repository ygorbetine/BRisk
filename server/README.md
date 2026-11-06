# Servidor central legado e emissor TURN

As rotas de conta, comunidades e mensagens deste diretório pertencem à arquitetura centralizada anterior do Briks. Elas não participam do aplicativo desktop P2P e não são iniciadas pelo fluxo padrão.

O endpoint `/rtc/credentials` é a exceção mantida: ele pode emitir credenciais TURN temporárias para deployments autenticados ou, quando explicitamente habilitado, para o modo P2P local protegido por rate limit. O segredo compartilhado nunca é enviado ao aplicativo.

Consulte `infrastructure/coturn/README.md` para configuração, TLS, portas e variáveis de produção.
