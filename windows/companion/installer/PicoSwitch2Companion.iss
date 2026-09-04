; PicoSwitch2 Companion -- Windows installer
;
; Built by ../build.ps1 -Installer, which passes the payload directory, the
; version and the output location in. Nothing here is developer-specific and
; nothing is hard-coded to a drive letter.
;
; REQUIRES INNO SETUP 6.3 OR LATER (ArchitecturesAllowed=x64compatible was added
; in 6.3). Built and tested against 6.7.3.
;
; ---------------------------------------------------------------------------
; What this installs
;
; The ordinary unpackaged, full-trust WinUI 3 desktop build -- the same binaries
; the portable ZIP carries. No MSIX, no AppContainer, no packaged identity: an
; installer is a delivery mechanism and does not change what the application is.
;
; The payload is FRAMEWORK-DEPENDENT on .NET but carries the Windows App SDK.
; The two runtimes are deliberately treated differently; see NeedsDotNetRuntime
; below and ../docs/README.md for the measurements behind it.

#define AppName        "PicoSwitch2 Companion"
#define AppPublisher   "PicoSwitch2"
#define AppUrl         "https://github.com/notsosaelin/PicoSwitch2"
#define AppExeName     "PicoSwitch.Companion.App.exe"

; Supplied by build.ps1: AppVersion, PayloadDir, OutputDir, OutputBaseFilename.
#ifndef AppVersion
  #error AppVersion must be passed with /DAppVersion=... (build.ps1 reads it from Package.appxmanifest)
#endif
#ifndef PayloadDir
  #error PayloadDir must be passed with /DPayloadDir=... (the publish output to install)
#endif

[Setup]
; A PERMANENT identity. Upgrades find the previous install through this, so it
; must never change -- a new AppId would install alongside the old one and leave
; two entries in Installed Apps.
AppId={{7C5AD4ED-2731-417C-B316-058505C7C083}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}

; {autopf} follows the scope the user picks on the very first page:
;   all users -> {commonpf}  = %ProgramFiles%\PicoSwitch2 Companion
;   just me   -> {userpf}    = %LOCALAPPDATA%\Programs\PicoSwitch2 Companion
; Both are resolved by Windows rather than assumed, so neither assumes C:.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}

; THE PRIVILEGE MODEL, and the point of this file.
;
; `lowest` means Setup does NOT ask for administrator rights to start. The
; override dialog then offers the scope choice, and Inno elevates ONLY if the
; user picks all-users. A free desktop application should not demand elevation
; merely because an all-users option exists (WINDOWS_PASS.md 27.5).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; x64compatible rather than x64: the same binaries run under emulation on ARM64,
; and refusing to install there would be a restriction the app does not have.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Matches SupportedOSPlatformVersion in Directory.Build.props.
MinVersion=10.0.22000

; The repository's real licence, shown because it exists -- not an invented EULA.
LicenseFile={#SourcePath}\..\..\..\LICENSE

WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
DisableProgramGroupPage=yes
AllowNoIcons=yes
ShowLanguageDialog=no
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}

; Offer to close a running copy on upgrade rather than failing on a locked file.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; NO [Tasks] SECTION, deliberately -- see the shortcut page in [Code].
;
; Inno renders [Tasks] in a TNewCheckListBox, which owner-draws its own
; checkboxes. At 200% display scaling on Windows 11 that draw is a few pixels
; left of the control's client area, so every checkbox loses its left border and
; the checked one looks cut in half. Removing the GroupDescription indent did
; not change it; the offset is in the control, not in the layout.
;
; Two ordinary TNewCheckBox controls on a page of our own are drawn by Windows
; and have none of that. The cost is that the shortcut choices are no longer
; addressable through /TASKS= in a silent install, so the two switches below
; replace it.

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; {autoprograms} and {autodesktop} follow the install scope, so a per-user
; install never writes into the all-users Start Menu.
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Check: WantStartMenuIcon
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExeName}"; Check: WantDesktopIcon

; NO [Run] SECTION EITHER.
;
; A `postinstall` entry becomes a row in WizardForm.RunList, which is another
; TNewCheckListBox and clips its checkbox exactly like the tasks list did. The
; Finish page's "Launch PicoSwitch2 Companion" is therefore its own
; TNewCheckBox, created in CurPageChanged and honoured in DeinitializeSetup.

