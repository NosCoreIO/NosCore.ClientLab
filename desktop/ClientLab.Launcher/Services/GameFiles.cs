using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Brings one version's game folder in line with the official manifest while touching as little as possible.
/// A NosTale patch usually changes a handful of files, so every file the previous verified version already has with
/// the same SHA-1 is hard-linked (no copy, no extra disk, no re-hash: its verified size and timestamp are trusted).
/// Only the remaining files are imported from the player's installation or downloaded, several at a time, and hashed
/// while they stream. Game folders are served read-only and replaced file by file (write then rename), never edited in
/// place, so versions sharing a file can never affect each other.
/// </summary>
public sealed class GameFiles
{
    public const int DownloadWorkers = 6;
    private readonly Func<JsonNode, CancellationToken, Task<HttpResponseMessage>> fetch;
    private readonly Action<string> log;
    private readonly Action<string, double> status;

    public GameFiles(Func<JsonNode, CancellationToken, Task<HttpResponseMessage>> fetch, Action<string> log, Action<string, double> status)
    { this.fetch = fetch; this.log = log; this.status = status; }

    public int Downloaded { get; private set; }
    public int Linked { get; private set; }

    /// <summary>Files of earlier verified versions that are still exactly as verified, by relative name.</summary>
    public static Dictionary<string, (string Path, string Sha1)> Donors(IEnumerable<string> clients)
    {
        var donors = new Dictionary<string, (string, string)>(StringComparer.Ordinal);
        foreach (var client in clients)
            foreach (var (name, path, sha1) in VerifiedClientFiles.Intact(client))
                donors.TryAdd(name + "\0" + sha1, (path, sha1));
        return donors;
    }

    /// <summary>Bytes a sync into an empty folder would have to fetch from the network, given those donors.</summary>
    public static long DownloadBytes(JsonArray entries, Dictionary<string, (string Path, string Sha1)> donors) =>
        entries.Where(entry => !donors.ContainsKey(entry!["file"]!.GetValue<string>() + "\0" + entry["sha1"]!.GetValue<string>()))
            .Sum(entry => entry!["size"]!.GetValue<long>());

    public async Task SyncAsync(string client, string manifestId, JsonArray entries, IEnumerable<string> previousClients, string importDirectory, CancellationToken token)
    {
        Directory.CreateDirectory(client);
        if (VerifiedClientFiles.IsCurrent(client, manifestId, entries))
        {
            log($"All {entries.Count} game files are already verified; no download or patch is needed.");
            return;
        }
        var donors = Donors(previousClients.Where(path => !SamePath(path, client)));
        var pending = new List<JsonNode>();
        var checkedFiles = 0;
        foreach (var entry in entries)
        {
            token.ThrowIfCancellationRequested();
            var name = entry!["file"]!.GetValue<string>(); var sha1 = entry["sha1"]!.GetValue<string>(); var size = entry["size"]!.GetValue<long>();
            var target = Path.Combine(client, name);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            if (donors.TryGetValue(name + "\0" + sha1, out var donor))
            {
                Install(target, temporary => Link(donor.Path, temporary));
                Linked++;
                continue;
            }
            // A previous, interrupted attempt at this very version may already hold the file.
            status($"Checking game files: {++checkedFiles}", 0);
            if (File.Exists(target) && new FileInfo(target).Length == size && await HashAsync(target, token) == sha1) continue;
            var imported = importDirectory.Length > 0 ? Path.Combine(importDirectory, name) : "";
            if (imported.Length > 0 && File.Exists(imported) && new FileInfo(imported).Length == size && await HashAsync(imported, token) == sha1)
            {
                // Copied, never linked: the official launcher may patch its own folder in place.
                Install(target, temporary => File.Copy(imported, temporary, overwrite: true));
                continue;
            }
            pending.Add(entry);
        }
        if (Linked > 0) log($"Reused {Linked} unchanged game files from the previous version without copying them.");
        if (pending.Count > 0) await DownloadAllAsync(client, pending, token);
        VerifiedClientFiles.MarkVerified(client, manifestId, entries);
        log($"Verified {entries.Count} game files; downloaded {Downloaded}, reused {Linked} unchanged.");
    }

