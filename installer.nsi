Unicode True
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"

!ifndef RS_ARTIFACT_ROOT
  !error "RS_ARTIFACT_ROOT must point to a clean Avatar Suite artifact directory."
!endif
!ifndef RS_PREREQUISITE_ROOT
  !error "RS_PREREQUISITE_ROOT must contain the verified Microsoft prerequisite installers."
!endif
!ifndef RS_VERSION
  !define RS_VERSION "1.0.0-owner.1"
!endif
!ifndef RS_CHANNEL
  !define RS_CHANNEL "Owner Build"
!endif
!ifndef RS_OUTPUT_FILE
  !define RS_OUTPUT_FILE "RearSilver-Avatar-Suite-Owner-Setup.exe"
!endif
!ifndef RS_VC_RUNTIME_MIN_VERSION
  !define RS_VC_RUNTIME_MIN_VERSION "0"
!endif

!define PRODUCT_NAME "RearSilver Avatar Suite"
!define PRODUCT_PUBLISHER "RearSilver Gaming"
!define PRODUCT_REG_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RearSilver Avatar Suite"

Name "${PRODUCT_NAME} | ${RS_CHANNEL}"
OutFile "${RS_OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\RearSilver Avatar Suite"
InstallDirRegKey HKLM "${PRODUCT_REG_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor lzma
SetCompressorDictSize 64
ManifestDPIAware true

!define MUI_ICON "assets\Branding\rearsilver-avatar-suite.ico"
!define MUI_UNICON "assets\Branding\rearsilver-avatar-suite.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_WELCOMEPAGE_TITLE "Welcome to RearSilver Avatar Suite"
!define MUI_WELCOMEPAGE_TITLE_3LINES
!define MUI_WELCOMEPAGE_TEXT "This setup installs RearSilver Avatar Suite ${RS_VERSION} (${RS_CHANNEL}).$\r$\n$\r$\nClose Avatar Suite before continuing."
!define MUI_FINISHPAGE_TITLE "RearSilver Avatar Suite is ready"
!define MUI_FINISHPAGE_TITLE_3LINES
!define MUI_FINISHPAGE_TEXT "RearSilver Avatar Suite has been installed successfully."
!define MUI_FINISHPAGE_RUN_TEXT "Launch RearSilver Avatar Suite"
!define MUI_FINISHPAGE_RUN "$INSTDIR\RearSilver Avatar Suite.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Function HasWebView2Runtime
  Push $0
  Push $1
  StrCpy $1 "0"
  SetRegView 32
  ReadRegStr $0 HKLM "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
  ${If} $0 != ""
  ${AndIf} $0 != "0.0.0.0"
    StrCpy $1 "1"
  ${Else}
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
    ${If} $0 != ""
    ${AndIf} $0 != "0.0.0.0"
      StrCpy $1 "1"
    ${EndIf}
  ${EndIf}
  SetRegView 64
  Pop $0
  Exch $1
FunctionEnd

Function InstallPrerequisites
  SetOutPath "$PLUGINSDIR"
  DetailPrint "Checking Microsoft Visual C++ Runtime..."
  StrCpy $3 "1"
  ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
  ReadRegStr $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Version"
  ${If} $0 == 1
  ${AndIf} $1 != ""
  ${AndIf} "${RS_VC_RUNTIME_MIN_VERSION}" != "0"
    StrCpy $2 $1 1
    ${If} $2 == "v"
      StrCpy $1 $1 "" 1
    ${EndIf}
    ${VersionCompare} $1 "${RS_VC_RUNTIME_MIN_VERSION}" $2
    ${If} $2 == 0
    ${OrIf} $2 == 1
      StrCpy $3 "0"
    ${EndIf}
  ${EndIf}
  ${If} $3 == "1"
    File "/oname=$PLUGINSDIR\vc_redist.x64.exe" "${RS_PREREQUISITE_ROOT}\vc_redist.x64.exe"
    ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1638
    ${AndIf} $0 != 3010
      MessageBox MB_ICONSTOP|MB_OK "Microsoft Visual C++ Runtime setup failed with code $0. Avatar Suite was not installed."
      Abort
    ${EndIf}
  ${EndIf}

  DetailPrint "Checking Microsoft Edge WebView2 Runtime..."
  Call HasWebView2Runtime
  Pop $1
  ${If} $1 != "1"
    File "/oname=$PLUGINSDIR\MicrosoftEdgeWebView2RuntimeInstallerX64.exe" "${RS_PREREQUISITE_ROOT}\MicrosoftEdgeWebView2RuntimeInstallerX64.exe"
    ExecWait '"$PLUGINSDIR\MicrosoftEdgeWebView2RuntimeInstallerX64.exe" /silent /install' $0
    ${If} $0 != 0
    ${AndIf} $0 != 3010
      MessageBox MB_ICONSTOP|MB_OK "Microsoft Edge WebView2 Runtime setup failed with code $0. Avatar Suite was not installed."
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "RearSilver Avatar Suite" MainSection
  SetShellVarContext all
  SetRegView 64
  Call InstallPrerequisites

  DetailPrint "Installing RearSilver Avatar Suite files..."
  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${RS_ARTIFACT_ROOT}\app\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\RearSilver Avatar Suite"
  CreateShortcut "$SMPROGRAMS\RearSilver Avatar Suite\RearSilver Avatar Suite.lnk" "$INSTDIR\RearSilver Avatar Suite.exe"
  CreateShortcut "$SMPROGRAMS\RearSilver Avatar Suite\Uninstall RearSilver Avatar Suite.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "DisplayName" "${PRODUCT_NAME} (${RS_CHANNEL})"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "DisplayVersion" "${RS_VERSION}"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "DisplayIcon" "$INSTDIR\RearSilver Avatar Suite.exe"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKLM "${PRODUCT_REG_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${PRODUCT_REG_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  Delete "$SMPROGRAMS\RearSilver Avatar Suite\RearSilver Avatar Suite.lnk"
  Delete "$SMPROGRAMS\RearSilver Avatar Suite\Uninstall RearSilver Avatar Suite.lnk"
  RMDir "$SMPROGRAMS\RearSilver Avatar Suite"
  Delete "$INSTDIR\*.*"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${PRODUCT_REG_KEY}"
SectionEnd
