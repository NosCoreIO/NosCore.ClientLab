using System.Net;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class GameFilesChecks
{
    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-gamefiles-check-" + Guid.NewGuid().ToString("N"));
        try
        {
            byte[] same = [1, 2, 3], before = [4, 5], after = [6, 7, 8], big = RandomNumberGenerator.GetBytes(3 * 1024 * 1024);
            static JsonObject Entry(string file, byte[] bytes) =>
                new() { ["file"] = file, ["sha1"] = Convert.ToHexStringLower(SHA1.HashData(bytes)), ["size"] = (long)bytes.Length, ["path"] = "/" + file };
            var served = new Dictionary<string, byte[]> { ["/data/same.NOS"] = same, ["/data/changed.NOS"] = before, ["/big.bin"] = big };
            var requests = 0; var concurrent = 0; var peak = 0;
            async Task<HttpResponseMessage> Fetch(JsonNode entry, CancellationToken token)
            {
                Interlocked.Increment(ref requests);
                var now = Interlocked.Increment(ref concurrent); lock (served) peak = Math.Max(peak, now);
                await Task.Delay(50, token);
                Interlocked.Decrement(ref concurrent);
                return served.TryGetValue(entry["path"]!.GetValue<string>(), out var bytes)
                    ? new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) }
                    : new HttpResponseMessage(HttpStatusCode.NotFound);
            }
            var first = Path.Combine(root, "v1", "client");
            var v1 = new JsonArray(Entry("data/same.NOS", same), Entry("data/changed.NOS", before), Entry("big.bin", big));
            var sync = new GameFiles(Fetch, _ => { }, (_, _) => { });
            await sync.SyncAsync(first, "v1", v1, [], "", CancellationToken.None);
            Check(sync.Downloaded == 3 && requests == 3 && VerifiedClientFiles.IsCurrent(first, "v1", v1), "A fresh install did not download and verify every file.");
            Check(peak > 1, "Downloads did not run in parallel.");

            // The next patch changes one file: only that one is fetched, the rest are linked and not re-hashed.
            served["/data/changed.NOS"] = after;
            var v2 = new JsonArray(Entry("data/same.NOS", same), Entry("data/changed.NOS", after), Entry("big.bin", big));
            Check(GameFiles.DownloadBytes(v2, GameFiles.Donors([first])) == after.Length, "The update plan did not count only the changed file.");
            var second = Path.Combine(root, "v2", "client"); requests = 0;
            var patch = new GameFiles(Fetch, _ => { }, (_, _) => { });
            await patch.SyncAsync(second, "v2", v2, [first], "", CancellationToken.None);
            Check(patch.Downloaded == 1 && patch.Linked == 2 && requests == 1, "A one-file patch fetched more than the changed file.");
            Check(File.ReadAllBytes(Path.Combine(second, "data/changed.NOS")).SequenceEqual(after) &&
                  File.ReadAllBytes(Path.Combine(first, "data/changed.NOS")).SequenceEqual(before), "The new version's file leaked into the previous version.");
            Check(File.ReadAllBytes(Path.Combine(second, "big.bin")).SequenceEqual(big), "A linked file differs from its source.");

            // A donor modified after verification is no longer trusted.
            File.WriteAllBytes(Path.Combine(first, "data/same.NOS"), [9, 9, 9]); File.SetLastWriteTimeUtc(Path.Combine(first, "data/same.NOS"), DateTime.UtcNow.AddMinutes(1));
            Check(!GameFiles.Donors([first]).Keys.Any(key => key.StartsWith("data/same.NOS")), "A modified donor file was still offered for linking.");

            // A corrupted download is rejected and leaves nothing behind.
            served["/big.bin"] = RandomNumberGenerator.GetBytes(big.Length);
            var third = Path.Combine(root, "v3", "client");
            try
            {
                await new GameFiles(Fetch, _ => { }, (_, _) => { }).SyncAsync(third, "v3", new JsonArray(Entry("big.bin", big)), [], "", CancellationToken.None);
                throw new Exception("A corrupted download was installed.");
            }
            catch (InvalidDataException) { }
            Check(!File.Exists(Path.Combine(third, "big.bin")) && !File.Exists(Path.Combine(third, "big.bin.part")), "A rejected download left files behind.");
            Console.WriteLine("PASS: patches link unchanged game files, download only changed ones in parallel, and reject corrupted or tampered files.");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, recursive: true); }
    }

    private static void Check(bool condition, string error) { if (!condition) throw new Exception(error); }
}
