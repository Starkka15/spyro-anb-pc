; Setup for The Legend of Spyro: A New Beginning -- PC port.
;
; Installs the two programs per user (no administrator rights, and the
; settings files beside them stay writable), then copies the game from the
; player's own copy: a folder or disc drive, or an Xbox disc image. The copy
; is done by the launcher (spyro_launcher.exe /import), which reads Xbox
; disc images and checks the game's version; Setup only asks where it is.
;
;   G:\InnoSetup\ISCC.exe installer\spyro.iss      -> release\

#define AppVersion "1.0.3"
#define AppTitle "The Legend of Spyro - A New Beginning"

[Setup]
AppId={{8C1B6E0B-5D3A-4F7E-9B7E-2A6F1D3C4E51}
AppName=The Legend of Spyro: A New Beginning (PC port)
AppVersion={#AppVersion}
AppVerName=The Legend of Spyro: A New Beginning (PC port) {#AppVersion}
DefaultDirName={autopf}\Spyro A New Beginning
DefaultGroupName={#AppTitle}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ExtraDiskSpaceRequired=2100000000
OutputDir=..\release
OutputBaseFilename=SpyroANB-PC-{#AppVersion}-Setup
SetupIconFile=..\res\spyro.ico
UninstallDisplayIcon={app}\spyro_launcher.exe
UninstallDisplayName=The Legend of Spyro: A New Beginning (PC port)
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\spyro_anb_recomp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\spyro_launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "LICENSES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppTitle}"; Filename: "{app}\spyro_launcher.exe"
Name: "{group}\Play (skip launcher)"; Filename: "{app}\spyro_anb_recomp.exe"
Name: "{group}\Read me"; Filename: "{app}\README.txt"
Name: "{autodesktop}\{#AppTitle}"; Filename: "{app}\spyro_launcher.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\spyro_launcher.exe"; Description: "Open the launcher"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The copied game and what the programs write beside themselves. Saves are
; elsewhere (%LOCALAPPDATA%\xboxrecomp) and stay.
Type: filesandordirs; Name: "{app}\game_files"
Type: files; Name: "{app}\spyro.ini"
Type: files; Name: "{app}\mkb.ini"
Type: files; Name: "{app}\spyro.log"
Type: files; Name: "{app}\xbox_kernel.log"
Type: files; Name: "{app}\crash.txt"

[Code]
var
  SourcePage: TInputOptionWizardPage;
  FolderPage: TInputDirWizardPage;
  ImagePage: TInputFileWizardPage;
  SourceChosen: Boolean;

procedure InitializeWizard;
begin
  SourcePage := CreateInputOptionPage(wpSelectDir,
    'Game files', 'Where is your copy of the game?',
    'This port contains none of the game. Its files (about 2 GB) are copied from your own copy of ' +
    'The Legend of Spyro: A New Beginning for the original Xbox.',
    True, False);
  SourcePage.Add('From a folder with the game''s files, or the disc in a drive that can read it');
  SourcePage.Add('From an Xbox disc image (.iso, full or trimmed)');
  SourcePage.Add('Later, from the launcher (or keep the game files already installed)');
  SourcePage.SelectedValueIndex := 0;

  FolderPage := CreateInputDirPage(SourcePage.ID,
    'Game files', 'Choose the game''s folder or disc drive',
    'The folder with default.xbe in it, or the drive the disc is in.', False, '');
  FolderPage.Add('');

  ImagePage := CreateInputFilePage(FolderPage.ID,
    'Game files', 'Choose the disc image', 'An image of the game''s Xbox disc.');
  ImagePage.Add('Disc image:', 'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files|*.*', '.iso');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  { Reinstalling over a copy that has the game already: keep it. }
  if (CurPageID = SourcePage.ID) and not SourceChosen then begin
    SourceChosen := True;
    if FileExists(ExpandConstant('{app}\game_files\default.xbe')) then
      SourcePage.SelectedValueIndex := 2;
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  { /GAMESRC or a silent install: no questions about the game files. }
  if (PageID = SourcePage.ID) or (PageID = FolderPage.ID) or (PageID = ImagePage.ID) then
    if WizardSilent or (ExpandConstant('{param:GAMESRC|}') <> '') then begin
      Result := True;
      Exit;
    end;
  if PageID = FolderPage.ID then
    Result := SourcePage.SelectedValueIndex <> 0;
  if PageID = ImagePage.ID then
    Result := SourcePage.SelectedValueIndex <> 1;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = FolderPage.ID) and not FileExists(AddBackslash(FolderPage.Values[0]) + 'default.xbe') then begin
    MsgBox('There is no default.xbe there. Choose the top folder of the game, or the drive the disc is in.',
           mbError, MB_OK);
    Result := False;
  end;
  if (CurPageID = ImagePage.ID) and not FileExists(ImagePage.Values[0]) then begin
    MsgBox('Choose the disc image file.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Src: String;
  Code: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;
  { /GAMESRC="folder or image" picks the source without the pages, for a
    silent install; a silent install without it leaves the game for later. }
  Src := ExpandConstant('{param:GAMESRC|}');
  if Src = '' then begin
    if WizardSilent then
      Exit;
    case SourcePage.SelectedValueIndex of
      0: Src := FolderPage.Values[0];
      1: Src := ImagePage.Values[0];
    else
      Exit;
    end;
  end;
  { A trailing backslash would escape the closing quote ("E:\"). }
  if Copy(Src, Length(Src), 1) = '\' then
    Src := Src + '\';
  WizardForm.StatusLabel.Caption := 'Copying the game files from your copy of the game...';
  if not Exec(ExpandConstant('{app}\spyro_launcher.exe'), '/import "' + Src + '"', ExpandConstant('{app}'),
              SW_SHOW, ewWaitUntilTerminated, Code) or (Code <> 0) then
    MsgBox('The game files were not installed. You can install them later with "Install game files..." ' +
           'in the launcher.', mbInformation, MB_OK);
end;
