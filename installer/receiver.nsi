; Art-Net Timecode Receiver, Windows x64 installer. makensis -DVERSION= -DOUT= -DSRC= receiver.nsi
Unicode true
!include "x64.nsh"
Name "Art-Net Timecode Receiver ${VERSION}"
OutFile "${OUT}"
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\Art-Net Timecode Receiver"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Art-Net Timecode Receiver"

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
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  File "${SRC}\ArtNetTimecodeReceiver.exe"
  CreateShortcut "$SMPROGRAMS\Art-Net Timecode Receiver.lnk" "$INSTDIR\ArtNetTimecodeReceiver.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNKEY}" "DisplayName" "Art-Net Timecode Receiver"
  WriteRegStr HKLM "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNKEY}" "Publisher" "Chris Thoms"
  WriteRegStr HKLM "${UNKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\Art-Net Timecode Receiver.lnk"
  Delete "$INSTDIR\ArtNetTimecodeReceiver.exe"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNKEY}"
SectionEnd
