; Axial for Windows installer. Built by CI:
;   iscc /DAppVersion=x.y.z /DSourceRoot=<repository> installer\Axial.iss
; Expects build\x64\bin\Release (native x64), build\x86\bin\Release (32-bit
; adapters) and out\app (published settings app).

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceRoot
  #define SourceRoot ".."
#endif
#define Native SourceRoot + "\build\x64\bin\Release"
#define Native32 SourceRoot + "\build\x86\bin\Release"
#define App SourceRoot + "\out\app"
#define Icon SourceRoot + "\app\Axial\Assets\Axial.ico"

[Setup]
AppId={{6B1E7E0A-58C4-4D7B-9C1B-3A9D2F0E5A41}
AppName=Axial
AppVersion={#AppVersion}
AppVerName=Axial {#AppVersion}
AppPublisher=Axial contributors
AppPublisherURL=https://github.com/dewi-ny-je/spacepilot-windows
DefaultDirName={autopf}\Axial
DisableProgramGroupPage=yes
DisableDirPage=auto
LicenseFile={#SourceRoot}\LICENSE
OutputDir={#SourceRoot}\out\installer
OutputBaseFilename=Axial-{#AppVersion}-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; AF_UNIX sockets need Windows 10 1803; WPF on .NET 8 needs 1607.
MinVersion=10.0.17763
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\Axial.exe
#if FileExists(Icon)
SetupIconFile={#Icon}
#endif

[Files]
Source: "{#App}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Native}\axial-service.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Native}\axialctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Native}\axial-web-setup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Native}\axial-bridge.dll"; DestDir: "{app}"; Flags: ignoreversion
; Compatibility DLLs on the system search path, for 64-bit and 32-bit clients.
; CAD applications may hold them open; replace on restart in that case.
Source: "{#Native}\siappdll.dll"; DestDir: "{sys}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Native}\TDxNavLib.dll"; DestDir: "{sys}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Native32}\siappdll.dll"; DestDir: "{syswow64}"; Flags: ignoreversion restartreplace uninsrestartdelete 32bit
Source: "{#Native32}\TDxNavLib.dll"; DestDir: "{syswow64}"; Flags: ignoreversion restartreplace uninsrestartdelete 32bit
Source: "{#SourceRoot}\LICENSE"; DestDir: "{app}\licenses"; DestName: "Axial-LICENSE.txt"
Source: "{#SourceRoot}\third_party\*"; DestDir: "{app}\licenses"; Excludes: "source\*,navlib\*,*.h"; Flags: recursesubdirs
Source: "{#SourceRoot}\docs\events.md"; DestDir: "{app}\docs"

[Icons]
Name: "{autoprograms}\Axial"; Filename: "{app}\Axial.exe"

[Run]
Filename: "{app}\Axial.exe"; Description: "Open Axial settings"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
; Removes the per-user web credentials and the CurrentUser root certificate.
Filename: "{app}\axial-web-setup.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "AxialWebSetup"

[Code]
procedure StopAxial;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM Axial.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM axial-service.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopAxial;
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    StopAxial;
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Axial');
  end;
end;
