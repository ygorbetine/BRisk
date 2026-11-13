; Instalador do Briks (app nativo CUI). Mesmo appId (com.briks.calls), nome, pasta, atalhos e chave de desinstalacao do instalador
; do Electron 1.2.x, para o Windows (e o electron-updater do 1.2.2) tratarem isto como ATUALIZACAO do mesmo produto.
; Instalacao por usuario (sem administrador): %LOCALAPPDATA%\Programs\@briksdesktop.
; Parametros de compilacao (-D): APP_VERSION, STAGE_DIR, OUT_FILE, [APP_GUID, INSTALL_SUBDIR, TEST_MODE]
; Texto ASCII de proposito (o makensis le o script como ANSI sem BOM).
Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifndef APP_VERSION
  !error "defina -DAPP_VERSION=x.y.z"
!endif
!ifndef STAGE_DIR
  !error "defina -DSTAGE_DIR=pasta"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "Briks-Setup-${APP_VERSION}.exe"
!endif
; GUID derivado do appId com.briks.calls (o mesmo que o electron-builder gerou); TEST_MODE usa outro, para nao tocar na instalacao real
!ifndef APP_GUID
  !define APP_GUID "303b4820-db60-5afe-b1ba-dc7d7a2d7acb"
!endif
!ifndef INSTALL_SUBDIR
  !define INSTALL_SUBDIR "@briksdesktop"
!endif
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_GUID}"
!define APP_KEY "Software\${APP_GUID}"
!define EXE_NAME "Briks.exe"
!define UNINST_NAME "Uninstall Briks.exe"
!ifdef TEST_MODE
  !define SHORTCUT_NAME "Briks (teste de instalador)"
  !define FW_NAME "Briks (teste de instalador)"
!else
  !define SHORTCUT_NAME "Briks"
  !define FW_NAME "Briks"
!endif

Name "Briks"
OutFile "${OUT_FILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\${INSTALL_SUBDIR}"
InstallDirRegKey HKCU "${APP_KEY}" "InstallLocation"
SetCompressor /SOLID lzma
BrandingText "Briks ${APP_VERSION}"
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "Briks"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "Briks"
VIAddVersionKey "FileDescription" "Instalador do Briks"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 Briks"
VIAddVersionKey "OriginalFilename" "Briks-Setup-${APP_VERSION}.exe"
VIAddVersionKey "InternalName" "Briks-Setup"
!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "PortugueseBR"

Var Updated
Var ForceRun
Var OldDir

Function .onInit
  ${GetParameters} $R0
  StrCpy $Updated "0"
  StrCpy $ForceRun "0"
  ${GetOptions} $R0 "--updated" $R1
  ${IfNot} ${Errors}
    StrCpy $Updated "1"
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "--force-run" $R1
  ${IfNot} ${Errors}
    StrCpy $ForceRun "1"
  ${EndIf}
  ClearErrors
!ifndef TEST_MODE
  ; o app precisa estar fechado (o electron-updater e o updater do CUI fecham o app antes de chamar o instalador)
  StrCpy $R2 "0"
  check_running:
  nsExec::Exec 'cmd /c tasklist /FI "IMAGENAME eq ${EXE_NAME}" /NH | find /I "${EXE_NAME}"'
  Pop $R3
  ${If} $R3 == "0"
    IntOp $R2 $R2 + 1
    ${If} $R2 < 10
      Sleep 1000
      Goto check_running
    ${EndIf}
    ${If} ${Silent}
      SetErrorLevel 2
      Abort
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "O Briks esta aberto. Feche-o e tente de novo." IDRETRY retry_running
    Abort
    retry_running:
    StrCpy $R2 "0"
    Goto check_running
  ${EndIf}
!endif
FunctionEnd