; NOTHING in [UninstallDelete].
;
; User configuration lives in %LOCALAPPDATA%\PicoSwitch2 -- the adapter registry,
; peer history, Amiibo library, Touch Gamepad layouts and settings -- which is
; outside {app} by design (WindowsDocumentStore.DefaultFolderName). Uninstalling
; removes the program and its shortcuts and leaves that untouched, so
; reinstalling finds the user's adapters exactly where they were.

; The prerequisite, as two overridable facts.
;
; Overridable ONLY so the absent-runtime path can be tested without uninstalling
; a runtime from a working machine: a test build passes a channel that cannot
; exist, which forces detection to report "missing" and exercises the download
; and failure handling for real. Shipping builds pass neither and get the values
; below.
#ifndef DotNetChannel
  #define DotNetChannel "9."
#endif
#ifndef DotNetUrl
  #define DotNetUrl "https://aka.ms/dotnet/9.0/windowsdesktop-runtime-win-x64.exe"
#endif

[Code]
const
  DotNetChannel = '{#DotNetChannel}';
  DotNetUrl = '{#DotNetUrl}';

var
  DownloadPage: TDownloadWizardPage;
  RuntimeNeeded: Boolean;
  RuntimeChecked: Boolean;
  ShortcutPage: TWizardPage;
  StartMenuCheck: TNewCheckBox;
  DesktopCheck: TNewCheckBox;
  LaunchCheck: TNewCheckBox;
  InstallCompleted: Boolean;

{ A command-line override for a silent install, replacing what /TASKS= used to
  do. Absent means "leave it at the default", which is what an unattended
  install of a desktop app should get. }
function SwitchValue(const Name: String; const Default: Boolean): Boolean;
var
  I: Integer;
  Param, Prefix, Value: String;
begin
  Result := Default;
  Prefix := '/' + Uppercase(Name) + '=';
  for I := 1 to ParamCount do
  begin
    Param := ParamStr(I);
    if Pos(Prefix, Uppercase(Param)) = 1 then
    begin
      Value := Uppercase(Copy(Param, Length(Prefix) + 1, MaxInt));
      Result := (Value = '1') or (Value = 'YES') or (Value = 'TRUE');
      Exit;
    end;
  end;
end;

{ The checkbox state, or the command line when there is no one to click it.

  WizardSilent is tested FIRST and the nil test second. Inno still runs
  InitializeWizard under /SILENT and /VERYSILENT -- the form is created, just
  never shown -- so the checkboxes exist there and reading .Checked would return
  the hard-coded default and ignore the switch entirely. Observed doing exactly
  that: /STARTMENUICON=0 /DESKTOPICON=1 produced a Start Menu shortcut and no
  Desktop one, the precise opposite of what was asked. }
function WantStartMenuIcon: Boolean;
begin
  if WizardSilent or (StartMenuCheck = nil) then
    Result := SwitchValue('STARTMENUICON', True)
  else
    Result := StartMenuCheck.Checked;
end;

function WantDesktopIcon: Boolean;
begin
  if WizardSilent or (DesktopCheck = nil) then
    Result := SwitchValue('DESKTOPICON', False)
  else
    Result := DesktopCheck.Checked;
end;

{ Is a .NET 9 Desktop Runtime present?

  Detected from the shared-framework directory rather than from the registry,
  because that is the location the host actually probes and it is what
  `dotnet --list-runtimes` reports. There is no documented registry value for
  "a desktop runtime of this major version exists", and inventing one would be
  guessing at Microsoft's layout.

  A .NET 9 application rolls forward across PATCH versions only, so any 9.x
  satisfies it and a 10.x does not. }
function DotNetDesktopRuntimePresent: Boolean;
var
  Root: String;
  Rec: TFindRec;
