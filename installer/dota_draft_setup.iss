#define appname "Dota_Drafter"
#define version "0.6.11"
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
LicenseFile=C:\Users\ANDREY\Documents\dota_drafter\LICENSE

Compression=lzma
SolidCompression=yes

[Languages]

Name: "english"; MessagesFile: "compiler:Default.isl";

[Tasks]

Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "quicklaunchicon"; Description: "Create Quick Launch shortcut"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]

; assets\*.png копируется по маске — Inno Setup никогда не удаляет файлы,
; пропавшие из источника (переименованный/убранный герой), поэтому папку
; полностью очищаем перед копированием, чтобы не копились устаревшие PNG.
Type: filesandordirs; Name: "{app}\assets"

[Dirs]

; Папка для кнопки [screenshot] (DrawDraftPanel): 10 регионов героев + 10
; регионов позиций сохраняются сюда при клике — для диагностики ошибок
; распознавания драфта. Создаётся пустой, приложение сохраняет в неё по требованию.
Name: "{app}\screenshots"

[Files]

Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\Dota_Drafter.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\catboostmodel.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\draft_helper_abstract_data.db"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\app_icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\draft_helper_abstract.cbm"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\hero_hashes.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\finalapp\assets\*.png"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "C:\Users\ANDREY\Documents\dota_drafter\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

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

// Ищет папку "...\dota 2 beta\game\dota\cfg" среди ВСЕХ библиотек Steam, а не
// только под путём установки самого Steam. Dota 2 (30+ ГБ) очень часто стоит
// в отдельной библиотеке на другом диске (steamapps\libraryfolders.vdf), и путь
// установки Steam из реестра тогда указывает не туда, где на самом деле лежит игра.
function FindDotaCfgFolder(): String;
var
  SteamPath, VdfPath, Candidate, LibPath, Line: String;
  Lines: TArrayOfString;
  I, PStart, PEnd: Integer;
begin
  Result := '';

  if not (RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Valve\Steam', 'InstallPath', SteamPath) or
          RegQueryStringValue(HKEY_CURRENT_USER, 'SOFTWARE\Valve\Steam', 'SteamPath', SteamPath)) then
    exit;

  StringChangeEx(SteamPath, '/', '\', True);

  // Кандидат 1: дефолтная библиотека (папка установки самого Steam)
  Candidate := SteamPath + '\steamapps\common\dota 2 beta\game\dota\cfg';
  if DirExists(Candidate) then
  begin
    Result := Candidate;
    exit;
  end;

  // Кандидаты 2..N: остальные библиотеки из steamapps\libraryfolders.vdf
  VdfPath := SteamPath + '\steamapps\libraryfolders.vdf';
  if not FileExists(VdfPath) then
    exit;
  if not LoadStringsFromFile(VdfPath, Lines) then
    exit;

  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Lines[I];
    if Pos('"path"', Line) > 0 then
    begin
      PStart := Pos('"path"', Line) + Length('"path"');
      while (PStart <= Length(Line)) and (Line[PStart] <> '"') do
        PStart := PStart + 1;
      PStart := PStart + 1;
      PEnd := PStart;
      while (PEnd <= Length(Line)) and (Line[PEnd] <> '"') do
        PEnd := PEnd + 1;
      if PEnd > PStart then
      begin
        LibPath := Copy(Line, PStart, PEnd - PStart);
        StringChangeEx(LibPath, '\\', '\', True);
        Candidate := LibPath + '\steamapps\common\dota 2 beta\game\dota\cfg';
        if DirExists(Candidate) then
        begin
          Result := Candidate;
          exit;
        end;
      end;
    end;
  end;
end;

function GetDotaCfgPath(Param: String): String;
var
  CfgFolder: String;
begin
  CfgFolder := FindDotaCfgFolder();
  if CfgFolder <> '' then
    Result := CfgFolder + '\gamestate_integration'
  else
    // Dota 2 не найдена — Check: DotaCfgPathExists и так пропустит копирование
    // этого файла. Но DestDir вычисляется через {code:...} независимо от Check,
    // и порядок их вычисления Inno Setup не документирует — если бы Result был
    // '', это невалидный путь для DestDir и мог бы прервать всю установку.
    // Отдаём заведомо существующую {app} как безопасную заглушку: копирования
    // всё равно не будет, а если вдруг и произойдёт — ничего не сломает.
    Result := ExpandConstant('{app}');
end;

function DotaCfgPathExists(): Boolean;
begin
  // Проверяем "...\cfg" (доказывает, что Dota 2 реально там установлена), а не
  // "...\cfg\gamestate_integration" — эту подпапку Steam/Dota не создаёт заранее
  // (она появляется, только если у пользователя уже стоял другой GSI-инструмент),
  // а Inno Setup создаст её сам при копировании файла в DestDir.
  Result := FindDotaCfgFolder() <> '';
end;

function IsSilentInstall(): Boolean;
begin
  Result := WizardSilent();
end;