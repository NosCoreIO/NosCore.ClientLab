# Packet logger

In **Settings → Developer tools**, enable **Open packet logger when the game starts**
and choose **Save settings**. On the next Play, ClientLab opens a separate packet
window. In browser mode, allow launcher popups. The window has Send/Receive
filters, text search, and Clear view (which does not delete the file).
Click a packet to select it; Ctrl/Command-click selects multiple packets. The
right-click menu offers Copy packet (with timestamp/tags), Copy raw text, Copy
selected text, Copy visible packets, and Select all. Ctrl/Command+C and
Ctrl/Command+A also work. Existing rows and selections remain stable as new
traffic arrives. Display timestamps are compact; log files retain the full UTC timestamp.

ClientLab adapts the cleartext capture signatures and login boundary documented
in NosCore.DeveloperTools. Its native Windows DLL injector cannot attach to a
WebAssembly guest. Instead the C# recompiler inserts observation calls at those
same guest instructions, before encryption for world sends and after decryption
for world/login receives. No external injector or elevated process is required.
This option captures packets; it does not send synthetic packets.

Logging is off by default. Each enabled game session creates a UTF-8 JSONL file
under `<ClientLab data directory>/packet-logs`, named
`packets-<UTC start timestamp>-<unique id>.jsonl`. The first line records the
session start; each packet has an ID, UTC timestamp, direction, connection and
raw plaintext, decoded from the client's Windows-1252 bytes (so a `€` in a name stays
`€`). Every line is flushed immediately. Logs can contain account data
and private chat; keep them on your own device. The viewer retains 10,000 entries
in memory; the file retains the full session.

To follow the newest session from PowerShell:

```powershell
$packetLog = Get-ChildItem "$env:LOCALAPPDATA\NosCore.ClientLab\packet-logs\*.jsonl" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
Get-Content -LiteralPath $packetLog.FullName -Tail 20 -Wait
```

On Linux/macOS, use `tail -f <log path>`. Activity and the packet window display
the exact file path. The game host also exposes cursor-paged JSON at
`/packets?since=<last ID>`; command-line HTTP readers should send
`Sec-Fetch-Site: none`. Cross-site web requests remain blocked.

The runtime revision advances once for the instrumentation. Subsequent launches
reuse the compiled runtime; toggling logging does not require another rebuild.
Signature matches must be unique, and the login tokenizer must immediately follow
the documented null check. If a patch changes these, the launcher reports the
unsupported hooks instead of attaching to arbitrary code.

## Plugins

A packet logger plugin replaces the built-in window. In **Settings → Developer tools**,
keep **Open packet logger when the game starts** enabled, choose the plugin's `.dll`
under **Packet logger plugin**, and save. Use **Use built-in** (desktop) or clear the
field (browser mode) to return to the built-in window. Packets are still written to the
JSONL log.

A plugin is a .NET 10 class library that references `NosCore.ClientLab.Plugins.dll`
(shipped next to `ClientLab.exe`; reference it with `<Private>false</Private>`) and has
one public class implementing `IPacketPlugin` with a parameterless constructor.
ClientLab loads it into the launcher process, in its own load context, with dependencies
resolved from the plugin's folder. Nothing is injected into the game. The plugin runs
with your permissions, so only choose plugins you trust.

```csharp
public sealed class MyLogger : IPacketPlugin
{
    private IGameSession? game;
    public Task StartAsync(IGameSession game, CancellationToken token) { this.game = game; return Task.CompletedTask; }
    public void OnPacket(Packet packet) => Console.WriteLine($"{packet.Connection} {packet.Direction} {packet.Raw}");
    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}
```

`OnPacket` receives every captured packet in order on one background thread. `IGameSession`
also lets a plugin:

- find a unique byte pattern in the client executable's code (`FindCode`, `??` for any byte);
- read and write guest memory at the client's own addresses (up to 1 MB per request), and
  read up to 256 ranges in one request (`ReadManyAsync`);
- call a client function with Delphi's register convention (`CallAsync`: EAX, EDX, ECX
  and up to three stack arguments);
- hand a world packet to the client's own send function or receive handler
  (`SendPacketAsync`, `ReceivePacketAsync`). These need the client to have sent or received a
  world packet first, because the function's object is taken from that call. Login packets
  cannot be sent this way. The text is encoded as Windows-1252, like the client's own packets;
  characters it lacks become `?`;
- queue mouse and keyboard input as the player's own (`PostInputAsync`), at positions in the
  game's pixels (`GetScreenSizeAsync`).

Memory, call and packet requests travel from the launcher through the game page to the game
thread. They run between window messages, the same place the client's own input handlers run,
and time out after 30 seconds. The game thread answers about once per frame, so a plugin that
polls several values should read them together. Input and screen-size requests are answered by
the page and do not wait for the game thread. The client's code was compiled ahead of time, so writing over
code bytes does not change what runs. Patch data instead, or call functions.
`StartAsync` runs before the game page exists: do not wait for game requests there.

Checks: `dotnet run --project desktop/ClientLab.Checks -- --packet-logger` (including
plugin loading and the request relay), `node desktop/ClientLab.Checks/plugin-runtime.mjs` and
`node desktop/ClientLab.Checks/game-options-ui.mjs`. Set `CLIENTLAB_TEST_EXE` to
a local client executable to verify all three signatures for that exact build.
`node desktop/ClientLab.Checks/packet-runtime.mjs` checks the actual embedded
capture code and its worker-to-host transport without starting a game.
