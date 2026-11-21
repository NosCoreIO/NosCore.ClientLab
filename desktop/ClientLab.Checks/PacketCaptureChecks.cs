using System.Text.Json;
using System.Text.Json.Nodes;
using System.Net.Http.Json;
using NosCore.ClientLab.Launcher.Services;
using NosCore.ClientLab.Recompiler;

internal static class PacketCaptureChecks
{
    public static async Task HostAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-packet-host-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var active = new JsonObject { ["client"] = root, ["runtime"] = root, ["executable"] = Path.Combine(root, "test.exe") };
            var settings = new JsonObject { ["provider"] = "noscore", ["packet_logger"] = "true" };
            await using var host = await GameHost.StartAsync(root, active, settings, gameSettings:new GameSettingsStore(root));
            using var http = new HttpClient { BaseAddress = new Uri(host.Url) };
            http.DefaultRequestHeaders.Add("Sec-Fetch-Site", "none");
            using var content = new StringContent(JsonSerializer.Serialize(new {kind=0, raw="walk 10 20", timestamp=DateTimeOffset.UtcNow}), System.Text.Encoding.UTF8, "application/json");
            var response = await http.PostAsync("packets", content);
            response.EnsureSuccessStatusCode();
            using var snapshot = JsonDocument.Parse(await http.GetStringAsync("packets?since=0"));
            if (snapshot.RootElement.GetProperty("entries")[0].GetProperty("Raw").GetString() != "walk 10 20") throw new Exception("Game host lost the captured packet.");
            if (!(await http.GetStringAsync("packet-log")).Contains("Packet logger")) throw new Exception("Packet window could not load.");
            http.DefaultRequestHeaders.Remove("Sec-Fetch-Site");
            http.DefaultRequestHeaders.Add("Sec-Fetch-Site", "cross-site");
            if ((await http.GetAsync("packets")).StatusCode != System.Net.HttpStatusCode.Forbidden) throw new Exception("Cross-site packet log access was permitted.");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
        Console.WriteLine("PASS: real game-host capture endpoint, packet viewer and cross-site access protections.");
    }
    public static void Run()
    {
        var image = new ExecutableImage(Convert.FromHexString("9053568BF28BD8EB0490"));
        if (!PacketHooks.Find(image).TryGetValue(0x1001, out var kind) || kind != 0)
            throw new Exception("DeveloperTools send signature did not resolve.");
        var ambiguous = new ExecutableImage(Convert.FromHexString("53568BF28BD8EB049053568BF28BD8EB04"));
        if (PacketHooks.Find(ambiguous).Count != 0) throw new Exception("An ambiguous packet hook was accepted.");
        var function = new FunctionDefinition(0x1001, "send", [new AddressRange(0x1001, 0x1009)]);
        if (!new CEmitter(image, function).Generate().Contains("port_capture_world_packet(c, 0, 0x00001001u);")) throw new Exception("The recompiler did not emit the packet hook.");
        if (Environment.GetEnvironmentVariable("CLIENTLAB_TEST_EXE") is { Length: > 0 } executable)
        {
            var live = new ExecutableImage(executable);
            var sites = PacketHooks.Find(live);
            foreach (var site in sites) Console.WriteLine($"Resolved packet hook {site.Value} at {site.Key:x8}");
            if (sites.Count != 3) throw new Exception("The selected client does not uniquely match all DeveloperTools capture signatures.");
        }
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-packet-check-" + Guid.NewGuid().ToString("N"));
        try
        {
            using var capture = new PacketCapture(root);
            capture.Add(0, "walk 10 20", DateTimeOffset.UtcNow);
            capture.Add(1, "say 1 2 hello", DateTimeOffset.UtcNow);
            capture.Add(2, "NsTeST", DateTimeOffset.UtcNow);
            using var snapshot = JsonDocument.Parse(JsonSerializer.Serialize(capture.Snapshot(1)));
            if (snapshot.RootElement.GetProperty("entries").GetArrayLength() != 2) throw new Exception("Packet cursor did not exclude previous entries.");
            using var reader = new StreamReader(new FileStream(capture.FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite));
            var lines = reader.ReadToEnd().Split('\n', StringSplitOptions.RemoveEmptyEntries);
            if (lines.Length != 4 || !Path.GetFileName(capture.FilePath).StartsWith("packets-")) throw new Exception("Timestamped packet log was not flushed for command-line readers.");
            using var session = JsonDocument.Parse(lines[0]);
            if (!session.RootElement.TryGetProperty("started", out _)) throw new Exception("Packet session start timestamp is missing.");
            using var login = JsonDocument.Parse(lines[3]);
            if (login.RootElement.GetProperty("Connection").GetString() != "Login" || login.RootElement.GetProperty("Direction").GetString() != "Receive") throw new Exception("Packet direction/connection mismatch.");
            try { capture.Add(3, "bad", DateTimeOffset.UtcNow); throw new Exception("Invalid hook kind accepted."); }
            catch (InvalidDataException) { }
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
        Console.WriteLine("PASS: DeveloperTools signature matching fails closed on ambiguity; packet cursors, directions, session timestamps and flushed JSONL logs.");
    }
}
