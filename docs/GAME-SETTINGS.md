# Game preferences

The client uses `Config.ini` beside its executable for game preferences. Analysis of
the client confirms `EW_Sound_Config` keys `BGMSoundOff`, `FxSoundOff`, and
`3DSoundOff`, plus display keys such as `FullScreen`, `ScreenSizeMode`, `BitsPerPel`,
and `Freq` under `EW_Multi_Navigator`. `NostaleEU.ini` is a separate binary file;
it is not the music/preferences INI.

The portable filesystem previously materialized modified game files in memory and
discarded them when the worker ended. The registry implementation is also in memory,
but it is not the persistence mechanism for the confirmed audio/display keys.

The C# game host now stores the original INI bytes at
`<ClientLab data directory>/game-settings/Config.ini`. On Windows with the default
data directory this is `%LOCALAPPDATA%\NosCore.ClientLab\game-settings\Config.ini`.
The file is shared by desktop/browser launches and survives client updates because
it is outside the version directories. The original client installation is not modified.

On first use, an existing `Config.ini` is imported from the selected installation or
the current game directory. Windows VirtualStore is also checked for that selected
installation. Once ClientLab has saved preferences, later patches/imports do not
replace them. Comments, unknown keys, encoding, and line endings are preserved;
the launcher does not translate the INI into another settings format.

The worker restores the file before entering the game and sends completed writes
or atomic replacements to the host. Only this settings file is persisted. Failed
saves keep the game running and preserve the previous durable file; a later save
can retry. A file lock coordinates writers and replacement is atomic.

Checks that do not require a running game or network listener:

```powershell
dotnet run --project desktop/ClientLab.Checks -- --game-settings
node desktop/ClientLab.Checks/game-settings-worker.mjs
node desktop/ClientLab.Checks/game-options-ui.mjs
```

Open **Settings → Game preferences** in the desktop or browser launcher to change
Music, Sound effects, 3D sound, and Full screen. These switches save automatically;
the confirmation appears beside them. **Save settings** is still used for the other
launcher settings.
The launcher Save button stays available during preparation and gameplay.
An active preparation keeps its original settings snapshot; file and tool changes
are used by the next preparation, and launch options by the next game session.
Changes apply on the next game launch; close the game before editing them.
The desktop game window also honors the saved fullscreen choice. Browser windows
remain subject to the browser's fullscreen controls.

The launcher edits only those four official INI keys, preserving other keys,
comments, encoding, and line endings. In-game options still work: confirm/apply
them so the client writes `Config.ini`, and the launcher reads them when Settings
is reopened.