begin
  Result := False;
  Root := ExpandConstant('{commonpf64}\dotnet\shared\Microsoft.WindowsDesktop.App');
  if not DirExists(Root) then
    Exit;

  if FindFirst(Root + '\*', Rec) then
  begin
    try
      repeat
        if (Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          if Copy(Rec.Name, 1, Length(DotNetChannel)) = DotNetChannel then
          begin
            Result := True;
            Exit;
          end;
      until not FindNext(Rec);
    finally
      FindClose(Rec);
    end;
  end;
end;

{ Cached, because [Run] evaluates its Check for every entry and the answer
  cannot change between the ready page and the end of installation. }
function NeedsDotNetRuntime: Boolean;
begin
  if not RuntimeChecked then
  begin
    RuntimeNeeded := not DotNetDesktopRuntimePresent;
    RuntimeChecked := True;
  end;
  Result := RuntimeNeeded;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage(
    SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), nil);

  { The shortcut choices, on a page of our own with real checkbox controls.
    Placed after the destination page, which is where Select Additional Tasks
    would have been. }
  ShortcutPage := CreateCustomPage(wpSelectDir,
    'Select Shortcuts',
    'Where would you like to be able to start PicoSwitch2 Companion?');

  StartMenuCheck := TNewCheckBox.Create(ShortcutPage);
  StartMenuCheck.Parent := ShortcutPage.Surface;
  StartMenuCheck.Left := 0;
  StartMenuCheck.Top := ScaleY(8);
  StartMenuCheck.Width := ShortcutPage.SurfaceWidth;
  StartMenuCheck.Height := ScaleY(20);
  StartMenuCheck.Caption := 'Create a &Start Menu shortcut';
  StartMenuCheck.Checked := True;

  DesktopCheck := TNewCheckBox.Create(ShortcutPage);
  DesktopCheck.Parent := ShortcutPage.Surface;
  DesktopCheck.Left := 0;
  DesktopCheck.Top := StartMenuCheck.Top + StartMenuCheck.Height + ScaleY(8);
  DesktopCheck.Width := ShortcutPage.SurfaceWidth;
  DesktopCheck.Height := ScaleY(20);
  DesktopCheck.Caption := 'Create a &Desktop shortcut';
  DesktopCheck.Checked := False;
end;

{ The Ready page's summary.

  Inno builds this from the built-in pages, and the shortcut choices now live on
  a custom one -- so without this they would silently vanish from the last
  screen before installing, which is the one screen a careful user reads. }
function UpdateReadyMemo(const Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Shortcuts, Scope: String;
begin
  { Built from WizardDirValue rather than from MemoDirInfo, which Inno leaves
    empty whenever it auto-skips the directory page -- and it does exactly that
    when a previous install is found, which is every upgrade. The destination is
    the one fact this page must never omit. }
  Result := 'Destination:' + NewLine + Space + WizardDirValue + NewLine + NewLine;

  if IsAdminInstallMode then
    Scope := 'All users'
  else
    Scope := 'Me only';
  Result := Result + 'Install for:' + NewLine + Space + Scope + NewLine + NewLine;

  Shortcuts := '';
  if WantStartMenuIcon then
    Shortcuts := Shortcuts + Space + 'Start Menu shortcut' + NewLine;
  if WantDesktopIcon then
    Shortcuts := Shortcuts + Space + 'Desktop shortcut' + NewLine;
  if Shortcuts = '' then
    Shortcuts := Space + 'None' + NewLine;

  Result := Result + 'Shortcuts:' + NewLine + Shortcuts;

  if NeedsDotNetRuntime then
    Result := Result + NewLine + 'Prerequisite:' + NewLine +
      Space + '.NET Desktop Runtime will be downloaded and installed' + NewLine;
end;

{ The Finish page's "launch it now" offer.

  Its own checkbox rather than a [Run] entry with the `postinstall` flag: that
  flag puts a row in WizardForm.RunList, which is a TNewCheckListBox and clips
  its checkbox at 200% scaling in the same way the tasks list did. }
procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID <> wpFinished) or (LaunchCheck <> nil) then
    Exit;

  LaunchCheck := TNewCheckBox.Create(WizardForm);
  LaunchCheck.Parent := WizardForm.FinishedPage;
  LaunchCheck.Left := WizardForm.FinishedLabel.Left;
  LaunchCheck.Top := WizardForm.FinishedLabel.Top +
    WizardForm.FinishedLabel.Height + ScaleY(16);
  LaunchCheck.Width := WizardForm.FinishedLabel.Width;
  LaunchCheck.Height := ScaleY(20);
  LaunchCheck.Caption := 'Launch {#AppName}';
  LaunchCheck.Checked := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then
    InstallCompleted := True;
