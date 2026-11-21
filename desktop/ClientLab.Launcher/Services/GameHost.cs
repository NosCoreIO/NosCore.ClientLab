using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Loopback server for the embedded browser build: the page, the WebAssembly runtime, the user's own
/// game folder (lazily, with HTTP Range support), the Gameforge sign-in endpoints, and a TCP relay the
/// sandboxed worker drives over HTTP because it cannot open sockets itself. Nothing of the client is copied.
/// </summary>
public sealed class GameHost : IAsyncDisposable
{
    private const string DefaultUserAgent = "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 " +
        "(KHTML, like Gecko) Chrome/72.0.3626.121 Safari/537.36";

    private readonly HttpListener listener = new();
    private readonly Relay relay = new();
    private readonly GameforgeAuthentication? auth;
    private readonly string webDir;
    private readonly string buildDir;
    private readonly string gameDir;
    private readonly string clientExe;
    private readonly string provider;
    private readonly string privateServerHost;
    private readonly Dictionary<int, int> privatePortRemap = [];
    private readonly string authority;
    private readonly string originUrl;
    private readonly Action<string>? log;
    private readonly GameSettingsStore? gameSettings;
    private readonly PacketCapture? packets;
    private readonly string settingsImportDirectory;
    private CancellationTokenSource? loop;

    public string Url { get; }

    private GameHost(string root, JsonObject active, JsonObject settings, Action<string>? log, GameSettingsStore? gameSettings)
    {
        this.log = log;
        this.gameSettings = gameSettings;
        settingsImportDirectory = settings["import_dir"]?.GetValue<string>() ?? "";
        webDir = Path.Combine(root, "recomp", "web");
        buildDir = Path.GetFullPath(active["runtime"]!.GetValue<string>());
        gameDir = Path.GetFullPath(active["client"]!.GetValue<string>());
        clientExe = Path.GetFullPath(active["executable"]!.GetValue<string>());
        provider = settings["provider"]?.GetValue<string>() ?? "gameforge";
        privateServerHost = settings["private_server_host"]?.GetValue<string>() ?? "";
        foreach (var mapping in (settings["private_port_remap"]?.GetValue<string>() ?? "").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var pair = mapping.Split('=');
            if (pair.Length != 2 || !int.TryParse(pair[0], out var from) || !int.TryParse(pair[1], out var to) || from is < 1 or > 65535 || to is < 1 or > 65535)
                throw new InvalidOperationException("Private-server port mappings must look like 4000=4000,5000=5000.");
            privatePortRemap[from] = to;
        }
        if (provider == "gameforge") auth = new GameforgeAuthentication(settings);
        var port = FreeLoopbackPort();
        Url = $"http://127.0.0.1:{port}/";
        authority = $"127.0.0.1:{port}";
        originUrl = $"http://127.0.0.1:{port}";
        listener.Prefixes.Add(Url);
        if (settings["packet_logger"]?.GetValue<string>() == "true" && gameSettings is not null)
        {
            packets = new PacketCapture(Path.GetDirectoryName(Path.GetDirectoryName(gameSettings.FilePath))!);
            log?.Invoke("Packet logger attached. Log: " + packets.FilePath);
        }
    }

    public static async Task<GameHost> StartAsync(string root, JsonObject active, JsonObject settings, Action<string>? log = null, GameSettingsStore? gameSettings = null)
    {
        var host = new GameHost(root, active, settings, log, gameSettings);
        try
        {
            host.listener.Start();
            host.loop = new CancellationTokenSource();
            _ = host.AcceptAsync(host.loop.Token);
            return host;
        }
        catch { await host.DisposeAsync(); throw; }
    }

    private static int FreeLoopbackPort()
    {
        var probe = new TcpListener(IPAddress.Loopback, 0);
        probe.Start();
        var port = ((IPEndPoint)probe.LocalEndpoint).Port;
        probe.Stop();
        return port;
    }

