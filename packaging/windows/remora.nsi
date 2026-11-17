; Remora for Windows installer (bd remora-7oih.6). Built on Linux by tools/windows-installer.sh
; from the folder tools/windows-stage.sh stages:
;
;   makensis -DDIST=<staged folder> -DVERSION=<x.y.z> -DOUTFILE=<installer.exe> remora.nsi
;
; What installing does: copies the folder to Program Files, adds a Start-menu shortcut to
; remora-gui.exe and an Apps & features entry, and — because Remora runs Android in a Linux guest
; that needs it — turns on the Windows Hypervisor Platform when it is off, which takes a restart.
; Uninstalling stops the guest, removes the Start-menu folders Remora wrote for its profiles and
; then the program, and leaves %LOCALAPPDATA%\remora alone: the guest's data disk holds every
; Android image and every profile's /data, and is the user's to delete.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin
ManifestDPIAware true

Name "Remora"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Remora"
InstallDirRegKey HKLM "Software\Remora" "InstallDir"
BrandingText "Remora ${VERSION}"
; tools/windows-icon.sh's, which the executables carry too. makensis runs from this directory.
Icon "io.github.EtherAura.Remora.ico"
UninstallIcon "io.github.EtherAura.Remora.ico"

!include "LogicLib.nsh"
!include "x64.nsh"

!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Remora"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "Remora needs 64-bit Windows."
        Abort
    ${EndIf}
    SetRegView 64
    ; A Program Files install is for every user, so is its Start-menu entry.
    SetShellVarContext all
FunctionEnd

Function un.onInit
    SetRegView 64
    SetShellVarContext all
FunctionEnd

; What keeps files in $INSTDIR open after Remora itself has exited, which an update could not
; replace and an uninstall could not delete: a running guest holds qemu\, and adb's server, which
; Remora starts and which outlives it, holds adb.exe and its two DLLs (seen on the test VM: the
; uninstall left all three behind). `guest stop` powers the guest down cleanly; both return at once
; when there is nothing running.
!macro StopRemora
    ${If} ${FileExists} "$INSTDIR\remora.exe"
        DetailPrint "Stopping Remora's Linux guest, if it is running..."
        nsExec::ExecToLog '"$INSTDIR\remora.exe" guest stop'
        Pop $0
    ${EndIf}
    ${If} ${FileExists} "$INSTDIR\adb.exe"
        nsExec::ExecToLog '"$INSTDIR\adb.exe" kill-server'
        Pop $0
    ${EndIf}
!macroend

Section "Remora"
    SetOutPath "$INSTDIR"
    !insertmacro StopRemora
    ; A clean copy: files a previous version shipped and this one does not must not linger,
    ; least of all a stale guest image or vendor script.
    RMDir /r "$INSTDIR\guest"
    RMDir /r "$INSTDIR\qemu"
    RMDir /r "$INSTDIR\vendor"
    File /r "${DIST}\*.*"

    CreateShortcut "$SMPROGRAMS\Remora.lnk" "$INSTDIR\remora-gui.exe"
    WriteUninstaller "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Remora" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${UNINST}" "DisplayName" "Remora"
    WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINST}" "Publisher" "Remora"
    WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\remora-gui.exe"
    WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINST}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
SectionEnd

; The guest runs on the Windows Hypervisor Platform. Asked rather than guessed: the feature exists
; on every edition, Home included, but is off by default.
Section "Windows Hypervisor Platform"
    nsExec::ExecToStack 'powershell.exe -NoProfile -NonInteractive -Command "(Get-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform).State"'
    Pop $0
    Pop $1
    StrCpy $1 $1 7
    ${If} $0 == 0
    ${AndIf} $1 != "Enabled"
        DetailPrint "Turning on the Windows Hypervisor Platform (Remora's Linux guest runs on it)..."
        nsExec::ExecToLog 'dism.exe /online /enable-feature /featurename:HypervisorPlatform /all /norestart'
        Pop $0
        ; 3010 is DISM's "done, restart required".
        ${If} $0 == 0
        ${OrIf} $0 == 3010
            SetRebootFlag true
        ${Else}
            DetailPrint "DISM could not turn it on (exit $0): Remora says how when it starts."
        ${EndIf}
    ${EndIf}
SectionEnd

Section "Uninstall"
    !insertmacro StopRemora
    ; The profiles' Start-menu folders (`remora apps`) are the user's, under their own Start menu:
    ; remora.exe runs as that user and removes what its records say it wrote.
    nsExec::ExecToLog '"$INSTDIR\remora.exe" apps --remove-all'
    Pop $0
    Delete "$SMPROGRAMS\Remora.lnk"
    RMDir /r "$INSTDIR"
    DeleteRegKey HKLM "${UNINST}"
    DeleteRegKey HKLM "Software\Remora"
SectionEnd
