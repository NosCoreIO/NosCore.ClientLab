using System.Net;
using System.Net.Http.Json;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class BrowserIdentityChecks
{
    private static async Task<HttpResponseMessage> PostAsync(HttpClient browser, string path, object value)
    {
        using var content = JsonContent.Create(value);
        await content.LoadIntoBufferAsync(); // Fetch sends the known length of its string body.
        return await browser.PostAsync(path, content);
    }

    public static async Task RunAsync()
    {
        using var identity = new GameforgeBrowserIdentity("aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb");
        using var browser = new HttpClient { BaseAddress = new Uri(identity.Url), Timeout = TimeSpan.FromSeconds(5) };
        using (var invalid = await PostAsync(browser, "ready", new { userAgent = "untrusted" }))
            if (invalid.StatusCode != HttpStatusCode.Forbidden) throw new Exception("Foreign bridge request was accepted.");
        browser.DefaultRequestHeaders.Referrer = new Uri(identity.Url);
        using (var response = await PostAsync(browser, "ready", new { userAgent = "actual-browser" })) response.EnsureSuccessStatusCode();
        await identity.WaitUntilReadyAsync();
        if (identity.UserAgent != "actual-browser") throw new Exception("Browser user agent was not retained.");
        var metadata = new JsonObject { ["installation"] = identity.InstallationId, ["session"] = "test-session" };
        var collection = identity.CreateAsync(browser, metadata);
        var job = (await browser.GetFromJsonAsync<JsonObject>("next"))!;
        if (job["request"]?["session"]?.ToString() != "test-session") throw new Exception("Collector request metadata was lost.");
        using (var response = await PostAsync(browser, "result", new { id = job["id"]!.ToString(), blackbox = "tra:publisher-output" })) response.EnsureSuccessStatusCode();
        if (await collection != "tra:publisher-output") throw new Exception("Collector output was modified.");
        collection = identity.CreateAsync(browser);
        job = (await browser.GetFromJsonAsync<JsonObject>("next"))!;
        using (var response = await PostAsync(browser, "result", new { id = job["id"]!.ToString(), error = "failed" })) response.EnsureSuccessStatusCode();
        try { await collection; throw new Exception("A collector failure was treated as an identity."); }
        catch (InvalidOperationException error) when (error.Message.Contains("identity script")) { }
        var pending = identity.CreateAsync(browser);
        identity.Dispose();
        try { await pending; throw new Exception("Disposal left identity collection pending."); }
        catch (OperationCanceledException) { }
        Console.WriteLine("PASS: browser identity bridge rejects foreign origins, retains real user-agent and request metadata, propagates collector failures, and cancels on shutdown.");
        await CheckPersistenceAsync();
        await CheckVerificationAsync();
    }

    private static async Task CheckPersistenceAsync()
    {
        const string installation = "aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb";
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-browser-state-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var fields = new JsonArray(Enumerable.Repeat<JsonNode?>(null, 30).ToArray());
            fields[22] = "existing-device";
            fields[25] = Convert.ToBase64String(System.Text.Encoding.ASCII.GetBytes("existing-vector 123"));
            var imported = Path.Combine(root, "identity.json");
            File.WriteAllText(imported, new JsonObject { ["installationId"] = installation, ["fingerprint"] = fields }.ToJsonString());
            using (var collector = new GameforgeBrowserIdentity(installation, root, imported))
            using (var page = new HttpClient { BaseAddress = new Uri(collector.Url), Timeout = TimeSpan.FromSeconds(5) })
            {
                var html = await page.GetStringAsync("");
                if (!html.Contains("existing-device") || !html.Contains("existing-vector 123")) throw new Exception("Existing device identifiers were not restored for the official script.");
                page.DefaultRequestHeaders.Referrer = new Uri(collector.Url);
                using (var response = await PostAsync(page, "ready", new { userAgent = "test-browser" })) response.EnsureSuccessStatusCode();
                var collection = collector.CreateAsync(page);
                var job = (await page.GetFromJsonAsync<JsonObject>("next"))!;
                using (var response = await PostAsync(page, "result", new { id = job["id"]!.ToString(), blackbox = "tra:test", storage = new Dictionary<string, string> { ["x-game"] = "existing-device", ["x-vec"] = "updated-vector 456", ["unrelated"] = "must-not-persist" } })) response.EnsureSuccessStatusCode();
                await collection;
            }
            using var reopened = new GameforgeBrowserIdentity(installation, root, imported);
            using var nextPage = new HttpClient();
            var nextHtml = await nextPage.GetStringAsync(reopened.Url);
            if (!nextHtml.Contains("updated-vector 456") || nextHtml.Contains("must-not-persist")) throw new Exception("Changing loopback origins lost official browser state or persisted unrelated storage.");
            Console.WriteLine("PASS: official device identifiers survive changing loopback ports, updated script state is retained, and unrelated browser storage is excluded.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    private static async Task CheckVerificationAsync()
    {
        using var collector = new GameforgeBrowserIdentity("aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb");
        using var page = new HttpClient { BaseAddress = new Uri(collector.Url), Timeout = TimeSpan.FromSeconds(5) };
        page.DefaultRequestHeaders.Referrer = new Uri(collector.Url);
        using (var ready = await PostAsync(page, "ready", new { userAgent = "test-browser" })) ready.EnsureSuccessStatusCode();
        var script = await page.GetStringAsync("verification.js");
        if (!script.Contains("https://pow-captcha.gameforge.com")) throw new Exception("Official widget adapter was not included in the executable.");
        foreach (var status in new[] { "solved", "failed", "cancelled" })
        {
            const string challenge = "12345678-1111-2222-3333-444444444444";
            var verification = collector.CompleteProofOfWorkAsync(challenge);
            var job = (await page.GetFromJsonAsync<JsonObject>("next"))!;
            if (job["kind"]?.ToString() != "pow" || job["challengeId"]?.ToString() != challenge) throw new Exception("Verification job changed the official challenge.");
            using (var unrelated = await PostAsync(page, "verification-result", new { id = "unknown", status = "solved" }))
                if (unrelated.StatusCode != HttpStatusCode.NotFound) throw new Exception("Unrelated verification result was accepted.");
            if (verification.IsCompleted) throw new Exception("An unrelated result completed the proof.");
            using (var response = await PostAsync(page, "verification-result", new { id = job["id"]!.ToString(), status })) response.EnsureSuccessStatusCode();
            if (status == "solved") await verification;
            else
            {
                try { await verification; throw new Exception("Failed/cancelled work was accepted."); }
                catch (InvalidOperationException error) when (error.Message.Contains(status == "cancelled" ? "cancelled" : "did not complete")) { }
            }
        }
        var pending = collector.CompleteProofOfWorkAsync("12345678-1111-2222-3333-444444444444");
        collector.Dispose();
        try { await pending; throw new Exception("Shutdown left proof pending."); } catch (OperationCanceledException) { }
        Console.WriteLine("PASS: PoW bridge correlates challenges/results, rejects unrelated success, preserves failure/cancellation, includes the official widget adapter, and cancels on shutdown.");
    }
}

