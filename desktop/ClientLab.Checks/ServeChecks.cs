using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class ServeChecks
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    private static void Rejects(string[] args, string why)
    {
        try { ServeOptions.Parse(args, _ => null); }
        catch (ArgumentException) { return; }
        throw new Exception("--serve accepted " + why + ": " + string.Join(' ', args));
    }

    public static async Task RunAsync()
    {
        var none = (Func<string, string?>)(_ => null);
        var defaults = ServeOptions.Parse(["--serve"], none);
        Check(defaults.Port == ServeOptions.DefaultPort && defaults.Provider == "gameforge" && defaults.Game is null, "--serve defaults changed.");
        var dev = ServeOptions.Parse(["--serve", "--game", "G", "--build", "B", "--port", "0", "--user", "admin", "--password", "pw",
            "--host", "127.0.0.1", "--remap", "4000=4100", "--remap", "1337=1338", "--packet-log"], none);
        Check(dev.Provider == "noscore" && dev.Port == 0 && dev.Remap == "4000=4100,1337=1338" && dev.RelayHost == "127.0.0.1" && dev.PacketLog,
            "--serve dev options were not parsed.");
        Check(ServeOptions.Parse(["--serve", "--auth-code", "abc"], none).Provider == "noscore", "A pre-issued code did not select the NosCore provider.");
        Check(ServeOptions.Parse(["--serve", "--auth-provider", "gameforge"], name => name == "TNT_INSTALLATION_ID" ? "id-from-env" : null).InstallationId == "id-from-env",
            "TNT_INSTALLATION_ID was ignored.");
        Rejects(["--serve", "--port", "70000"], "an out-of-range port");
        Rejects(["--serve", "--user", "admin"], "a user without a password");
        Rejects(["--serve", "--provider", "steam"], "an unknown provider");
        Rejects(["--serve", "--provider", "gameforge", "--user", "a", "--password", "b"], "NosCore credentials with the Gameforge provider");
        Rejects(["--serve", "--user", "a", "--password", "b", "--remap", "4000"], "a malformed remap");
        Rejects(["--serve", "--remap", "4000=4001"], "a remap without the NosCore provider");
        Rejects(["--serve", "--build", "B"], "a dev build without its game folder");
        Rejects(["--serve", "--gmae", "G"], "a misspelt option");
        Rejects(["--serve", "--game"], "an option without its value");

        var root = Path.Combine(Path.GetTempPath(), "ClientLab-serve-check-" + Guid.NewGuid().ToString("N"));
        try
        {
            var game = Directory.CreateDirectory(Path.Combine(root, "game")).FullName;
            var build = Directory.CreateDirectory(Path.Combine(root, "build")).FullName;
            var web = Directory.CreateDirectory(Path.Combine(root, "source", "recomp", "web")).FullName;
            await File.WriteAllTextAsync(Path.Combine(build, "nostale.js"), "// fixture");
            await File.WriteAllTextAsync(Path.Combine(game, "NostaleClientX.exe"), "MZ fixture");
            await File.WriteAllTextAsync(Path.Combine(web, "index.html"), "<!doctype html><title>fixture</title>");

            // A dev build falls back to the game folder's official executable; without --game the prepared runtime is used.
            var resolved = ServeOptions.Parse(["--serve", "--game", game, "--build", build], none).ResolveActive(root);
            Check(resolved["executable"]!.GetValue<string>() == Path.Combine(game, "NostaleClientX.exe"), "--serve did not find the game folder's executable.");
            try { defaults.ResolveActive(root); throw new Exception("--serve started without a prepared game."); }
            catch (InvalidOperationException) { }
            await File.WriteAllTextAsync(Path.Combine(root, "active.json"), new JsonObject {
                ["client"] = game, ["runtime"] = build, ["executable"] = Path.Combine(game, "NostaleClientX.exe") }.ToJsonString());
            Check(defaults.ResolveActive(root)["runtime"]!.GetValue<string>() == build, "--serve ignored the prepared runtime.");

            var options = ServeOptions.Parse(["--serve", "--game", game, "--build", build, "--port", "0", "--auth-code", "fixture-code"], none);
            await using (var host = await GameHost.StartAsync(Path.Combine(root, "source"), resolved, options.Settings(root), options: options.HostOptions()))
            {
                using var http = new HttpClient { BaseAddress = new Uri(host.Url), Timeout = TimeSpan.FromSeconds(10) };
                // Playwright's page.goto is a top-level navigation: Sec-Fetch-Site "none".
                using var navigation = new HttpRequestMessage(HttpMethod.Get, "/");
                navigation.Headers.Add("Sec-Fetch-Site", "none");
                navigation.Headers.Add("Sec-Fetch-Mode", "navigate");
                navigation.Headers.Add("Sec-Fetch-Dest", "document");
                using (var page = await http.SendAsync(navigation))
                    Check(page.IsSuccessStatusCode && (await page.Content.ReadAsStringAsync()).Contains("fixture"), "A browser navigation to the --serve URL was refused.");
                using var codeRequest = new HttpRequestMessage(HttpMethod.Get, "/auth-code");
                codeRequest.Headers.Add("Sec-Fetch-Site", "same-origin");
                using (var code = await http.SendAsync(codeRequest))
                    Check(code.IsSuccessStatusCode && await code.Content.ReadAsStringAsync() == "fixture-code", "/auth-code did not return the pre-issued code.");
                using var crossSite = new HttpRequestMessage(HttpMethod.Get, "/auth-code");
                crossSite.Headers.Add("Sec-Fetch-Site", "cross-site");
                using (var refused = await http.SendAsync(crossSite))
                    Check((int)refused.StatusCode == 403, "Another site could read the auth code.");
            }
            await using (var desktop = await GameHost.StartAsync(Path.Combine(root, "source"), resolved, new JsonObject { ["provider"] = "noscore" }))
            {
                using var http = new HttpClient { BaseAddress = new Uri(desktop.Url), Timeout = TimeSpan.FromSeconds(10) };
                using var request = new HttpRequestMessage(HttpMethod.Get, "/auth-code");
                request.Headers.Add("Sec-Fetch-Site", "same-origin");
                using var response = await http.SendAsync(request);
                Check((int)response.StatusCode == 404, "The desktop host answered /auth-code without a code source.");
            }
        }
        finally { Directory.Delete(root, recursive: true); }
        Console.WriteLine("PASS: --serve options, build resolution, Playwright navigation and the /auth-code endpoint.");
    }
}
