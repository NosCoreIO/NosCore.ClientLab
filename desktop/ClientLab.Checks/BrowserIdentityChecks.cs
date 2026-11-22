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
        using var identity = new GameforgeBrowserIdentity("aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb", officialClientIdentity: () => null);
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
        CheckOfficialClientImport();
    }

    private static void CheckOfficialClientImport()
    {
        // Mimic the official Gameforge client's Local Storage LevelDB framing: the key, a few non-value
        // bytes, then the value. x-vec carries the rotation timestamp the parser orders by.
        var latin1 = System.Text.Encoding.GetEncoding(28591);
        static byte[] Record(string device, string vector, long stamp, System.Text.Encoding enc)
            => enc.GetBytes($"_https://client-frontend.gameforge.com\0\x01x-game\0\x01{device}\x02\x01x-vec\0\x01{vector} {stamp}\x03\x08\xff");

        var newest = Record("it4va3bgy2ypr371ve1rqypeq1a", "AbC-newfile-vec*/=", 1_700_000_000_000, latin1);
        // An older file holds a later rotation of the same vector; recency is by timestamp, not file order.
        var older = Record("it4va3bgy2ypr371ve1rqypeq1a", "AbC-latest-vec*/=", 1_700_000_009_999, latin1);
        var parsed = GameforgeOfficialClientIdentity.Parse(new[] { newest, older })
            ?? throw new Exception("Official-client device identity was not recovered from LevelDB bytes.");
        if (parsed["x-game"]!.GetValue<string>() != "it4va3bgy2ypr371ve1rqypeq1a") throw new Exception("Wrong device identifier parsed from official client storage.");
        if (parsed["x-vec"]!.GetValue<string>() != "AbC-latest-vec*/= 1700000009999") throw new Exception("Parser did not choose the most recently rotated vector.");

        if (GameforgeOfficialClientIdentity.Parse(new[] { latin1.GetBytes("\x01x-game\0\x01onlythedevicetokenvalue00\x02") }) is not null)
            throw new Exception("A storage blob missing x-vec must not yield an identity.");
        if (GameforgeOfficialClientIdentity.Parse(Array.Empty<byte[]>()) is not null)
            throw new Exception("Empty storage must not yield an identity.");
        Console.WriteLine("PASS: official Gameforge client device identity is recovered from Local Storage bytes, the latest vector rotation wins, and incomplete storage yields nothing.");
        CheckOfficialClientDatabase();
    }

    // Builds a Local Storage LevelDB the way Chromium leaves it after compaction: an old device id in the
    // write-ahead log, the current one inside a Snappy-compressed table whose key text is split by back-references.
    private static void CheckOfficialClientDatabase()
    {
        const string origin = "_https://client-frontend.gameforge.com\0\u0001";
        static byte[] L(string text) => System.Text.Encoding.Latin1.GetBytes(text);
        var log = Log(10, (origin + "x-game", "\u0001olddeviceidentifier00000"), (origin + "x-vec", "\u0001old-vector 100"));
        var table = Table(new[] { (origin + "x-game", 50UL, "\u0001currentdeviceidentifier00"), (origin + "x-vec", 51UL, "\u0001current-vector 200") });
        if (System.Text.Encoding.Latin1.GetString(table).Contains("x-game")) throw new Exception("The table fixture must hide its keys behind Snappy back-references.");

        var parsed = GameforgeOfficialClientIdentity.ParseDatabase(new[] { ("000013.log", log), ("000011.ldb", table) })
            ?? throw new Exception("The device identity inside a compressed table was not read.");
        if (parsed["x-game"]!.GetValue<string>() != "currentdeviceidentifier00" || parsed["x-vec"]!.GetValue<string>() != "current-vector 200")
            throw new Exception("An older log record won over a newer compacted table record.");

        var newerLog = Log(90, (origin + "x-vec", "\u0001logged-vector 300"));
        parsed = GameforgeOfficialClientIdentity.ParseDatabase(new[] { ("000013.log", newerLog), ("000011.ldb", table) })!;
        if (parsed["x-game"]!.GetValue<string>() != "currentdeviceidentifier00" || parsed["x-vec"]!.GetValue<string>() != "logged-vector 300")
            throw new Exception("A newer write-ahead log record did not win over the table.");
        Console.WriteLine("PASS: official device identity is read as LevelDB records, through Snappy-compressed tables, with the highest sequence winning.");

        static byte[] Log(ulong sequence, params (string Key, string Value)[] puts)
        {
            var batch = new List<byte>();
            batch.AddRange(BitConverter.GetBytes(sequence)); batch.AddRange(BitConverter.GetBytes((uint)puts.Length));
            foreach (var (key, value) in puts) { batch.Add(1); Slice(batch, L(key)); Slice(batch, L(value)); }
            var record = new List<byte>(); record.AddRange(new byte[4]); record.AddRange(BitConverter.GetBytes((ushort)batch.Count)); record.Add(1); record.AddRange(batch);
            return record.ToArray();
        }

        static byte[] Table((string Key, ulong Sequence, string Value)[] entries)
        {
            var file = new List<byte>();
            var data = Block(entries.Select(entry => (L(entry.Key).Concat(BitConverter.GetBytes(entry.Sequence << 8 | 1)).ToArray(), L(entry.Value))));
            var dataHandle = AppendBlock(file, SnappyCompress(data), compressed: true);
            var index = Block(new[] { (L(origin + "x-vecÿ"), dataHandle) });
            var indexHandle = AppendBlock(file, index, compressed: false);
            var metaHandle = AppendBlock(file, Block(Array.Empty<(byte[], byte[])>()), compressed: false);
            var footer = new List<byte>(); footer.AddRange(metaHandle); footer.AddRange(indexHandle);
            footer.AddRange(new byte[40 - footer.Count]); footer.AddRange(BitConverter.GetBytes(0xdb4775248b80fb57UL));
            file.AddRange(footer);
            return file.ToArray();
        }

        static byte[] AppendBlock(List<byte> file, byte[] contents, bool compressed)
        {
            var handle = new List<byte>(); Varint(handle, (ulong)file.Count); Varint(handle, (ulong)contents.Length);
            file.AddRange(contents); file.Add(compressed ? (byte)1 : (byte)0); file.AddRange(new byte[4]);
            return handle.ToArray();
        }

        static byte[] Block(IEnumerable<(byte[] Key, byte[] Value)> entries)
        {
            var block = new List<byte>();
            foreach (var (key, value) in entries) { Varint(block, 0); Varint(block, (ulong)key.Length); Varint(block, (ulong)value.Length); block.AddRange(key); block.AddRange(value); }
            block.AddRange(BitConverter.GetBytes(0u)); block.AddRange(BitConverter.GetBytes(1u));
            return block.ToArray();
        }

        // Greedy Snappy encoder: 2-byte-offset copies for repeats of 4+ bytes, literals otherwise.
        static byte[] SnappyCompress(byte[] input)
        {
            var output = new List<byte>(); Varint(output, (ulong)input.Length);
            var literalStart = 0; var position = 0;
            void Flush(int end)
            {
                for (var start = literalStart; start < end; start += 60)
                {
                    var count = Math.Min(60, end - start);
                    output.Add((byte)((count - 1) << 2)); output.AddRange(input.AsSpan(start, count).ToArray());
                }
            }
            while (position < input.Length)
            {
                int bestLength = 0, bestOffset = 0;
                for (var candidate = Math.Max(0, position - 65535); candidate < position; candidate++)
                {
                    var length = 0;
                    while (length < 64 && position + length < input.Length && input[candidate + length] == input[position + length]) length++;
                    if (length > bestLength) { bestLength = length; bestOffset = position - candidate; }
                }
                if (bestLength < 4) { position++; continue; }
                Flush(position);
                output.Add((byte)(((bestLength - 1) << 2) | 2)); output.AddRange(BitConverter.GetBytes((ushort)bestOffset));
                position += bestLength; literalStart = position;
            }
            Flush(input.Length);
            return output.ToArray();
        }

        static void Slice(List<byte> target, byte[] value) { Varint(target, (ulong)value.Length); target.AddRange(value); }
        static void Varint(List<byte> target, ulong value) { while (value >= 0x80) { target.Add((byte)(value | 0x80)); value >>= 7; } target.Add((byte)value); }
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
            using (var collector = new GameforgeBrowserIdentity(installation, root, imported, () => null))
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
            using (var reopened = new GameforgeBrowserIdentity(installation, root, imported, () => null))
            using (var nextPage = new HttpClient())
            {
                var nextHtml = await nextPage.GetStringAsync(reopened.Url);
                if (!nextHtml.Contains("updated-vector 456") || nextHtml.Contains("must-not-persist")) throw new Exception("Changing loopback origins lost official browser state or persisted unrelated storage.");
            }
            Console.WriteLine("PASS: official device identifiers survive changing loopback ports, updated script state is retained, and unrelated browser storage is excluded.");

            // A saved state file bound to a collector-minted id must yield to the official client's registered id;
            // when the ids already match, whichever vector was rotated last wins.
            JsonObject OlderOfficial() => new() { ["x-game"] = "existing-device", ["x-vec"] = "older-official-vector 100" };
            using (var kept = new GameforgeBrowserIdentity(installation, root, imported, OlderOfficial))
            using (var keptPage = new HttpClient())
            {
                var html = await keptPage.GetStringAsync(kept.Url);
                if (!html.Contains("updated-vector 456")) throw new Exception("A saved vector rotated after the official client's should be kept.");
            }
            JsonObject NewerOfficial() => new() { ["x-game"] = "existing-device", ["x-vec"] = "newer-official-vector 999999" };
            using (var refreshed = new GameforgeBrowserIdentity(installation, root, imported, NewerOfficial))
            using (var refreshedPage = new HttpClient())
            {
                var html = await refreshedPage.GetStringAsync(refreshed.Url);
                if (!html.Contains("newer-official-vector 999999")) throw new Exception("A newer official vector rotation should replace an older saved one.");
            }
            JsonObject Official() => new() { ["x-game"] = "existing-device", ["x-vec"] = "registered-vector 999" };
            File.WriteAllText(Path.Combine(root, "gameforge-browser-state-" + installation + ".json"),
                new JsonObject { ["x-game"] = "minted-stale-id", ["x-vec"] = "minted-vector 111" }.ToJsonString());
            using (var healed = new GameforgeBrowserIdentity(installation, root, imported, Official))
            using (var healPage = new HttpClient())
            {
                var html = await healPage.GetStringAsync(healed.Url);
                if (!html.Contains("existing-device") || !html.Contains("registered-vector 999") || html.Contains("minted-stale-id"))
                    throw new Exception("A saved id differing from the official client was not replaced by the registered identity.");
            }
            Console.WriteLine("PASS: the official Gameforge client's registered device id overrides a stale minted state file but defers to a matching one.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    private static async Task CheckVerificationAsync()
    {
        using var collector = new GameforgeBrowserIdentity("aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb", officialClientIdentity: () => null);
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

