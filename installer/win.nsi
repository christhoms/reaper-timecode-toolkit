; CT LTC to Art-Net Timecode, Windows x64 installer. makensis -DVERSION= -DOUT= -DSRC= win.nsi
Unicode true
!include "x64.nsh"
Name "CT LTC to Art-Net Timecode ${VERSION}"
OutFile "${OUT}"
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\CT LTC ArtNet"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CT LTC ArtNet"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "64-bit Windows required."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section
  SetOutPath "$COMMONFILES64\CLAP"
  File "${SRC}\CT_LTC_ArtNet.clap"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNKEY}" "DisplayName" "CT LTC to Art-Net Timecode"
  WriteRegStr HKLM "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNKEY}" "Publisher" "Chris Thoms"
  WriteRegStr HKLM "${UNKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$COMMONFILES64\CLAP\CT_LTC_ArtNet.clap"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNKEY}"
SectionEnd
