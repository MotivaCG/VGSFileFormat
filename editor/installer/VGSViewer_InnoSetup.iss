; SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
; Copyright (c) 2026 Victor M. Feliz. All rights reserved.

; VGS Viewer installer (Inno Setup 6).
;
; It packages the folder deploy.ps1 makes (build\...-Release\deploy): the viewer, its Qt
; libraries, the MSVC runtime and the decoder licence. Run the deploy first, then
; compile this script (Inno Setup IDE, or ISCC.exe VGSViewer_InnoSetup.iss).
;
; The version is not set here: it is read from ..\viewer\viewerversion.h, where the viewer
; takes it from too, so the installer and the program always agree.

; ---- Version, from viewerversion.h (#define VIEWER_VERSION_NAME "v1.0.0") ----
#define VersionHeader AddBackslash(SourcePath) + "..\viewer\viewerversion.h"
#if !FileExists(VersionHeader)
  #error viewerversion.h not found beside the installer folder
#endif
#define MyAppVersion ""
#define HeaderFile
#define HeaderLine
#define Rest
; A quote is """" in ISPP, and a define inside a #sub is local unless public.
#sub ReadVersionLine
  #define HeaderLine FileRead(HeaderFile)
  #if Pos("VIEWER_VERSION_NAME", HeaderLine) > 0 && Pos("""", HeaderLine) > 0
    #define Rest Copy(HeaderLine, Pos("""", HeaderLine) + 1)
    #define public MyAppVersion Copy(Rest, 1, Pos("""", Rest) - 1)
  #endif
#endsub
#for {HeaderFile = FileOpen(VersionHeader); !FileEof(HeaderFile); 0} ReadVersionLine
#expr FileClose(HeaderFile)
#if MyAppVersion == ""
  #error VIEWER_VERSION_NAME not found in viewerversion.h
#endif
; "v1.2.3" -> "1.2.3.0", the four numbers Windows shows as the file version.
#define MyAppVersionNum Copy(MyAppVersion, 2) + ".0"

; ---- What and where ----
#define MyAppName "VGS Viewer"
#define MyAppExeName "VGSViewer.exe"
#define MyAppPathToInclude AddBackslash(SourcePath) + "..\viewer\build\Desktop_Qt_6_8_0_MSVC2022_64bit-Release\deploy"
#define MyAppOutputPath AddBackslash(SourcePath) + "Output"
#define MyAppInstallerFile "VGSViewerInstaller_" + MyAppVersion

#define MyAppPublisher "Victor M. Feliz"
#define MyAppURL "https://www.the4dscanner.com"


#if !FileExists(MyAppPathToInclude + "\" + MyAppExeName)
  #error The deploy folder has no VGSViewer.exe: build Release and run deploy.ps1 first.
#endif

[Setup]
; AppId identifies VGS Viewer for upgrades and uninstall; never reuse it for another program.
AppId={{F80F7745-0B27-4757-BC48-EDFE737490B2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
VersionInfoVersion={#MyAppVersionNum}
VersionInfoProductVersion={#MyAppVersionNum}
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Copyright (c) 2026 Victor M. Feliz. All rights reserved.
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (c) 2026 Victor M. Feliz. All rights reserved.
DefaultDirName={autopf}\VGS Viewer
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
SetupIconFile=..\assets\gracia\logo.ico
LicenseFile=..\..\decoder\LICENSE.md
; 64-bit only, installed in the 64-bit Program Files.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
DisableProgramGroupPage=yes
; Close a running viewer before replacing its files.
CloseApplications=yes
OutputDir={#MyAppOutputPath}
OutputBaseFilename={#MyAppInstallerFile}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocvgs"; Description: "VGS captures (.vgs)"; GroupDescription: "Open these files with VGS Viewer by default:"
Name: "assocpgs"; Description: "PGS captures (.pgs)"; GroupDescription: "Open these files with VGS Viewer by default:"

[Files]
Source: "{#MyAppPathToInclude}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppPathToInclude}\*"; DestDir: "{app}"; Excludes: "{#MyAppExeName}"; Flags: ignoreversion recursesubdirs createallsubdirs
; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Registry]

; Captures: a ProgID each, offered in "Open with" whatever is chosen; the default only when
; its task is ticked. Windows keeps a choice the user made themselves in "Open with", so
; there it may still ask once which program to use.
Root: HKA; Subkey: "Software\Classes\VGSViewer.vgs"; ValueType: string; ValueName: ""; ValueData: "VGS Capture"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\VGSViewer.vgs\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\VGSViewer.vgs\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.vgs\OpenWithProgids"; ValueType: string; ValueName: "VGSViewer.vgs"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.vgs"; ValueType: string; ValueName: ""; ValueData: "VGSViewer.vgs"; Flags: uninsdeletevalue; Tasks: assocvgs

Root: HKA; Subkey: "Software\Classes\VGSViewer.pgs"; ValueType: string; ValueName: ""; ValueData: "PGS Capture"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\VGSViewer.pgs\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\VGSViewer.pgs\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.pgs\OpenWithProgids"; ValueType: string; ValueName: "VGSViewer.pgs"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.pgs"; ValueType: string; ValueName: ""; ValueData: "VGSViewer.pgs"; Flags: uninsdeletevalue; Tasks: assocpgs


[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
