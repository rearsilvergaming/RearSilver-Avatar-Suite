Unicode True
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"

!ifndef RS_ARTIFACT_ROOT
  !error "RS_ARTIFACT_ROOT must point to a clean Avatar Suite artifact directory."
!endif
!ifndef RS_PREREQUISITE_ROOT
  !error "RS_PREREQUISITE_ROOT must contain the verified Microsoft prerequisite installers."
!endif
!ifndef RS_VERSION
  !define RS_VERSION "1.0.0-owner.6"
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
!ifndef RS_EXPIRY_ENABLED
  !define RS_EXPIRY_ENABLED "OFF"
!endif
!ifndef RS_EXPIRY_DISPLAY
  !define RS_EXPIRY_DISPLAY "Not applicable"
!endif

!define PRODUCT_NAME "RearSilver Avatar Suite"
!define PRODUCT_PUBLISHER "RearSilver Gaming"
!define PRODUCT_WEB_SITE "https://github.com/rearsilvergaming/RearSilver-Avatar-Suite"
!define PRODUCT_REG_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RearSilver Avatar Suite"
Var UpdateHandoff
Var PreviousVersion

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
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "assets\Branding\installer-header.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP "assets\Branding\installer-welcome.bmp"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_HEADER_TEXT "RearSilver Avatar Suite"
!define MUI_HEADER_SUBTEXT "Standalone PNGTuber creation and output"
!define MUI_WELCOMEPAGE_TITLE "Welcome to RearSilver Avatar Suite"
!define MUI_WELCOMEPAGE_TITLE_3LINES
!if "${RS_EXPIRY_ENABLED}" == "ON"
  !define MUI_WELCOMEPAGE_TEXT "This setup installs RearSilver Avatar Suite ${RS_VERSION} (${RS_CHANNEL}).$\r$\n$\r$\nThe application and supporting files are installed as one managed product and can be updated or removed cleanly.$\r$\n$\r$\nThis time-limited test build expires on ${RS_EXPIRY_DISPLAY}. Settings and diagnostics remain available for recovery and support.$\r$\n$\r$\nClose Avatar Suite before continuing."
!else
  !define MUI_WELCOMEPAGE_TEXT "This setup installs RearSilver Avatar Suite ${RS_VERSION} (${RS_CHANNEL}).$\r$\n$\r$\nThe application and supporting files are installed as one managed product and can be updated or removed cleanly.$\r$\n$\r$\nClose Avatar Suite before continuing."
!endif
!define MUI_FINISHPAGE_TITLE "RearSilver Avatar Suite is ready"
!define MUI_FINISHPAGE_TITLE_3LINES
!define MUI_FINISHPAGE_TEXT "RearSilver Avatar Suite has been installed successfully.$\r$\n$\r$\nOpen Avatar Suite to configure or load an avatar."
!define MUI_FINISHPAGE_RUN_TEXT "Launch RearSilver Avatar Suite"
!define MUI_FINISHPAGE_RUN "$INSTDIR\RearSilver Avatar Suite.exe"
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose where to install RearSilver Avatar Suite and its supporting files."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Avatar Suite destination folder"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "License.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  StrCpy $UpdateHandoff "0"
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/UPDATEHANDOFF" $1
  IfErrors +2 0
    StrCpy $UpdateHandoff "1"
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP|MB_OK "RearSilver Avatar Suite requires 64-bit Windows."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP|MB_OK "RearSilver Avatar Suite requires Windows 10 or Windows 11."
    Abort
  ${EndIf}
FunctionEnd

Function EnsureAvatarSuiteClosed
  StrCmp $UpdateHandoff "1" closed_for_handoff
  check_avatar_suite:
  FindWindow $0 "RearSilverAvatarWindow" ""
  ${If} $0 != 0
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "RearSilver Avatar Suite is running. Close it before continuing, then choose Retry." IDRETRY check_avatar_suite IDCANCEL cancel_avatar_suite
  ${EndIf}
  Return
  closed_for_handoff:
    Return
  cancel_avatar_suite:
    Abort
FunctionEnd

Function un.EnsureAvatarSuiteClosed
  check_avatar_suite_uninstall:
  FindWindow $0 "RearSilverAvatarWindow" ""
  ${If} $0 != 0
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "RearSilver Avatar Suite is running. Close it before uninstalling, then choose Retry." IDRETRY check_avatar_suite_uninstall IDCANCEL cancel_avatar_suite_uninstall
  ${EndIf}
  Return
  cancel_avatar_suite_uninstall:
    Abort
