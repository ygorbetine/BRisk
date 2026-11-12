# Assinatura de código gratuita (SignPath Foundation)

O Briks é AGPL-3.0 e o repositório é público, então pode usar o certificado gratuito da **SignPath Foundation**. O publicador que o Windows mostra passa a ser "SignPath Foundation". O SmartScreen ainda pode avisar nas primeiras semanas, até o app ganhar reputação.

## O que só o dono do projeto pode fazer
1. Pedir o certificado em https://signpath.org/foundation (URL do repositório, licença, descrição). Espera de alguns dias.
2. Na SignPath, criar a organização e o projeto com slug `briks`, ligar ao GitHub (Trusted Build System "GitHub.com") e criar o signing policy `release-signing` (com aprovação manual, se quiser).
3. No projeto, criar as duas **artifact configurations** abaixo (slugs `executaveis` e `instalador`).
4. No GitHub (Settings do repositório):
   - Secret `SIGNPATH_API_TOKEN` (token de um usuário API da SignPath com permissão de submeter).
   - Variables `SIGNPATH_ORGANIZATION_ID`, `BRIKS_SUPABASE_URL` e `BRIKS_SUPABASE_PUBLISHABLE_KEY` (a chave é a publishable/anon, pública; nunca a secret).
   - Environment `release` com **Required reviewers** (você): é o que impede a publicação sem o seu OK.

## Artifact configuration `executaveis` (zip com os .exe)
```xml
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <zip-file>
    <pe-file path="Briks.exe"><authenticode-sign/></pe-file>
    <pe-file path="briks_overlay.exe"><authenticode-sign/></pe-file>
    <pe-file path="briks-core.exe"><authenticode-sign/></pe-file>
    <pe-file path="briks-desktop-backend.exe"><authenticode-sign/></pe-file>
  </zip-file>
</artifact-configuration>
```

## Artifact configuration `instalador`
```xml
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <zip-file>
    <pe-file path="Briks-Setup-*.exe"><authenticode-sign/></pe-file>
  </zip-file>
</artifact-configuration>
```

## Como rodar
Actions > "Release Windows (assinado)" > Run workflow, com a versão (x.y.z) e `publish` desligado ou ligado.
Ordem: compila e monta o stage (`build-installer.ps1 -StageOnly`) → assina os 4 .exe → confere a assinatura → empacota (`-PackageOnly`) → assina o instalador → gera o `latest.yml` com o sha512 do arquivo **já assinado** (`make-latest-yml.ps1`) → guarda como artifact.
Com `publish` ligado, o job `publish` espera a aprovação do ambiente `release` e só então cria a tag e o release no GitHub.

## Observações
- O `latest.yml` tem que ser gerado depois da assinatura, porque a assinatura muda os bytes do instalador.
- Os DLLs do runtime do VC já vêm assinados pela Microsoft e não entram na configuração.
- `scripts/release-cui.ps1` continua servindo para builds locais sem assinatura (ensaio e testes). Release oficial assinado sai pelo workflow.
- Este fluxo ainda não foi executado: falta a aprovação da SignPath. Teste primeiro com o signing policy `test-signing` (a assinatura de teste aparece como não confiável, e o `verify-windows-signature.ps1` vai falhar nela de propósito).