Section "Install"
  ; --- remove a instalacao anterior do MESMO produto (Electron 1.x ou CUI): o desinstalador dela nao apaga os dados do usuario ---
  StrCpy $OldDir ""
  ReadRegStr $OldDir HKCU "${APP_KEY}" "InstallLocation"
  ReadRegStr $R0 HKCU "${UNINST_KEY}" "QuietUninstallString"
  ${If} $R0 != ""
  ${AndIf} $OldDir != ""
  ${AndIf} ${FileExists} "$OldDir\${UNINST_NAME}"
    DetailPrint "Removendo a versao anterior..."
    ExecWait '$R0 /KEEP_APP_DATA _?=$OldDir' $R1
    Delete "$OldDir\${UNINST_NAME}"
  ${EndIf}
  ; sobras do Electron (Chromium) quando o desinstalador antigo nao rodou: so apagamos se for mesmo uma pasta do Electron
  ${If} ${FileExists} "$INSTDIR\chrome_100_percent.pak"
    RMDir /r "$INSTDIR"
  ${EndIf}

  ; 1.4.1: nenhum processo auxiliar (core, overlay, backend) deste diretorio pode segurar arquivo: encerra so os que rodam de $INSTDIR (outras instalacoes nao sao tocadas)
  nsExec::Exec `powershell -NoProfile -NonInteractive -WindowStyle Hidden -Command "Get-Process briks-core,briks_overlay,briks-desktop-backend -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force"`
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"
  FileOpen $R9 "$INSTDIR\briks.installed" w
  FileWrite $R9 "${APP_VERSION}"
  FileClose $R9
  WriteUninstaller "$INSTDIR\${UNINST_NAME}"

  WriteRegStr HKCU "${APP_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APP_KEY}" "ShortcutName" "${SHORTCUT_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "Briks ${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Briks"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE_NAME},0"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\${UNINST_NAME}" /currentuser'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\${UNINST_NAME}" /currentuser /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $R0 $R1 $R2
  IntFmt $R0 "0x%08X" $R0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $R0

  ; Firewall: regra de ENTRADA so para os programas instalados (sem abrir porta geral). Apaga antes de criar (reinstalar/atualizar nao duplica).
  ; Exige administrador; a instalacao e por usuario, entao sem admin a regra e pulada com aviso no log e a instalacao segue (o Windows pergunta na 1a chamada).
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${FW_NAME}"'
  Pop $R0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${FW_NAME} core"'
  Pop $R0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="${FW_NAME}" dir=in action=allow program="$INSTDIR\${EXE_NAME}" enable=yes profile=private,public'
  Pop $R0
  ${If} $R0 == "0"
    nsExec::ExecToLog 'netsh advfirewall firewall add rule name="${FW_NAME} core" dir=in action=allow program="$INSTDIRriks-core.exe" enable=yes profile=private,public'
    Pop $R0
  ${EndIf}
  ${If} $R0 != "0"
    DetailPrint "Aviso: regra do Firewall nao criada (precisa de administrador). O Windows pode perguntar na primeira chamada."
  ${EndIf}

  CreateShortCut "$SMPROGRAMS\${SHORTCUT_NAME}.lnk" "$INSTDIR\${EXE_NAME}" "" "$INSTDIR\${EXE_NAME}" 0
  CreateShortCut "$DESKTOP\${SHORTCUT_NAME}.lnk" "$INSTDIR\${EXE_NAME}" "" "$INSTDIR\${EXE_NAME}" 0
SectionEnd

Function .onInstSuccess
!ifndef TEST_MODE
  ; instalacao pela interface ou atualizacao automatica (--force-run): abre o app
  ${If} $ForceRun == "1"
    Exec '"$INSTDIR\${EXE_NAME}"'
  ${ElseIfNot} ${Silent}
    Exec '"$INSTDIR\${EXE_NAME}"'
  ${EndIf}
!endif
FunctionEnd

Section "Uninstall"
  ; 1.4.1: nenhum processo auxiliar (core, overlay, backend) deste diretorio pode segurar arquivo: encerra so os que rodam de $INSTDIR (outras instalacoes nao sao tocadas)
  nsExec::Exec `powershell -NoProfile -NonInteractive -WindowStyle Hidden -Command "Get-Process briks-core,briks_overlay,briks-desktop-backend -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force"`
  ; so apaga a pasta inteira se foi este instalador que a criou; os dados do usuario (%APPDATA%\Briks-CUI) nunca sao tocados
  ${If} ${FileExists} "$INSTDIR\briks.installed"
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${FW_NAME}"'
    Pop $R0
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="${FW_NAME} core"'
    Pop $R0
    Delete "$SMPROGRAMS\${SHORTCUT_NAME}.lnk"
    Delete "$DESKTOP\${SHORTCUT_NAME}.lnk"
    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "${APP_KEY}"
    RMDir /r "$INSTDIR"
  ${Else}
    MessageBox MB_OK|MB_ICONSTOP "Esta pasta nao foi criada por este instalador; nada foi removido."
  ${EndIf}
SectionEnd
