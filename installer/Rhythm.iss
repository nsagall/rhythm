; ---------------------------------------------------------------------------
; Rhythm - Inno Setup wizard script
; ---------------------------------------------------------------------------
; Packages the game (Rhythm.exe) and the chart editor (RhythmEditor.exe) from
; a staged install tree into a single self-contained RhythmSetup-x.y.z.exe.
;
; The staged tree is produced by CMake's install() rules (see CMakeLists.txt):
;
;     cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
;     cmake --build build
;     cmake --install build --prefix build/stage
;
; then this script is compiled against build/stage. installer/build-installer.ps1
; does all of that in one step; `cmake --build build --target installer` also
; works when Inno Setup 6 is installed.
;
; Overridable defines (pass with ISCC /DName=Value):
;   AppVersion  version string baked into the installer   (default 1.0.0)
;   StageDir    folder holding the staged files to ship   (default ..\build\stage)
;   OutputDir   where the setup .exe is written           (default ..\build\installer)
; ---------------------------------------------------------------------------

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\build\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\installer"
#endif

#define AppName "Rhythm"
#define AppPublisher "Nick Sagall"
#define GameExe "Rhythm.exe"
#define EditorExe "RhythmEditor.exe"

; Fail early with a clear message instead of building an empty installer.
#if !FileExists(AddBackslash(StageDir) + GameExe)
  #error Staged build not found. Run: cmake --install build --prefix build/stage
#endif

[Setup]
; AppId uniquely identifies this app for upgrades/uninstall - never change it.
AppId={{8A8269E2-76D3-4716-B676-D4480E7FE865}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#GameExe}
UninstallDisplayName={#AppName} {#AppVersion}
; Per-user install by default: no admin prompt, lands in %LOCALAPPDATA%\Programs.
; The user can still elevate and install machine-wide from the first page.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 7 SP1 and up. The apps carry their own XAudio 2.9 + D3DCompiler
; (staged beside the exes) for 7/8.1, where those aren't in-box; on 10/11 the
; OS copies are used instead. See the redistributable block in CMakeLists.txt.
MinVersion=6.1sp1
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=RhythmSetup-{#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut for Rhythm"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopicon_editor"; Description: "Create a desktop shortcut for the Rhythm &Editor"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything CMake staged: both exes, Content\, Colors.ini, runtime DLLs.
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Rhythm";        Filename: "{app}\{#GameExe}";   WorkingDir: "{app}"; Comment: "Play Rhythm"
Name: "{group}\Rhythm Editor"; Filename: "{app}\{#EditorExe}"; WorkingDir: "{app}"; Comment: "Author and edit charts"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Rhythm";        Filename: "{app}\{#GameExe}";   WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\Rhythm Editor"; Filename: "{app}\{#EditorExe}"; WorkingDir: "{app}"; Tasks: desktopicon_editor

[Run]
Filename: "{app}\{#GameExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Files the apps generate next to themselves at runtime (Dear ImGui layout
; cache) - not tracked by the installer, so remove them explicitly and drop
; the folder if nothing else is left. Player data (settings, high scores)
; lives in %APPDATA%\Rhythm and is intentionally preserved.
Type: files; Name: "{app}\imgui.ini"
Type: dirifempty; Name: "{app}\Content"
Type: dirifempty; Name: "{app}"
