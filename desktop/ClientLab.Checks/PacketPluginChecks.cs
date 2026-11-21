using System.Collections.Concurrent;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;
using NosCore.ClientLab.Plugins;
using NosCore.ClientLab.Recompiler;

/// <summary>Loaded from this assembly as a plugin: answers the first sent packet by using every game request once.</summary>
public sealed class CheckPacketPlugin : IPacketPlugin
{
    private IGameSession? game;
    private Task? work;

    public Task StartAsync(IGameSession game, CancellationToken cancellationToken) { this.game = game; return Task.CompletedTask; }

    public void OnPacket(Packet packet)
    {
        if (packet.Direction != PacketDirection.Send || work is not null) return;
        work = Task.Run(async () =>
        {
            try
            {
                var sent = await game!.SendPacketAsync("say plugin");
                var bytes = await game.ReadAsync(0x20000000, 4);
                await game.WriteAsync(0x20000000, new byte[] { 9, 8 });
                var eax = await game.CallAsync(0x401000, 1, 2, 3, [4, 5]);
                game.Log($"done {packet.Raw} sent={sent} read={Convert.ToHexString(bytes)} eax={eax:x}");
            }
            catch (Exception error) { game!.Log("failed " + error.Message); }
        });
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}

internal static class PacketPluginChecks
{
    public static async Task RunAsync()
    {
        var code = new ExecutableImage(Convert.FromHexString("9053568BF28BD8EB04905356"), 0x401000);
        if (GameBridge.FindUnique(code, "53 56 8B ?? 8B D8") != 0x401001) throw new Exception("A wildcard code pattern did not resolve.");
        if (GameBridge.FindUnique(code, "53 56") is not null) throw new Exception("An ambiguous code pattern was accepted.");
        if (GameBridge.FindUnique(code, "?? 53 56 8B F2") != 0x401000) throw new Exception("A pattern starting with a wildcard did not resolve.");
        try { GameBridge.FindUnique(code, "?? ??"); throw new Exception("A pattern without fixed bytes was accepted."); }
        catch (ArgumentException) { }

        var root = Path.Combine(Path.GetTempPath(), "ClientLab-packet-plugin-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var active = new JsonObject { ["client"] = root, ["runtime"] = root, ["executable"] = Path.Combine(root, "test.exe") };
            var missing = new JsonObject { ["provider"] = "noscore", ["packet_logger"] = "true", ["packet_plugin"] = Path.Combine(root, "missing.dll") };
            try { await using var _ = await GameHost.StartAsync(root, active, missing, gameSettings: new GameSettingsStore(root)); throw new Exception("A missing plugin was accepted."); }
            catch (FileNotFoundException) { }

            var logs = new ConcurrentQueue<string>();
            var settings = new JsonObject { ["provider"] = "noscore", ["packet_logger"] = "true", ["packet_plugin"] = typeof(CheckPacketPlugin).Assembly.Location };
            await using var host = await GameHost.StartAsync(root, active, settings, logs.Enqueue, new GameSettingsStore(root));
            if (!logs.Any(line => line.Contains(nameof(CheckPacketPlugin)))) throw new Exception("The plugin was not loaded: " + string.Join(" | ", logs));
            using var http = new HttpClient { BaseAddress = new Uri(host.Url), Timeout = TimeSpan.FromSeconds(30) };
            http.DefaultRequestHeaders.Add("Sec-Fetch-Site", "same-origin");
            (await http.PostAsync("packets", Json(new { kind = 0, raw = "walk 10 20", timestamp = DateTimeOffset.UtcNow }))).EnsureSuccessStatusCode();

            // Play the game page: answer each request the plugin makes, as worker.js would.
            var packet = await NextAsync(http);
            if (packet["op"]!.GetValue<string>() != "packet" || packet["kind"]!.GetValue<int>() != 0 || packet["raw"]!.GetValue<string>() != "say plugin")
                throw new Exception("The plugin's packet request was not relayed: " + packet.ToJsonString());
            await AnswerAsync(http, packet, new JsonObject { ["ok"] = true });
            var read = await NextAsync(http);
            if (read["op"]!.GetValue<string>() != "read" || read["address"]!.GetValue<uint>() != 0x20000000 || read["length"]!.GetValue<int>() != 4)
                throw new Exception("The plugin's read request was not relayed: " + read.ToJsonString());
            await AnswerAsync(http, read, new JsonObject { ["hex"] = "01020304" });
            var write = await NextAsync(http);
            if (write["op"]!.GetValue<string>() != "write" || write["hex"]!.GetValue<string>() != "0908")
                throw new Exception("The plugin's write request was not relayed: " + write.ToJsonString());
            await AnswerAsync(http, write, new JsonObject());
            var call = await NextAsync(http);
            if (call["op"]!.GetValue<string>() != "call" || call["address"]!.GetValue<uint>() != 0x401000 || call["ecx"]!.GetValue<uint>() != 3 ||
                call["stack"]!.AsArray().Count != 2)
                throw new Exception("The plugin's call request was not relayed: " + call.ToJsonString());
            await AnswerAsync(http, call, new JsonObject { ["eax"] = 0xbeefu });

            for (var wait = 0; wait < 100 && !logs.Any(line => line.Contains("done ") || line.Contains("failed ")); wait++) await Task.Delay(50);
            var result = logs.FirstOrDefault(line => line.Contains("done ") || line.Contains("failed "));
            if (result is null || !result.Contains("done walk 10 20 sent=True read=01020304 eax=beef"))
                throw new Exception("The plugin did not get the game's answers: " + (result ?? string.Join(" | ", logs)));

            http.DefaultRequestHeaders.Remove("Sec-Fetch-Site");
            http.DefaultRequestHeaders.Add("Sec-Fetch-Site", "cross-site");
            if ((await http.GetAsync("plugin/next")).StatusCode != HttpStatusCode.Forbidden) throw new Exception("A cross-site page could fetch plugin requests.");
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
        Console.WriteLine("PASS: packet plugins load from their assembly, receive captured packets and reach the game through the page relay; code patterns fail closed on ambiguity.");
    }

    private static StringContent Json(object value) => new(JsonSerializer.Serialize(value), Encoding.UTF8, "application/json");

    private static async Task<JsonObject> NextAsync(HttpClient http)
    {
        var response = await http.GetAsync("plugin/next");
        if (response.StatusCode != HttpStatusCode.OK) throw new Exception("No plugin request reached the page: " + response.StatusCode);
        return JsonNode.Parse(await response.Content.ReadAsStringAsync())!.AsObject();
    }

    private static async Task AnswerAsync(HttpClient http, JsonObject request, JsonObject result) =>
        (await http.PostAsync("plugin/answer", Json(new { id = request["id"]!.GetValue<long>(), result }))).EnsureSuccessStatusCode();
}