end;

{ Launched as the ORIGINAL user, which matters when Setup elevated itself for an
  all-users install: inheriting Setup's token would leave PicoSwitch2 Companion
  running as administrator for the rest of the session, and it is an ordinary
  desktop process that has no business being elevated.

  DeinitializeSetup is the hook that runs after Finish is clicked. It also runs
  when Setup is cancelled, hence the completed flag -- cancelling must not start
  an application that was never installed. }
procedure DeinitializeSetup;
var
  Ignored: Integer;
begin
  if not InstallCompleted then Exit;

  { The `skipifsilent` that the [Run] entry used to carry. Without it an
    unattended install starts the application on the way out, which is both a
    surprise and, in a test harness, a running process that blocks the next
    uninstall. Observed doing exactly that. }
  if WizardSilent then Exit;

  if LaunchCheck = nil then Exit;
  if not LaunchCheck.Checked then Exit;

  ShellExecAsOriginalUser('', ExpandConstant('{app}\{#AppExeName}'), '',
    ExpandConstant('{app}'), SW_SHOWNORMAL, ewNoWait, Ignored);
end;

{ The prerequisite, fetched and installed before any application file is copied.

  PrepareToInstall rather than NextButtonClick(wpReady), which was the first
  shape of this and was wrong: NextButtonClick belongs to the wizard, so it does
  not fire under /SILENT or /VERYSILENT and the runtime would simply never be
  installed there. PrepareToInstall runs in both, which is what a prerequisite
  needs.

  Returning a non-empty string aborts the installation and shows it, so a
  machine that cannot get the runtime is left with nothing installed rather than
  with an application that cannot start. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
  Installer: String;
begin
  Result := '';
  if not NeedsDotNetRuntime then
    Exit;

  { Downloaded rather than bundled. The runtime is ~58 MB against a ~21 MB
    installer, so carrying it would more than triple every download for the
    majority of users who already have it. Fetched from Microsoft's own aka.ms
    redirect, which resolves to builds.dotnet.microsoft.com -- no third-party
    mirror, and no runtime DLLs copied into system locations by hand. }
  Installer := ExpandConstant('{tmp}\windowsdesktop-runtime.exe');
  DownloadPage.Clear;
  DownloadPage.Add(DotNetUrl, 'windowsdesktop-runtime.exe', '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
    except
      { NOTE: no line here may BEGIN with '#'. The Inno preprocessor reads a
        leading '#' as a directive, so a wrapped `#13#10` at the start of a line
        fails the compile with "Unknown preprocessor directive". }
      Result := 'The .NET Desktop Runtime could not be downloaded.'
        + #13#10#13#10 + AddPeriod(GetExceptionMessage) + #13#10#13#10
        + 'Install it from https://dotnet.microsoft.com/download/dotnet/9.0'
        + ' and run this installer again.';
      Exit;
    end;
  finally
    DownloadPage.Hide;
  end;

  { The runtime installs MACHINE-WIDE and its own manifest requires
    administrator, so Windows raises the UAC prompt here even when Setup itself
    is running unelevated for a per-user install. That is the one point at which
    a "just me" install can still ask for elevation, and only when the runtime
    is genuinely absent.

    /passive rather than /quiet: with a UAC prompt in play the user is already
    being interrupted, and a silent 58 MB install with no progress looks like a
    hang. }
  WizardForm.StatusLabel.Caption := 'Installing the .NET Desktop Runtime...';
  if not ShellExec('', Installer, '/install /passive /norestart', '',
                   SW_SHOW, ewWaitUntilTerminated, ExitCode) then
  begin
    Result := 'The .NET Desktop Runtime installer could not be started.';
    Exit;
  end;

  { 3010 is "success, a restart is pending" -- not a failure. 1602 is the user
    declining the elevation prompt, which deserves its own sentence rather than
    a bare exit code. }
  if ExitCode = 3010 then
    NeedsRestart := True
  else if ExitCode = 1602 then
    Result := 'Installation of the .NET Desktop Runtime was cancelled.' + #13#10 +
              'PicoSwitch2 Companion cannot start without it.'
  else if ExitCode <> 0 then
    Result := 'The .NET Desktop Runtime installer failed with code ' +
              IntToStr(ExitCode) + '.';
end;
