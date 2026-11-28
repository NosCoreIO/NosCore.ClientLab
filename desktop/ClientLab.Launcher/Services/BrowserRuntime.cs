using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Chromium, which shows the game, the Gameforge sign-in and the packet logger, is not part of the launcher download:
/// publish.ps1 ships it as its own release asset and leaves a <c>browser.json</c> (address and SHA-256) next to the
/// launcher. On first start the launcher downloads it once into its data folder, then hard-links its files into its own
/// folder, where CefGlue looks for them. A launcher update that keeps the same Chromium reuses the download.
/// A build without <c>browser.json</c> (a development build) already has Chromium next to it.
/// </summary>
public static class BrowserRuntime
{
    public const string ManifestFile = "browser.json";
    private const string ReadyMarker = ".browser-ready";
    private static readonly object gate = new();
    private static Task? preparing;
    private static Action<double>? progress;
    private static readonly TimeSpan StallTimeout = TimeSpan.FromMinutes(1);

    public sealed record Manifest(string Name, Uri Url, string Sha256, long Size);

    /// <summary>True when Chromium is already in place, so opening a browser needs no download.</summary>
    public static bool IsReady(string baseDirectory)
    {
        var manifest = ReadManifest(baseDirectory);
        return manifest is null || ReadMarker(baseDirectory) == manifest.Sha256;
    }

    /// <summary>Starts (or joins) the download for this launcher. <paramref name="onProgress"/> gets 0..1 while the
    /// archive downloads; a failed attempt is forgotten so the next call tries again.</summary>
    public static Task EnsureAsync(string dataDirectory, Action<double>? onProgress = null)
    {
        lock (gate)
        {
            if (onProgress is not null) progress += onProgress;
            if (preparing is null || preparing.IsFaulted || preparing.IsCanceled)
            {
                preparing = Task.Run(() => PrepareAsync(AppContext.BaseDirectory, Path.Combine(dataDirectory, "chromium"), null,
                    value => progress?.Invoke(value), CancellationToken.None));
            }
            return Unsubscribing(preparing, onProgress);
        }
    }

    private static async Task Unsubscribing(Task task, Action<double>? onProgress)
    {
        // No UI context: the diagnostic --game-url start waits for this synchronously on the UI thread.
        try { await task.ConfigureAwait(false); }
        finally { if (onProgress is not null) lock (gate) progress -= onProgress; }
    }

