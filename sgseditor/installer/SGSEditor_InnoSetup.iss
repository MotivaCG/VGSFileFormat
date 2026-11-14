; SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
; Copyright (c) 2026 Victor M. Feliz. All rights reserved.

; SGS Editor installer (Inno Setup 6).
;
; It packages the folder deploy.ps1 makes (build\...-Release\deploy): the editor, its Qt
; libraries, the MSVC runtime, the VGS Editor licence and gs2mesh with its CUDA and ONNX
; libraries (about 5 GB, so the installer comes in parts of up to 2 GB beside setup.exe).
; Run the deploy first, then compile this script (Inno Setup IDE, or ISCC.exe
; SGSEditor_InnoSetup.iss; for a CMake build add /DMyAppPathToInclude=<build>\deploy).
;
; The version is not set here: it is read from ..\sgsversion.h, where the editor takes it
; from too, so the installer and the program always agree.

; ---- Version, from sgsversion.h (#define SGS_EDITOR_VERSION_NAME "v1.0.0") ----
#define VersionHeader AddBackslash(SourcePath) + "..\sgsversion.h"
#if !FileExists(VersionHeader)
  #error sgsversion.h not found beside the installer folder
#endif
#define MyAppVersion ""
#define HeaderFile
#define HeaderLine
#define Rest
; A quote is """" in ISPP, and a define inside a #sub is local unless public.
#sub ReadVersionLine
  #define HeaderLine FileRead(HeaderFile)
  #if Pos("SGS_EDITOR_VERSION_NAME", HeaderLine) > 0 && Pos("""", HeaderLine) > 0
    #define Rest Copy(HeaderLine, Pos("""", HeaderLine) + 1)
    #define public MyAppVersion Copy(Rest, 1, Pos("""", Rest) - 1)
  #endif
#endsub
#for {HeaderFile = FileOpen(VersionHeader); !FileEof(HeaderFile); 0} ReadVersionLine
#expr FileClose(HeaderFile)
#if MyAppVersion == ""
  #error SGS_EDITOR_VERSION_NAME not found in sgsversion.h
#endif
; "v1.2.3" -> "1.2.3.0", the four numbers Windows shows as the file version.
#define MyAppVersionNum Copy(MyAppVersion, 2) + ".0"

; ---- What and where ----
#define MyAppName "SGS Editor"
#define MyAppExeName "SGSEditor.exe"
#ifndef MyAppPathToInclude
  #define MyAppPathToInclude AddBackslash(SourcePath) + "..\build\Desktop_Qt_6_8_0_MSVC2022_64bit-Release\deploy"
#endif
#define MyAppOutputPath AddBackslash(SourcePath) + "Output"
#define MyAppInstallerFile "SGSEditorInstaller_" + MyAppVersion

#define MyAppPublisher "VGS"
#define MyAppURL "https://www.the4dscanner.com"


#if !FileExists(MyAppPathToInclude + "\" + MyAppExeName)
  #error The deploy folder has no SGSEditor.exe: build Release and run deploy.ps1 first.
#endif

#if !FileExists(MyAppPathToInclude + "\gs2mesh\gs2mesh.dll")
  #error The deploy folder has no gs2mesh: build and install gs2mesh, then run deploy.ps1 again.
#endif

[Setup]
; AppId identifies SGS Editor for upgrades and uninstall; never reuse it for another program.
AppId={{04C952B7-2246-489A-BF29-59111FD7BC30}
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
DefaultDirName={autopf}\SGS Editor
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
SetupIconFile=..\..\editor\assets\gracia\logo.ico
LicenseFile=..\..\editor\LICENSE.md
; 64-bit only, installed in the 64-bit Program Files.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
DisableProgramGroupPage=yes
; Close a running editor before replacing its files.
CloseApplications=yes
OutputDir={#MyAppOutputPath}
OutputBaseFilename={#MyAppInstallerFile}
; gs2mesh's CUDA libraries make the setup larger than one file may be: parts beside setup.exe.
DiskSpanning=yes
DiskSliceSize=max
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=8
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocproject"; Description: "SGS projects (.sgsproj)"; GroupDescription: "Open these files with SGS Editor by default:"

[Files]
Source: "{#MyAppPathToInclude}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppPathToInclude}\*"; DestDir: "{app}"; Excludes: "{#MyAppExeName}"; Flags: ignoreversion recursesubdirs createallsubdirs
; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Registry]
; Projects: the editor's own, by default when the task is ticked.
Root: HKA; Subkey: "Software\Classes\SGSEditor.sgsproj"; ValueType: string; ValueName: ""; ValueData: "SGS Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\SGSEditor.sgsproj\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\SGSEditor.sgsproj\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.sgsproj\OpenWithProgids"; ValueType: string; ValueName: "SGSEditor.sgsproj"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.sgsproj"; ValueType: string; ValueName: ""; ValueData: "SGSEditor.sgsproj"; Flags: uninsdeletevalue; Tasks: assocproject
; PLYs: offered in "Open with" only; other programs open them too.
Root: HKA; Subkey: "Software\Classes\SGSEditor.ply"; ValueType: string; ValueName: ""; ValueData: "Gaussian Splatting PLY"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\SGSEditor.ply\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.ply\OpenWithProgids"; ValueType: string; ValueName: "SGSEditor.ply"; ValueData: ""; Flags: uninsdeletevalue

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