    private async Task DownloadAllAsync(string client, List<JsonNode> pending, CancellationToken token)
    {
        var total = Math.Max(1, pending.Sum(entry => entry["size"]!.GetValue<long>())); long received = 0; var finished = 0;
        void Report() => status($"Downloading {Volatile.Read(ref finished)}/{pending.Count} files · {Interlocked.Read(ref received) / 1048576} of {total / 1048576} MB",
            Interlocked.Read(ref received) / (double)total);
        Report();
        // Largest first, so one big archive does not start last and run alone.
        await Parallel.ForEachAsync(pending.OrderByDescending(entry => entry["size"]!.GetValue<long>()),
            new ParallelOptions { MaxDegreeOfParallelism = DownloadWorkers, CancellationToken = token }, async (entry, cancellation) =>
            {
                var name = entry["file"]!.GetValue<string>(); var sha1 = entry["sha1"]!.GetValue<string>(); var size = entry["size"]!.GetValue<long>();
                var target = Path.Combine(client, name); var temporary = target + ".part";
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA1);
                long written = 0;
                try
                {
                    using (var response = await fetch(entry, cancellation))
                    {
                        response.EnsureSuccessStatusCode();
                        await using var input = await response.Content.ReadAsStreamAsync(cancellation);
                        await using var output = File.Create(temporary);
                        var buffer = new byte[256 * 1024]; int length;
                        while ((length = await input.ReadAsync(buffer, cancellation)) > 0)
                        {
                            hash.AppendData(buffer, 0, length);
                            await output.WriteAsync(buffer.AsMemory(0, length), cancellation);
                            written += length; Interlocked.Add(ref received, length);
                            if (written > size) throw new InvalidDataException($"{name} is larger than its published size.");
                        }
                    }
                    if (written != size || Convert.ToHexStringLower(hash.GetHashAndReset()) != sha1)
                        throw new InvalidDataException($"Checksum mismatch for {name}. The downloaded file was not installed.");
                    File.Move(temporary, target, overwrite: true);
                }
                catch
                {
                    Interlocked.Add(ref received, -written);
                    try { File.Delete(temporary); } catch (IOException) { }
                    throw;
                }
                lock (this) Downloaded++;
                Interlocked.Increment(ref finished);
                log("Downloaded " + name);
                Report();
            });
    }

    private static void Install(string target, Action<string> create)
    {
        var temporary = target + ".installing";
        if (File.Exists(temporary)) File.Delete(temporary);
        create(temporary);
        File.Move(temporary, target, overwrite: true);
    }

    /// <summary>Hard link when the file system allows it (NTFS, APFS, ext4, ...), otherwise a plain copy.</summary>
    public static void Link(string existing, string link)
    {
        var linked = OperatingSystem.IsWindows() ? CreateHardLinkW(link, existing, IntPtr.Zero) : UnixLink(existing, link) == 0;
        if (!linked) File.Copy(existing, link, overwrite: true);
    }

    private static bool SamePath(string a, string b) =>
        string.Equals(Path.GetFullPath(a).TrimEnd(Path.DirectorySeparatorChar), Path.GetFullPath(b).TrimEnd(Path.DirectorySeparatorChar),
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    private static async Task<string> HashAsync(string path, CancellationToken token)
    {
        await using var file = File.OpenRead(path);
        return Convert.ToHexStringLower(await SHA1.HashDataAsync(file, token));
    }

    [DllImport("kernel32.dll", EntryPoint = "CreateHardLinkW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateHardLinkW(string fileName, string existingFileName, IntPtr securityAttributes);

    [DllImport("libc", EntryPoint = "link", SetLastError = true)]
    private static extern int UnixLink(string existing, string link);
}
