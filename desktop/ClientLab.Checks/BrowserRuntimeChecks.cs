using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using NosCore.ClientLab.Launcher.Services;

internal static class BrowserRuntimeChecks
{
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }

    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-browser-runtime-" + Guid.NewGuid().ToString("N"));
        try
        {
            // A Chromium archive as publish.ps1 writes it: files at the paths CefGlue expects next to the launcher.
            var archive = new MemoryStream();
            using (var zip = new ZipArchive(archive, ZipArchiveMode.Create, leaveOpen: true))
                foreach (var (name, text) in new[] { ("libcef.dll", "chromium"), ("CefGlueBrowserProcess/Xilium.CefGlue.BrowserProcess.exe", "helper"), ("locales/en-US.pak", "en") })
                {
                    await using var entry = zip.CreateEntry(name).Open();
                    await entry.WriteAsync(Encoding.UTF8.GetBytes(text));
                }
            var bytes = archive.ToArray();
            var sha = Convert.ToHexStringLower(SHA256.HashData(bytes));
            var server = new Server(bytes);
            var launcher = Path.Combine(root, "launcher"); var cache = Path.Combine(root, "cache");
            Directory.CreateDirectory(launcher);

            // A development build has no browser.json: Chromium is already next to it, nothing is downloaded.
            Check(BrowserRuntime.IsReady(launcher), "A build without browser.json must count as ready.");
            await BrowserRuntime.PrepareAsync(launcher, cache, server, null, CancellationToken.None);
            Check(server.Requests == 0, "A build without browser.json downloaded Chromium.");

            await File.WriteAllTextAsync(Path.Combine(launcher, BrowserRuntime.ManifestFile),
                $$"""{"name":"ClientLab-browser-0.2.9-win-x64.zip","url":"https://example.invalid/v0.2.9/ClientLab-browser-0.2.9-win-x64.zip","sha256":"{{sha}}","size":{{bytes.Length}}}""");
            Check(!BrowserRuntime.IsReady(launcher), "A launcher without its Chromium files counted as ready.");
            var progress = 0.0;
            await BrowserRuntime.PrepareAsync(launcher, cache, server, value => progress = value, CancellationToken.None);
            Check(server.Requests == 1 && progress == 1, "Chromium was not downloaded once with its progress.");
            Check(await File.ReadAllTextAsync(Path.Combine(launcher, "CefGlueBrowserProcess", "Xilium.CefGlue.BrowserProcess.exe")) == "helper" &&
                  await File.ReadAllTextAsync(Path.Combine(launcher, "libcef.dll")) == "chromium", "Chromium's files were not placed next to the launcher.");
            Check(BrowserRuntime.IsReady(launcher), "A prepared launcher did not count as ready.");

            // The next launcher release with the same Chromium links the cached copy instead of downloading again.
            var next = Path.Combine(root, "next");
            Directory.CreateDirectory(next);
            File.Copy(Path.Combine(launcher, BrowserRuntime.ManifestFile), Path.Combine(next, BrowserRuntime.ManifestFile));
            await BrowserRuntime.PrepareAsync(next, cache, server, null, CancellationToken.None);
            Check(server.Requests == 1 && File.Exists(Path.Combine(next, "locales", "en-US.pak")), "An updated launcher downloaded the same Chromium again.");

            // A download that does not match the published checksum is refused and leaves nothing behind.
            var tampered = Path.Combine(root, "tampered");
            Directory.CreateDirectory(tampered);
            await File.WriteAllTextAsync(Path.Combine(tampered, BrowserRuntime.ManifestFile),
                $$"""{"name":"x.zip","url":"https://example.invalid/x.zip","sha256":"{{new string('b', 64)}}","size":1}""");
            var refused = false;
            try { await BrowserRuntime.PrepareAsync(tampered, cache, server, null, CancellationToken.None); }
            catch (InvalidDataException) { refused = true; }
            Check(refused && !File.Exists(Path.Combine(tampered, "libcef.dll")) && !Directory.Exists(Path.Combine(cache, new string('b', 64))),
                "A Chromium download with the wrong checksum was accepted.");

            // browser.json can only point at an HTTPS address and a plain file name.
            foreach (var bad in new[]
            {
                $$"""{"name":"x.zip","url":"http://example.invalid/x.zip","sha256":"{{sha}}","size":1}""",
                $$"""{"name":"../x.zip","url":"https://example.invalid/x.zip","sha256":"{{sha}}","size":1}""",
                """{"name":"x.zip","url":"https://example.invalid/x.zip","sha256":"xyz","size":1}""",
            })
            {
                await File.WriteAllTextAsync(Path.Combine(tampered, BrowserRuntime.ManifestFile), bad);
                var rejected = false;
                try { BrowserRuntime.ReadManifest(tampered); } catch (InvalidDataException) { rejected = true; }
                Check(rejected, "An unsafe browser.json was accepted: " + bad);
            }
            Console.WriteLine("PASS: Chromium downloads once, is checked against its checksum, and is reused by later launcher releases.");
        }
        finally
        {
            try { Directory.Delete(root, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    private sealed class Server(byte[] archive) : HttpMessageHandler
    {
        public int Requests;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
        {
            Interlocked.Increment(ref Requests);
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(archive) });
        }
    }
}