    private async Task AcceptAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            HttpListenerContext context;
            try { context = await listener.GetContextAsync(); }
            catch (Exception) when (token.IsCancellationRequested || !listener.IsListening) { return; }
            catch (HttpListenerException) { return; }
            _ = Task.Run(() => HandleAsync(context), token);
        }
    }

    private async Task HandleAsync(HttpListenerContext context)
    {
        try
        {
            // Loopback-bound with a random port, but still reachable by other browsers on this machine, so the
            // relay must only answer our own page — otherwise it is an SSRF primitive. Three layers, none of which
            // the real Chromium/CEF client trips:
            //  - Host: a DNS-rebinding page reaches us under its own hostname, not our loopback authority.
            //  - Sec-Fetch-Site: a forbidden header JS cannot forge, and required (fail closed). Cross-site requests,
            //    including no-cors <img> GETs that send no Origin, carry "cross-site"; our page sends "same-origin"/"none".
            //  - Origin: blocks cross-origin fetches that do send one.
            // The browser-mode launcher opens the page from its own loopback port: a same-site top-level navigation to
            // "/" is allowed, since a navigation cannot read the response.
            if (!string.Equals(context.Request.Headers["Host"], authority, StringComparison.OrdinalIgnoreCase))
            { Reply(context, 403); return; }
            var site = context.Request.Headers["Sec-Fetch-Site"];
            var pageNavigation = site == "same-site" && context.Request.HttpMethod == "GET" && context.Request.Url!.AbsolutePath is "/" or "/packet-log" &&
                context.Request.Headers["Sec-Fetch-Mode"] == "navigate" && context.Request.Headers["Sec-Fetch-Dest"] == "document";
            if (site is not ("same-origin" or "none") && !pageNavigation)
            { Reply(context, 403); return; }
            var origin = context.Request.Headers["Origin"];
            if (origin is not null && !string.Equals(origin, originUrl, StringComparison.Ordinal))
            { Reply(context, 403); return; }
            var path = Uri.UnescapeDataString(context.Request.Url!.AbsolutePath);
            if (path == "/packet-log" && context.Request.HttpMethod == "GET")
            {
                using var page = typeof(GameHost).Assembly.GetManifestResourceStream("PacketLogger.html")!;
                context.Response.ContentType = "text/html; charset=utf-8";
                await page.CopyToAsync(context.Response.OutputStream); return;
            }
            if (path == "/packets")
            {
                if (packets is null) { Reply(context, 404); return; }
                if (context.Request.HttpMethod == "GET")
                {
                    long.TryParse(context.Request.QueryString["since"], out var since);
                    context.Response.ContentType = "application/json";
                    Reply(context, 200, System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(packets.Snapshot(since)), "application/json"); return;
                }
                if (context.Request.HttpMethod != "POST" || context.Request.ContentLength64 is < 0 or > 524288) { Reply(context, 400); return; }
                using var buffer = new MemoryStream();
                await context.Request.InputStream.CopyToAsync(buffer);
                if (buffer.Length > 524288) { Reply(context, 413); return; }
                var packet = JsonNode.Parse(buffer.ToArray())!;
                packets.Add(packet["kind"]!.GetValue<int>(), packet["raw"]!.GetValue<string>(), packet["timestamp"]!.GetValue<DateTimeOffset>());
                Reply(context, 204); return;
            }
            if (path.StartsWith("/game-settings/", StringComparison.Ordinal))
            {
                var file = GameSettingsStore.Files.FirstOrDefault(name => "/game-settings/" + name.ToLowerInvariant() == path);
                if (file is null) { Reply(context, 404); return; }
                if (gameSettings is null) { Reply(context, 404); return; }
                context.Response.Headers["X-ClientLab-Settings"] = "enabled";
                if (context.Request.HttpMethod == "GET")
                {
                    try
                    {
                        var bytes = await gameSettings.ReadAsync(file, [settingsImportDirectory, gameDir]);
                        Reply(context, bytes is null ? 404 : 200, bytes); return;
                    }
                    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                    {
                        log?.Invoke("Game settings could not be restored. The existing settings file has been kept.");
                        throw;
                    }
                }
                if (context.Request.HttpMethod != "POST") { Reply(context, 405); return; }
                if (context.Request.ContentLength64 is < 0 or > GameSettingsStore.MaximumBytes) { Reply(context, 413); return; }
                using var settingsBytes = new MemoryStream();
                await context.Request.InputStream.CopyToAsync(settingsBytes);
                if (settingsBytes.Length > GameSettingsStore.MaximumBytes) { Reply(context, 413); return; }
                try { await gameSettings.WriteAsync(file, settingsBytes.ToArray()); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    log?.Invoke("Game settings could not be saved. Check the data folder's write permissions.");
                    throw;
                }
                Reply(context, 204); return;
            }
            if (path == "/game/log")
            {
                if (context.Request.HttpMethod != "POST" || context.Request.ContentLength64 is < 0 or > 16384) { Reply(context, 400); return; }
                using var reader = new StreamReader(context.Request.InputStream);
                var text = await reader.ReadToEndAsync();
                if (text.Length > 8192) { Reply(context, 413); return; }
                log?.Invoke("Game: " + text);
                Reply(context, 204); return;
            }
            if (path.StartsWith("/net/", StringComparison.Ordinal))
            {
                if (context.Request.HttpMethod != "POST") { Reply(context, 405); return; }
                await NetAsync(context, path[5..]);
                return;
            }
            if (path is "/auth/login" or "/auth/challenge" or "/auth/launch" or "/auth/launch-code")
            { await AuthAsync(context, path); return; }
            if (path == "/auth-code") { Reply(context, 404); return; }
            if (path == "/game-index.json") { GameIndex(context); return; }
            await StaticAsync(context, path);
        }
        catch (Exception error)
        {
            try { ReplyJson(context, 502, new JsonObject { ["error"] = error.Message }); } catch { /* client gone */ }
        }
        finally { try { context.Response.OutputStream.Close(); } catch { /* client gone */ } }
    }

    private static void CommonHeaders(HttpListenerContext context, string contentType, long length, bool immutable = false)
    {
        var response = context.Response;
        response.ContentType = contentType;
        response.ContentLength64 = length;
        // Cross-origin isolation keeps SharedArrayBuffer available to the worker runtime.
        response.AddHeader("Accept-Ranges", "bytes");
        response.AddHeader("Cross-Origin-Opener-Policy", "same-origin");
        response.AddHeader("Cross-Origin-Embedder-Policy", "credentialless");
        response.AddHeader("Cache-Control", immutable ? "max-age=31536000, immutable" : "no-cache");
    }

    private static void Reply(HttpListenerContext context, int status, byte[]? body = null, string contentType = "application/octet-stream")
    {
        body ??= [];
        context.Response.StatusCode = status;
        CommonHeaders(context, contentType, body.Length);
        if (context.Request.HttpMethod != "HEAD") context.Response.OutputStream.Write(body);
    }

    private static void ReplyJson(HttpListenerContext context, int status, JsonNode value) =>
        Reply(context, status, Encoding.UTF8.GetBytes(value.ToJsonString()), "application/json");

    private async Task AuthAsync(HttpListenerContext context, string path)
    {
        if (context.Request.HttpMethod != "POST" || auth is null)
        { ReplyJson(context, 404, new JsonObject { ["error"] = "Gameforge browser login is not enabled" }); return; }
        JsonObject payload;
        using (var reader = new StreamReader(context.Request.InputStream))
        {
            var text = await reader.ReadToEndAsync();
            if (text.Length > 16384) { ReplyJson(context, 413, new JsonObject { ["error"] = "request is too large" }); return; }
            payload = (JsonNode.Parse(string.IsNullOrEmpty(text) ? "{}" : text) as JsonObject) ?? new JsonObject();
        }
        try
        {
            var result = path switch
            {
                "/auth/login" => await auth.LoginAsync(payload, DefaultUserAgent),
                "/auth/challenge" => await auth.ChallengeAsync(payload),
                "/auth/launch" => await auth.LaunchAsync(payload, DefaultUserAgent),
                _ => await auth.LaunchAsync(payload, DefaultUserAgent, codeOnly: true),
            };
            if (result["code"] is { } code) result["code"] = GameforgeAuthentication.ClientTicket(code.GetValue<string>());
            ReplyJson(context, 200, result);
        }
        catch (GameforgeAuthentication.GameforgeException error)
        {
            var types = error.Body?["errorTypes"]?.AsArray().Select(value => value?.ToString()).ToArray() ?? [];
            if (types.Contains("CREDENTIALS_INVALID") || error.Status == 403)
                ReplyJson(context, 401, new JsonObject { ["error"] = "Gameforge rejected those credentials." });
            else
                ReplyJson(context, 502, new JsonObject { ["error"] = error.Message });
        }
        catch (InvalidOperationException error) { ReplyJson(context, 400, new JsonObject { ["error"] = error.Message }); }
    }

    private async Task NetAsync(HttpListenerContext context, string op)
    {
        var query = ParseQuery(context.Request.Url!.Query);
        try
        {
            if (op == "connect")
            {
                var host = query["host"]; var port = int.Parse(query["port"]);
                if (provider == "noscore")
                {
                    if (privateServerHost.Length > 0) host = privateServerHost;
                    if (privatePortRemap.TryGetValue(port, out var mapped)) port = mapped;
                }
                var id = await relay.ConnectAsync(host, port);
                Reply(context, 200, Encoding.ASCII.GetBytes(id.ToString()), "text/plain");
                return;
            }
            var cid = long.Parse(query["id"]);
            switch (op)
            {
                case "send":
                    using (var memory = new MemoryStream())
                    {
                        await context.Request.InputStream.CopyToAsync(memory);
                        Reply(context, relay.Send(cid, memory.ToArray()) ? 200 : 410);
                    }
                    return;
                case "recv":
                    var data = relay.Receive(cid, query.ContainsKey("wait"));
                    if (data is null) Reply(context, 410); else Reply(context, 200, data);
                    return;
                case "close":
                    relay.Close(cid);
                    Reply(context, 200);
                    return;
            }
        }
        catch (Exception error) when (error is SocketException or IOException or FormatException)
        {
            Reply(context, 502, Encoding.UTF8.GetBytes(error.Message), "text/plain");
            return;
        }
        Reply(context, 404);
    }

    private void GameIndex(HttpListenerContext context)
    {
        var entries = new JsonArray();
        var gameRoot = Path.GetFullPath(gameDir);
        foreach (var file in Directory.EnumerateFiles(gameRoot, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(gameRoot, file).Replace('\\', '/');
            if (relative.Equals("NosCore.exe", StringComparison.OrdinalIgnoreCase)) continue;
            var info = new FileInfo(file);
            entries.Add(new JsonObject { ["path"] = relative, ["size"] = info.Length, ["mtime"] = ToUnixSeconds(info) });
        }
        var client = new FileInfo(clientExe);
        entries.Add(new JsonObject { ["path"] = "NosCore.exe", ["size"] = client.Length, ["mtime"] = ToUnixSeconds(client) });
        ReplyJson(context, 200, entries);
    }

    private async Task StaticAsync(HttpListenerContext context, string path)
    {
        var full = Resolve(path);
        if (full is null || !File.Exists(full)) { Reply(context, 404); return; }
        var info = new FileInfo(full);
        var size = info.Length;
        var contentType = ContentType(full);
        long start = 0, end = size - 1;
        var status = 200;
        var range = context.Request.Headers["Range"];
        if (range is not null && range.StartsWith("bytes=", StringComparison.Ordinal))
        {
            var span = range[6..];
            var dash = span.IndexOf('-');
            var first = span[..dash];
            var last = span[(dash + 1)..];
            start = first.Length > 0 ? long.Parse(first) : Math.Max(0, size - long.Parse(last));
            end = first.Length > 0 && last.Length > 0 ? long.Parse(last) : size - 1;
            end = Math.Min(end, size - 1);
            status = 206;
        }
        // Game-file URLs carry the file's mtime (worker.js), so a changed file gets a new URL and the browser
        // may keep every range it has read; a reload then serves them from its cache instead of from here.
        var versioned = path.StartsWith("/game/", StringComparison.Ordinal) && context.Request.Url!.Query.Contains("v=");
        context.Response.StatusCode = status;
        CommonHeaders(context, contentType, end - start + 1, immutable: versioned);
        if (versioned)
        {
            context.Response.AddHeader("Last-Modified", info.LastWriteTimeUtc.ToString("R"));
            context.Response.AddHeader("ETag", $"\"{size:x}-{context.Request.Url!.Query}\"");
        }
        if (status == 206) context.Response.AddHeader("Content-Range", $"bytes {start}-{end}/{size}");
        if (context.Request.HttpMethod == "HEAD") return;
        await using var stream = File.OpenRead(full);
        stream.Seek(start, SeekOrigin.Begin);
        var left = end - start + 1;
        var buffer = new byte[1 << 20];
        while (left > 0)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(0, (int)Math.Min(left, buffer.Length)));
            if (read == 0) break;
            await context.Response.OutputStream.WriteAsync(buffer.AsMemory(0, read));
            left -= read;
        }
    }

    private string? Resolve(string path)
    {
        if (path is "/" or "/index.html") return Path.Combine(webDir, "index.html");
        if (path is "/worker.js" or "/gl.js" or "/control.js") return Path.Combine(webDir, path[1..]);
        if (path.StartsWith("/build/", StringComparison.Ordinal))
            return Contained(buildDir, path["/build/".Length..]);
        if (path.StartsWith("/game/", StringComparison.Ordinal))
        {
            var relative = path["/game/".Length..];
            if (relative.Equals("NosCore.exe", StringComparison.OrdinalIgnoreCase)) return clientExe;
            return Contained(gameDir, relative);
        }
        return null;
    }

    private static string? Contained(string directory, string relative)
    {
        var root = Path.GetFullPath(directory);
        var full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        var prefix = root.EndsWith(Path.DirectorySeparatorChar) ? root : root + Path.DirectorySeparatorChar;
        return full.StartsWith(prefix, StringComparison.Ordinal) ? full : null;
    }

    private static long ToUnixSeconds(FileInfo info) => new DateTimeOffset(info.LastWriteTimeUtc).ToUnixTimeSeconds();

    private static Dictionary<string, string> ParseQuery(string query)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var pair in query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var equals = pair.IndexOf('=');
            var key = equals < 0 ? pair : pair[..equals];
            result[Uri.UnescapeDataString(key)] = equals < 0 ? "" : Uri.UnescapeDataString(pair[(equals + 1)..]);
        }
        return result;
    }

    private static string ContentType(string path) => Path.GetExtension(path).ToLowerInvariant() switch
    {
        ".html" => "text/html",
        ".js" => "text/javascript",
        ".wasm" => "application/wasm",
        ".json" => "application/json",
        ".css" => "text/css",
        ".png" => "image/png",
        ".jpg" or ".jpeg" => "image/jpeg",
        ".wav" => "audio/wav",
        ".mp3" => "audio/mpeg",
        _ => "application/octet-stream",
    };

    public async ValueTask DisposeAsync()
    {
        loop?.Cancel();
        try { listener.Stop(); } catch { /* already stopped */ }
        listener.Close();
        await relay.DisposeAsync();
        auth?.Dispose();
        packets?.Dispose();
        loop?.Dispose();
    }

    /// <summary>
    /// TCP connections held for the worker, which cannot open sockets itself and never yields to an event loop:
    /// it drives them with synchronous HTTP requests (/net/connect, /net/send, /net/recv, /net/close).
    /// </summary>
    private sealed class Relay : IAsyncDisposable
    {
        private const double IdleSeconds = 20;
        private readonly ConcurrentDictionary<long, Connection> connections = new();
        private readonly CancellationTokenSource reaper = new();

        public Relay() => _ = ReapAsync(reaper.Token);

        public async Task<long> ConnectAsync(string host, int port)
        {
            var client = new TcpClient();
            await client.ConnectAsync(host, port).WaitAsync(TimeSpan.FromSeconds(5));
            var connection = new Connection(client);
            // js_net_connect returns an EM_JS int and the guest stores a signed int.
            // Larger IDs are truncated (and >53-bit IDs also lose JS precision), so
            // the subsequent send/receive requests cannot find their connection.
            long id;
            do { id = RandomNumberGenerator.GetInt32(1, int.MaxValue); }
            while (!connections.TryAdd(id, connection));
            _ = connection.ReadLoopAsync();
            return id;
        }

        public bool Send(long id, byte[] data)
        {
            if (!connections.TryGetValue(id, out var connection) || connection.Closed) return false;
            return connection.Send(data);
        }

        public byte[]? Receive(long id, bool wait)
        {
            if (!connections.TryGetValue(id, out var connection)) return null;
            return connection.Receive(wait);
        }

        public void Close(long id)
        {
            if (connections.TryRemove(id, out var connection)) connection.Dispose();
        }

        private async Task ReapAsync(CancellationToken token)
        {
            // A reloaded or closed page never sends /net/close; without this its sockets stay open and the
            // server keeps the account logged in.
            while (!token.IsCancellationRequested)
            {
                try { await Task.Delay(5000, token); } catch (OperationCanceledException) { return; }
                var now = Environment.TickCount64;
                foreach (var (id, connection) in connections)
                    if (now - connection.LastSeen > IdleSeconds * 1000) Close(id);
            }
        }

        public async ValueTask DisposeAsync()
        {
            await reaper.CancelAsync();
            foreach (var id in connections.Keys) Close(id);
            reaper.Dispose();
        }

        private sealed class Connection : IDisposable
        {
            private readonly TcpClient client;
            private readonly NetworkStream stream;
            private readonly object gate = new();
            private readonly List<byte> buffer = [];
            public bool Closed { get; private set; }
            public long LastSeen { get; private set; } = Environment.TickCount64;

            public Connection(TcpClient client)
            {
                this.client = client;
                stream = client.GetStream();
            }

            public async Task ReadLoopAsync()
            {
                var chunk = new byte[65536];
                try
                {
                    int read;
                    while ((read = await stream.ReadAsync(chunk)) > 0)
                        lock (gate)
                        {
                            buffer.AddRange(chunk.AsSpan(0, read).ToArray());
                            Monitor.PulseAll(gate);
                        }
                }
                catch (Exception) { /* connection dropped */ }
                lock (gate) { Closed = true; Monitor.PulseAll(gate); }
            }

            public bool Send(byte[] data)
            {
                LastSeen = Environment.TickCount64;
                try { stream.Write(data); return true; }
                catch (Exception) { return false; }
            }

            public byte[]? Receive(bool wait)
            {
                LastSeen = Environment.TickCount64;
                lock (gate)
                {
                    if (wait && buffer.Count == 0 && !Closed) Monitor.Wait(gate, 1000);
                    if (buffer.Count == 0) return Closed ? null : [];
                    var data = buffer.ToArray();
                    buffer.Clear();
                    return data;
                }
            }

            public void Dispose()
            {
                try { client.Close(); } catch { /* already closed */ }
            }
        }
    }
}