FunctionEnd

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
    DetailPrint "Extracting Microsoft Visual C++ installer..."
    File "/oname=$PLUGINSDIR\vc_redist.x64.exe" "${RS_PREREQUISITE_ROOT}\vc_redist.x64.exe"
    DetailPrint "Installing Microsoft Visual C++ Runtime; please wait..."
    ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
    ${If} $0 != 0
    ${AndIf} $0 != 1638
    ${AndIf} $0 != 3010
      MessageBox MB_ICONSTOP|MB_OK "Microsoft Visual C++ Runtime setup failed with code $0. Avatar Suite was not installed."
      Abort
    ${EndIf}
  ${Else}
    DetailPrint "Microsoft Visual C++ Runtime $1 is already suitable; skipping installation."
  ${EndIf}

  DetailPrint "Checking Microsoft Edge WebView2 Runtime..."
  Call HasWebView2Runtime
  Pop $1
  ${If} $1 != "1"
    DetailPrint "Extracting and installing Microsoft Edge WebView2 Runtime; this may take a few minutes..."
    File "/oname=$PLUGINSDIR\MicrosoftEdgeWebView2RuntimeInstallerX64.exe" "${RS_PREREQUISITE_ROOT}\MicrosoftEdgeWebView2RuntimeInstallerX64.exe"
    ExecWait '"$PLUGINSDIR\MicrosoftEdgeWebView2RuntimeInstallerX64.exe" /silent /install' $0
    ${If} $0 != 0
    ${AndIf} $0 != 3010
      MessageBox MB_ICONSTOP|MB_OK "Microsoft Edge WebView2 Runtime setup failed with code $0. Avatar Suite was not installed."
      Abort
    ${EndIf}
    Call HasWebView2Runtime
    Pop $1
    ${If} $1 != "1"
      MessageBox MB_ICONSTOP|MB_OK "Microsoft Edge WebView2 Runtime could not be verified after installation. Avatar Suite was not installed."
      Abort
    ${EndIf}
  ${Else}
    DetailPrint "Microsoft Edge WebView2 Runtime is already installed."
  ${EndIf}
FunctionEnd

Section "RearSilver Avatar Suite" MainSection
  SetShellVarContext all
  SetRegView 64
  Call EnsureAvatarSuiteClosed
  ReadRegStr $PreviousVersion HKLM "${PRODUCT_REG_KEY}" "DisplayVersion"
  Call InstallPrerequisites

  DetailPrint "Installing RearSilver Avatar Suite files..."
  ; Replace only files and directories owned by Avatar Suite. The directory
  ; chooser permits an existing destination, so never recursively delete the
  ; installation root or any unrelated files a user may keep there.
  RMDir /r "$INSTDIR\built-in-layers"
  Delete "$INSTDIR\RearSilver Avatar Suite.exe"
  Delete "$INSTDIR\RearSilver-Avatar-Suite-Updater.exe"
  Delete "$INSTDIR\WebView2Loader.dll"
  Delete "$INSTDIR\avatar-settings.html"
  Delete "$INSTDIR\spout2-license.txt"
  Delete "$INSTDIR\settings-header.png"
  Delete "$INSTDIR\settings-badge.png"
  Delete "$INSTDIR\splash.png"
  Delete "$INSTDIR\Sora-Variable.ttf"
  Delete "$INSTDIR\default-avatar-idle.png"
  Delete "$INSTDIR\default-avatar-reaction.png"
  Delete "$INSTDIR\rail-presets.png"
  Delete "$INSTDIR\rail-reactions-on.png"
  Delete "$INSTDIR\rail-reactions-off.png"
  Delete "$INSTDIR\rail-websocket.png"
  Delete "$INSTDIR\rail-background.png"
  Delete "$INSTDIR\rail-tools.png"
  Delete "$INSTDIR\rail-settings.png"
  Delete "$INSTDIR\Uninstall.exe"
  SetOutPath "$INSTDIR"
  File /r "${RS_ARTIFACT_ROOT}\app\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\RearSilver Avatar Suite"
  CreateShortcut "$SMPROGRAMS\RearSilver Avatar Suite\RearSilver Avatar Suite.lnk" "$INSTDIR\RearSilver Avatar Suite.exe"
  CreateShortcut "$SMPROGRAMS\RearSilver Avatar Suite\Uninstall RearSilver Avatar Suite.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "DisplayName" "${PRODUCT_NAME} (${RS_CHANNEL})"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "DisplayVersion" "${RS_VERSION}"
  ${If} $PreviousVersion != ""
  ${AndIf} $PreviousVersion != "${RS_VERSION}"
    WriteRegStr HKLM "${PRODUCT_REG_KEY}" "PreviousVersion" "$PreviousVersion"
    WriteRegStr HKLM "${PRODUCT_REG_KEY}" "UpdateCompletedVersion" "${RS_VERSION}"
  ${Else}
    DeleteRegValue HKLM "${PRODUCT_REG_KEY}" "PreviousVersion"
    DeleteRegValue HKLM "${PRODUCT_REG_KEY}" "UpdateCompletedVersion"
  ${EndIf}
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_REG_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
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
  Call un.EnsureAvatarSuiteClosed
  Delete "$SMPROGRAMS\RearSilver Avatar Suite\RearSilver Avatar Suite.lnk"
  Delete "$SMPROGRAMS\RearSilver Avatar Suite\Uninstall RearSilver Avatar Suite.lnk"
  RMDir "$SMPROGRAMS\RearSilver Avatar Suite"
  RMDir /r "$INSTDIR\built-in-layers"
  Delete "$INSTDIR\RearSilver Avatar Suite.exe"
  Delete "$INSTDIR\RearSilver-Avatar-Suite-Updater.exe"
  Delete "$INSTDIR\WebView2Loader.dll"
  Delete "$INSTDIR\avatar-settings.html"
  Delete "$INSTDIR\spout2-license.txt"
  Delete "$INSTDIR\settings-header.png"
  Delete "$INSTDIR\settings-badge.png"
  Delete "$INSTDIR\splash.png"
  Delete "$INSTDIR\Sora-Variable.ttf"
  Delete "$INSTDIR\default-avatar-idle.png"
  Delete "$INSTDIR\default-avatar-reaction.png"
  Delete "$INSTDIR\rail-*.png"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${PRODUCT_REG_KEY}"
SectionEnd
