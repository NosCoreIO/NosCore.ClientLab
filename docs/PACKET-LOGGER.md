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
raw plaintext. Every line is flushed immediately. Logs can contain account data
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

Checks: `dotnet run --project desktop/ClientLab.Checks -- --packet-logger` and
`node desktop/ClientLab.Checks/game-options-ui.mjs`. Set `CLIENTLAB_TEST_EXE` to
a local client executable to verify all three signatures for that exact build.
`node desktop/ClientLab.Checks/packet-runtime.mjs` checks the actual embedded
capture code and its worker-to-host transport without starting a game.
