# Game preferences

The client keeps its preferences in two files beside its executable:

- `Config.ini`, which it only reads at start. Analysis of the client confirms
  `EW_Sound_Config` keys `BGMSoundOff`, `FxSoundOff`, and `3DSoundOff`, plus display
  keys such as `FullScreen`, `ScreenSizeMode`, `BitsPerPel`, and `Freq` under
  `EW_Multi_Navigator`. The official launcher writes it; the client never does.
- `Config.dat`, a 1 KB binary block holding the in-game options (sound volumes and
  the rest of the options window). The client loads it at start and writes it only
  while shutting down, when the game object is destroyed.

`NostaleEU.ini` is a separate binary file; it is not a preferences file. The client
reads a few registry values but never writes any, so the in-memory registry does not
lose settings.

The C# game host stores the original bytes of both files in
`<ClientLab data directory>/game-settings/`. On Windows with the default data
directory this is `%LOCALAPPDATA%\NosCore.ClientLab\game-settings\`. The files are
shared by desktop/browser launches and survive client updates because they are
outside the version directories. The original client installation is not modified.

On first use, each file is imported from the selected installation or the current
game directory if it exists there. Windows VirtualStore is also checked for that
selected installation. Once ClientLab has saved a file, later patches/imports do not
replace it. Comments, unknown keys, encoding, and line endings are preserved; the
launcher does not translate the INI into another settings format.

The worker restores the files before entering the game and sends completed writes
or atomic replacements to the host. Only these settings files are persisted.

Because `Config.dat` is written at shutdown, the desktop game window's close button
behaves like the native one: the game asks whether to quit, saves its options when
the player confirms, and the window closes once the game has exited. Closing the
window again while the game asks closes it at once, without saving. In a browser
tab, leave through the game's own exit so the client gets to save; closing the tab
ends the client without a shutdown. Failed
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
comments, encoding, and line endings. The client does not write these flags itself,
so they only change from the launcher (or the official launcher before import).