    public static Manifest? ReadManifest(string baseDirectory)
    {
        var path = Path.Combine(baseDirectory, ManifestFile);
        if (!File.Exists(path)) return null;
        var node = JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? throw new InvalidDataException("browser.json is not an object.");
        var name = node["name"]?.GetValue<string>() ?? "";
        var sha = node["sha256"]?.GetValue<string>()?.ToLowerInvariant() ?? "";
        var url = node["url"]?.GetValue<string>() ?? "";
        var size = node["size"]?.GetValue<long>() ?? 0;
        if (name.Length == 0 || name != Path.GetFileName(name) || name.Contains("..")) throw new InvalidDataException("browser.json has an unsafe name.");
        if (sha.Length != 64 || !sha.All(Uri.IsHexDigit)) throw new InvalidDataException("browser.json has an invalid checksum.");
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.Scheme != Uri.UriSchemeHttps) throw new InvalidDataException("Chromium must be downloaded over HTTPS.");
        return new Manifest(name, uri, sha, size);
    }

    /// <summary>Makes Chromium available in <paramref name="baseDirectory"/>: downloads and unpacks the archive into
    /// <paramref name="cacheDirectory"/> unless it is already there, then links every file into place.</summary>
    public static async Task PrepareAsync(string baseDirectory, string cacheDirectory, HttpMessageHandler? handler,
        Action<double>? onProgress, CancellationToken token)
    {
        if (ReadManifest(baseDirectory) is not { } manifest || ReadMarker(baseDirectory) == manifest.Sha256) return;
        Directory.CreateDirectory(cacheDirectory);
        using (await AcquireLockAsync(Path.Combine(cacheDirectory, manifest.Sha256 + ".lock"), token))
        {
            var unpacked = Path.Combine(cacheDirectory, manifest.Sha256);
            if (!File.Exists(Path.Combine(unpacked, ".complete")))
            {
                var archive = Path.Combine(cacheDirectory, manifest.Sha256 + ".zip");
                await DownloadAsync(manifest, archive, handler, onProgress, token);
                try { Unpack(archive, cacheDirectory, unpacked); }
                finally { File.Delete(archive); }
            }
            LinkInto(unpacked, baseDirectory);
            await File.WriteAllTextAsync(Path.Combine(baseDirectory, ReadyMarker), manifest.Sha256, token);
            Prune(cacheDirectory, manifest.Sha256);
        }
    }

    private static string? ReadMarker(string baseDirectory)
    {
        try { return File.ReadAllText(Path.Combine(baseDirectory, ReadyMarker)).Trim(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    private static async Task DownloadAsync(Manifest manifest, string path, HttpMessageHandler? handler, Action<double>? onProgress, CancellationToken token)
    {
        using var http = handler is null ? new HttpClient() : new HttpClient(handler, disposeHandler: false);
        http.Timeout = TimeSpan.FromMinutes(30);
        http.DefaultRequestHeaders.UserAgent.ParseAdd("NosCore.ClientLab/0.2");
        // HttpClient.Timeout stops at the headers: a server that stalls in the body is cut off once it sends nothing for a minute.
        using var stalled = CancellationTokenSource.CreateLinkedTokenSource(token);
        stalled.CancelAfter(StallTimeout);
        using var response = await http.GetAsync(manifest.Url, HttpCompletionOption.ResponseHeadersRead, stalled.Token);
        response.EnsureSuccessStatusCode();
        var size = response.Content.Headers.ContentLength ?? manifest.Size;
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        try
        {
            await using var output = File.Create(path + ".part");
            await using var input = await response.Content.ReadAsStreamAsync(stalled.Token);
            var buffer = new byte[1024 * 1024]; long done = 0; int length;
            while ((length = await input.ReadAsync(buffer, stalled.Token)) > 0)
            {
                stalled.CancelAfter(StallTimeout);
                await output.WriteAsync(buffer.AsMemory(0, length), token);
                hash.AppendData(buffer, 0, length);
                done += length;
                if (size > 0) onProgress?.Invoke(Math.Min(1, done / (double)size));
            }
        }
        catch
        {
            File.Delete(path + ".part");
            throw;
        }
        if (Convert.ToHexStringLower(hash.GetHashAndReset()) != manifest.Sha256)
        {
            File.Delete(path + ".part");
            throw new InvalidDataException("The downloaded Chromium does not match its published checksum.");
        }
        File.Move(path + ".part", path, overwrite: true);
    }

    private static void Unpack(string archive, string cacheDirectory, string folder)
    {
        var temporary = Path.Combine(cacheDirectory, ".extract-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        try
        {
            using (var zip = ZipFile.OpenRead(archive))
            {
                var prefix = Path.GetFullPath(temporary) + Path.DirectorySeparatorChar;
                foreach (var entry in zip.Entries)
                {
                    var path = Path.GetFullPath(Path.Combine(temporary, entry.FullName.Replace('\\', '/')));
                    if (!path.StartsWith(prefix, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                        throw new InvalidDataException("The Chromium archive contains an unsafe path.");
                    if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
                        throw new InvalidDataException("The Chromium archive must contain regular files, not symbolic links.");
                    if (entry.FullName.EndsWith('/')) { Directory.CreateDirectory(path); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    entry.ExtractToFile(path, overwrite: false);
                    if (!OperatingSystem.IsWindows())
                    {
                        var mode = (entry.ExternalAttributes >> 16) & 0x1FF;
                        if (mode != 0) File.SetUnixFileMode(path, (UnixFileMode)mode);
                    }
                }
            }
            File.WriteAllText(Path.Combine(temporary, ".complete"), "1");
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true); // an earlier, interrupted unpack
            Directory.Move(temporary, folder);
        }
        finally
        {
            if (Directory.Exists(temporary)) Directory.Delete(temporary, recursive: true);
        }
    }

    /// <summary>Hard links cost no disk space and outlive the cache copy; a volume without them gets a copy.</summary>
    private static void LinkInto(string source, string target)
    {
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(source, file);
            if (relative == ".complete") continue;
            var destination = Path.Combine(target, relative);
            if (File.Exists(destination))
            {
                // A link or copy from an earlier, interrupted run shares the cached file's size and write time.
                var existing = new FileInfo(destination); var cached = new FileInfo(file);
                if (existing.Length == cached.Length && existing.LastWriteTimeUtc == cached.LastWriteTimeUtc) continue;
                File.Delete(destination);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            if (!TryHardLink(file, destination)) File.Copy(file, destination);
        }
    }

    private static bool TryHardLink(string existing, string link)
    {
        try
        {
            return OperatingSystem.IsWindows() ? CreateHardLink(link, existing, IntPtr.Zero) : UnixLink(existing, link) == 0;
        }
        catch (Exception error) when (error is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Removes the downloads of other Chromium versions. Launchers that still use one keep their own links.</summary>
    private static void Prune(string cacheDirectory, string current)
    {
        foreach (var folder in Directory.EnumerateDirectories(cacheDirectory))
        {
            var name = Path.GetFileName(folder);
            if (name == current || !(name.Length == 64 && name.All(Uri.IsHexDigit))) continue;
            try
            {
                // A launcher still linking from that folder holds its lock: leave the folder to a later start.
                using var held = new FileStream(folder + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                Directory.Delete(folder, recursive: true);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }

    private static async Task<FileStream> AcquireLockAsync(string path, CancellationToken token)
    {
        var started = DateTime.UtcNow;
        while (true)
        {
            try { return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            // Another launcher is downloading the same Chromium: wait for it rather than download twice.
            catch (IOException) when (DateTime.UtcNow - started < TimeSpan.FromMinutes(30)) { await Task.Delay(250, token); }
        }
    }

    [DllImport("kernel32.dll", EntryPoint = "CreateHardLinkW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateHardLink(string fileName, string existingFileName, IntPtr securityAttributes);

    [DllImport("libc", EntryPoint = "link", SetLastError = true)]
    private static extern int UnixLink(string existing, string link);
}
