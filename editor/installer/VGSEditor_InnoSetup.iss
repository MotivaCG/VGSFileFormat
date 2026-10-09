; SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
; Copyright (c) 2026 Victor M. Feliz. All rights reserved.
;
; Proprietary software owned by Victor M. Feliz.
; ScanMeNow and The4DScanner are licensed for internal use only.
; No ownership, sale, redistribution, sublicensing or modification rights
; are granted. All other rights remain reserved to the copyright holder.
; See LICENSE.md for the limited use grant and applicable terms.
; Other uses require prior written authorisation, subject to mandatory law.

; VGS Editor installer (Inno Setup 6).
;
; It packages the folder deploy.ps1 makes (build\...-Release\deploy): the editor, its Qt
; libraries, the MSVC runtime, the built-in presets and ffmpeg. Run the deploy first, then
; compile this script (Inno Setup IDE, or ISCC.exe VGSEditor_InnoSetup.iss).
;
; The version is not set here: it is read from ..\licensemanagement.h, where the editor
; takes it from too, so the installer and the program always agree.

; ---- Version, from licensemanagement.h (#define VERSION_NAME "v1.0.0") ----
#define VersionHeader AddBackslash(SourcePath) + "..\licensemanagement.h"
#if !FileExists(VersionHeader)
  #error licensemanagement.h not found beside the installer folder
#endif
#define MyAppVersion ""
#define HeaderFile
#define HeaderLine
#define Rest
; A quote is """" in ISPP, and a define inside a #sub is local unless public.
#sub ReadVersionLine
  #define HeaderLine FileRead(HeaderFile)
  #if Pos("VERSION_NAME", HeaderLine) > 0 && Pos("""", HeaderLine) > 0
    #define Rest Copy(HeaderLine, Pos("""", HeaderLine) + 1)
    #define public MyAppVersion Copy(Rest, 1, Pos("""", Rest) - 1)
  #endif
#endsub
#for {HeaderFile = FileOpen(VersionHeader); !FileEof(HeaderFile); 0} ReadVersionLine
#expr FileClose(HeaderFile)
#if MyAppVersion == ""
  #error VERSION_NAME not found in licensemanagement.h
#endif
; "v1.2.3" -> "1.2.3.0", the four numbers Windows shows as the file version.
#define MyAppVersionNum Copy(MyAppVersion, 2) + ".0"

; ---- What and where ----
#define MyAppName "VGS Editor"
#define MyAppExeName "VGSEditor.exe"
#define MyAppPathToInclude AddBackslash(SourcePath) + "..\build\Desktop_Qt_6_8_0_MSVC2022_64bit-Release\deploy"
#define MyAppOutputPath AddBackslash(SourcePath) + "Output"
#define MyAppInstallerFile "VGSEditorInstaller_" + MyAppVersion

#define MyAppPublisher "Victor M. Feliz"
#define MyAppURL "https://www.the4dscanner.com"

; Projects always open in the editor. Captures (.vgs, .pgs, .mint) always appear in Explorer's
; "Open with"; whether the editor becomes what opens them is asked, one checkbox each.
#define MyAppAssocName "VGS Editor Project"
#define MyAppAssocExt ".vgsproj"
#define MyAppAssocKey "VGSEditor.Project"

#if !FileExists(MyAppPathToInclude + "\" + MyAppExeName)
  #error The deploy folder has no VGSEditor.exe: build Release and run deploy.ps1 first.
#endif

[Setup]
; AppId identifies VGS Editor for upgrades and uninstall; never reuse it for another program.
AppId={{36146AC4-9972-4804-A97A-84BB6C959942}
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
DefaultDirName={autopf}\VGS Editor
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
SetupIconFile=..\assets\gracia\logo.ico
LicenseFile=..\LICENSE.md
; 64-bit only, installed in the 64-bit Program Files.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
DisableProgramGroupPage=yes
; Close a running editor before replacing its files.
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
Name: "assocvgs"; Description: "VGS captures (.vgs)"; GroupDescription: "Open these files with VGS Editor by default:"
Name: "assocpgs"; Description: "PGS captures (.pgs)"; GroupDescription: "Open these files with VGS Editor by default:"
Name: "assocmint"; Description: "MINT captures (.mint)"; GroupDescription: "Open these files with VGS Editor by default:"

[InstallDelete]
; An update replaces the built-in presets whole, so one removed from the editor goes too.
; The user's own presets live in their AppData and are never touched.
Type: filesandordirs; Name: "{app}\presets"

[Files]
Source: "{#MyAppPathToInclude}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppPathToInclude}\*"; DestDir: "{app}"; Excludes: "{#MyAppExeName}"; Flags: ignoreversion recursesubdirs createallsubdirs
; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Registry]
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocExt}\OpenWithProgids"; ValueType: string; ValueName: "{#MyAppAssocKey}"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocKey}"; ValueType: string; ValueName: ""; ValueData: "{#MyAppAssocName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocKey}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocKey}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocExt}"; ValueType: string; ValueName: ""; ValueData: "{#MyAppAssocKey}"; Flags: uninsdeletevalue

; Captures: a ProgID each, offered in "Open with" whatever is chosen; the default only when
; its task is ticked. Windows keeps a choice the user made themselves in "Open with", so
; there it may still ask once which program to use.
Root: HKA; Subkey: "Software\Classes\VGSEditor.vgs"; ValueType: string; ValueName: ""; ValueData: "VGS Capture"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\VGSEditor.vgs\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\VGSEditor.vgs\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.vgs\OpenWithProgids"; ValueType: string; ValueName: "VGSEditor.vgs"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.vgs"; ValueType: string; ValueName: ""; ValueData: "VGSEditor.vgs"; Flags: uninsdeletevalue; Tasks: assocvgs

Root: HKA; Subkey: "Software\Classes\VGSEditor.pgs"; ValueType: string; ValueName: ""; ValueData: "PGS Capture"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\VGSEditor.pgs\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\VGSEditor.pgs\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.pgs\OpenWithProgids"; ValueType: string; ValueName: "VGSEditor.pgs"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.pgs"; ValueType: string; ValueName: ""; ValueData: "VGSEditor.pgs"; Flags: uninsdeletevalue; Tasks: assocpgs

Root: HKA; Subkey: "Software\Classes\VGSEditor.mint"; ValueType: string; ValueName: ""; ValueData: "MINT Capture"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\VGSEditor.mint\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\VGSEditor.mint\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\.mint\OpenWithProgids"; ValueType: string; ValueName: "VGSEditor.mint"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.mint"; ValueType: string; ValueName: ""; ValueData: "VGSEditor.mint"; Flags: uninsdeletevalue; Tasks: assocmint

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
