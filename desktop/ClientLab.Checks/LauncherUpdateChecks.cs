using System.Net;
using System.Security.Cryptography;
using System.Text;
using NosCore.ClientLab.Launcher.Services;
using NosCore.ClientLab.Updates;

internal static class LauncherUpdateChecks
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    public static async Task RunAsync()
    {
        // Semantic version order, including prereleases and build metadata.
        Check(LauncherUpdate.CompareVersions("0.2.10", "0.2.9") > 0, "0.2.10 must be newer than 0.2.9.");
        Check(LauncherUpdate.CompareVersions("v1.0.0", "1.0.0") == 0, "A leading v changed the version.");
        Check(LauncherUpdate.CompareVersions("1.0.0-rc.1", "1.0.0") < 0, "A prerelease must sort before its release.");
        Check(LauncherUpdate.CompareVersions("1.0.0-rc.10", "1.0.0-rc.2") > 0, "Numeric prerelease parts must compare numerically.");
        Check(LauncherUpdate.CompareVersions("0.2.3+abc", "0.2.3") == 0, "Build metadata must not affect order.");
        Check(LauncherUpdate.CompareVersions("garbage", "0.0.1") < 0, "An unparseable version must never look newer.");

        // Manifest parsing rejects unsafe names and malformed checksums.
        var sha = new string('a', 64);
        var manifest = ReleaseManifest.Parse($$$$"""{"version":"0.2.7","assets":{"win-x64":{"name":"ClientLab-0.2.7-win-x64.exe","sha256":"{{{{sha}}}}"}}}""");
        Check(manifest.Version == "0.2.7" && manifest.Assets["win-x64"].Name == "ClientLab-0.2.7-win-x64.exe", "Release manifest parsing failed.");
        foreach (var bad in new[]
        {
            $$$$"""{"version":"0.2.7","assets":{"win-x64":{"name":"../evil.exe","sha256":"{{{{sha}}}}"}}}""",
            $$$$"""{"version":"0.2.7","assets":{"win-x64":{"name":"ClientLab.exe","sha256":"xyz"}}}""",
            """{"version":"latest","assets":{}}""",
        })
        {
            try { ReleaseManifest.Parse(bad); throw new Exception("Accepted an unsafe release manifest: " + bad); }
            catch (InvalidDataException) { }
        }

        var root = Path.Combine(Path.GetTempPath(), "ClientLab-update-checks-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var updates = Path.Combine(root, "updates");
            var install = Path.Combine(root, "install"); Directory.CreateDirectory(install);
            var target = Path.Combine(install, OperatingSystem.IsWindows() ? "ClientLab.exe" : "ClientLab");
            await File.WriteAllTextAsync(target, "old launcher");
            var payload = Encoding.UTF8.GetBytes("new launcher");
            var payloadSha = Convert.ToHexStringLower(SHA256.HashData(payload));
            var release = ReleaseManifest.Parse($$$$"""{"version":"0.2.8","assets":{"test-rid":{"name":"ClientLab-0.2.8-test-rid","sha256":"{{{{payloadSha}}}}"}}}""");

            // A download whose bytes do not match the manifest is rejected and deleted.
            Directory.CreateDirectory(updates);
            var tampered = Path.Combine(updates, "tampered.part");
            await File.WriteAllTextAsync(tampered, "tampered launcher");
            try { LauncherUpdate.Stage(updates, tampered, release, "test-rid"); throw new Exception("Staged a launcher with the wrong checksum."); }
            catch (InvalidDataException) { }
            Check(!File.Exists(tampered), "A rejected launcher download was left behind.");
            Check(LauncherUpdate.FindStaged(updates, "0.2.7", "test-rid") is null, "A rejected download was reported as staged.");

            // A verified download is staged, found only when newer and for the same runtime, then swapped in.
            var good = Path.Combine(updates, "good.part");
            await File.WriteAllBytesAsync(good, payload);
            LauncherUpdate.Stage(updates, good, release, "test-rid");
            Check(LauncherUpdate.FindStaged(updates, "0.2.7", "other-rid") is null, "An update for another runtime was offered.");
            LauncherUpdate.Stage(updates, await Rewrite(updates, payload), release, "test-rid");
            Check(LauncherUpdate.FindStaged(updates, "0.2.8", "test-rid") is null, "An update that is not newer was offered.");
            LauncherUpdate.Stage(updates, await Rewrite(updates, payload), release, "test-rid");
            // Corrupting the staged file after verification must stop the swap.
            await File.WriteAllTextAsync(Path.Combine(updates, "ClientLab-0.2.8-test-rid"), "corrupted later");
            Check(LauncherUpdate.FindStaged(updates, "0.2.7", "test-rid") is null, "A staged launcher changed after verification was still offered.");

            LauncherUpdate.Stage(updates, await Rewrite(updates, payload), release, "test-rid");
            var staged = LauncherUpdate.FindStaged(updates, "0.2.7", "test-rid") ?? throw new Exception("A verified update was not found.");
            Check(LauncherUpdate.Apply(updates, staged, target), "The staged launcher was not applied.");
            Check(await File.ReadAllTextAsync(target) == "new launcher", "The swap did not install the new launcher.");
            Check(!Directory.Exists(updates), "Applied updates were not cleared.");
            if (OperatingSystem.IsWindows()) Check(File.Exists(target + ".old"), "Windows swap must move the running executable aside.");
            LauncherUpdate.CleanupOld(target);
            Check(!File.Exists(target + ".old") && !File.Exists(target + ".new"), "Leftover executables were not cleaned up.");

            // End to end through the launcher service, against a fake release server.
            var server = new FakeReleases(release, payload);
            var log = new List<string>();
            using (var service = new LauncherUpdates(log.Add, bootstrapPath: target, runtime: "test-rid", currentVersion: "0.2.7",
                updatesDirectory: updates, handler: server, manifestUrl: "https://releases.invalid/release-manifest.json", assetBaseUrl: "https://releases.invalid/"))
            {
                var result = await service.CheckAsync(CancellationToken.None);
                Check(result?.Version == "0.2.8" && service.Staged == result, "The launcher service did not stage the newer release: " + string.Join(" | ", log));
                Check(server.AssetRequests == 1, "The launcher service did not download the asset exactly once.");
                await service.CheckAsync(CancellationToken.None);
                Check(server.AssetRequests == 1, "An already staged release was downloaded again.");
            }
            using (var dev = new LauncherUpdates(log.Add, bootstrapPath: "", runtime: "test-rid", currentVersion: "0.2.7", updatesDirectory: updates, handler: server))
                Check(!dev.Enabled && await dev.CheckAsync(CancellationToken.None) is null, "A launcher without a bootstrap must not update itself.");
            using (var offline = new LauncherUpdates(log.Add, bootstrapPath: target, runtime: "test-rid", currentVersion: "0.2.7",
                updatesDirectory: Path.Combine(root, "offline"), handler: new FakeReleases(null, payload), manifestUrl: "https://releases.invalid/release-manifest.json"))
                Check(await offline.CheckAsync(CancellationToken.None) is null && log.Any(line => line.StartsWith("Launcher update check skipped", StringComparison.Ordinal)),
                    "A failed update check must only be logged.");
        }
        finally { Directory.Delete(root, recursive: true); }
        Console.WriteLine("PASS: launcher self-update versions, manifest, checksum rejection, staging and swap.");
    }

    private static async Task<string> Rewrite(string updates, byte[] payload)
    {
        Directory.CreateDirectory(updates);
        var path = Path.Combine(updates, Guid.NewGuid().ToString("N") + ".part");
        await File.WriteAllBytesAsync(path, payload);
        return path;
    }

    private sealed class FakeReleases(ReleaseManifest? manifest, byte[] payload) : HttpMessageHandler
    {
        public int AssetRequests;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            if (manifest is null) return Task.FromResult(new HttpResponseMessage(HttpStatusCode.ServiceUnavailable));
            if (request.RequestUri!.AbsolutePath.EndsWith("release-manifest.json", StringComparison.Ordinal))
            {
                var asset = manifest.Assets.First();
                var json = $$$$"""{"version":"{{{{manifest.Version}}}}","assets":{"{{{{asset.Key}}}}":{"name":"{{{{asset.Value.Name}}}}","sha256":"{{{{asset.Value.Sha256}}}}"}}}""";
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(json) });
            }
            Interlocked.Increment(ref AssetRequests);
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(payload) });
        }
    }
}
