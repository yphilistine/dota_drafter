#define appname "Dota_Drafter"
#define version "0.1.2"
#define url "https://github.com/yphilistine/dota_drafter"
#define exename "Dota_Drafter.exe"

[Setup]

AppId={{F8CC8F61-22AA-49A3-A0F3-920B96F2BE00}
AppName={#appname}
AppVersion={#version}
AppPublisherUrl={#url}
AppSupportUrl={#url}
AppUpdatesUrl={#url}

DefaultDirName={localappdata}\{#appname}
PrivilegesRequired=lowest
DefaultGroupName={#appname}

OutputDir=C:\Users\ANDREY\Documents\dota_drafter\installer
OutputBaseFileName=dota_drafter_setup

SetupIconFile=C:\Users\ANDREY\Documents\dota_drafter\finalapp\app_icon.ico

Compression=lzma
SolidCompression=yes

[Languages]

Name: "english"; MessagesFile: "compiler:Default.isl";

[Tasks]

Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "quicklaunchicon"; Description: "Create Quick Launch shortcut"; GroupDescription: "{cm:AdditionalIcons}"

[Files]

Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\Dota_Drafter.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\catboostmodel.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\draft_helper_abstract_data.db"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\app_icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\draft_helper_abstract.cbm"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\hero_hashes.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\assets\*.png"; DestDir: "{app}\assets"; Flags: ignoreversion

Source: "C:\Users\ANDREY\Documents\dota_drafter\installer\gamestate_integration_dota2.cfg"; DestDir: "{code:GetDotaCfgPath}"; Flags: ignoreversion; Check: DotaCfgPathExists

[Icons]

Name: "{group}\{#appname}"; Filename: "{app}\{#exename}"; IconFilename: "{app}\app_icon.ico"
Name: "{userdesktop}\{#appname}"; Filename: "{app}\{#exename}"; IconFilename: "{app}\app_icon.ico"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#appname}"; Filename: "{app}\{#exename}"; IconFilename: "{app}\app_icon.ico"; Tasks: quicklaunchicon

[Run]

Filename: "{app}\{#exename}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\{#exename}"; Flags: nowait skipifdoesntexist; Check: IsSilentInstall

[UninstallDelete]

Type: filesandordirs; Name: "{app}"

[Code]

function GetDotaCfgPath(Param: String): String;
var
  SteamPath: String;
begin
  Result := '';
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Valve\Steam', 'InstallPath', SteamPath) or
     RegQueryStringValue(HKEY_CURRENT_USER, 'SOFTWARE\Valve\Steam', 'SteamPath', SteamPath) then
  begin
    StringChangeEx(SteamPath, '/', '\', True);
    Result := SteamPath + '\steamapps\common\dota 2 beta\game\dota\cfg\gamestate_integration';
  end;
end;

function DotaCfgPathExists(): Boolean;
begin
  Result := DirExists(GetDotaCfgPath(''));
end;

function IsSilentInstall(): Boolean;
begin
  Result := WizardSilent();
end;