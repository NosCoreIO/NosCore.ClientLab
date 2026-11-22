using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Reads the device identity (<c>x-game</c>/<c>x-vec</c>) that the official Gameforge client registered
/// on this machine, so ClientLab reuses the registered identifier instead of minting a fresh one that
/// Gameforge has never seen. An unregistered device identifier passes sign-in and iovation but makes
/// <c>auth/thin/codes</c> fail with HTTP 403 "Not allowed to create code".
///
/// The values live in the official client's Chromium Local Storage (a LevelDB directory). The records are
/// read with <see cref="LevelDbReader"/> so the newest value wins even inside Snappy-compressed tables (a raw
/// byte scan misses those and returns an older, no longer registered device id); a tolerant byte scan remains
/// as a fallback. Any failure returns null and the caller falls back to an imported identity file or the browser collector.
/// </summary>
public static class GameforgeOfficialClientIdentity
{
    // Latin-1 keeps every byte 1:1 so the regexes below match the raw record bytes, printable or not.
    private static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

    public static JsonObject? TryRead()
    {
        if (!OperatingSystem.IsWindows()) return null;
        try { return TryReadCore(); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Security.SecurityException) { return null; }
    }

    private static JsonObject? TryReadCore()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var clientRoot = Path.Combine(local, "Gameforge4d", "GameforgeClient");
        if (!Directory.Exists(clientRoot)) return null;

        // Each Chromium cache (webcache, webcache-c<version>, …) has its own Local Storage; the client
        // rewrites the one for the version it is running, so prefer the most recently modified database.
        var databases = Directory.EnumerateDirectories(clientRoot, "leveldb", SearchOption.AllDirectories)
            .Where(directory => directory.Replace('/', '\\').Contains(@"\Local Storage\", StringComparison.OrdinalIgnoreCase))
            .Select(directory => Directory.EnumerateFiles(directory)
                .Where(file => file.EndsWith(".log", StringComparison.OrdinalIgnoreCase) || file.EndsWith(".ldb", StringComparison.OrdinalIgnoreCase))
                .Select(file => new FileInfo(file)).OrderByDescending(file => file.LastWriteTimeUtc).ToArray())
            .Where(files => files.Length > 0)
            .OrderByDescending(files => files[0].LastWriteTimeUtc)
            .ToArray();
        foreach (var files in databases)
            if (ParseDatabase(files.Select(file => (file.Name, ReadShared(file.FullName)))) is { } identity) return identity;
        // Fallback for storage the record reader cannot walk: a tolerant scan of the raw bytes.
        return Parse(databases.SelectMany(files => files).OrderByDescending(file => file.LastWriteTimeUtc).Select(file => ReadShared(file.FullName)).ToArray());
    }

    /// <summary>
    /// Reads <c>x-game</c>/<c>x-vec</c> from one Local Storage LevelDB database as records, so the newest value
    /// wins by sequence number even when a compacted, Snappy-compressed table holds it. Returns null when either key is absent.
    /// </summary>
    public static JsonObject? ParseDatabase(IEnumerable<(string Name, byte[] Data)> files)
    {
        string? game = null, vector = null;
        foreach (var (key, value) in LevelDbReader.ReadLatest(files))
        {
            // Chromium keys are "_<origin>\0<encoding byte><script key>"; values are "<encoding byte><text>".
            if (!key.StartsWith("_https://", StringComparison.Ordinal) || !key.Contains("gameforge.com\0", StringComparison.Ordinal)) continue;
            var name = key[(key.IndexOf('\0') + 1)..];
            if (name.Length > 0 && name[0] is '\u0001' or '\u0000') name = name[1..];
            if (name is not ("x-game" or "x-vec") || value.Length == 0) continue;
            var text = value[0] == 0 ? Encoding.Unicode.GetString(value, 1, value.Length - 1) : Latin1.GetString(value, 1, value.Length - 1);
            if (name == "x-game") game = text; else vector = text;
        }
        if (game is not { Length: > 0 and <= 4096 } || vector is not { Length: > 0 and <= 4096 }) return null;
        return new JsonObject { ["x-game"] = game, ["x-vec"] = vector };
    }

    /// <summary>
    /// Extracts <c>x-game</c>/<c>x-vec</c> from raw Local Storage LevelDB file bytes, ordered newest file first.
    /// Split out from disk access so the byte scan is testable on any platform. Returns null when either key is absent.
    /// </summary>
    public static JsonObject? Parse(IReadOnlyList<byte[]> blobsNewestFirst)
    {
        string? game = null; string? vector = null; long vectorStamp = -1;
        foreach (var blob in blobsNewestFirst)
        {
            var text = Latin1.GetString(blob);
            // x-game is a stable 20-40 char lowercase/digit token written straight after the key framing.
            foreach (Match key in Regex.Matches(text, "x-game"))
            {
                var window = text.Substring(key.Index + 6, Math.Min(64, text.Length - key.Index - 6));
                var value = Regex.Match(window, "[a-z0-9]{20,40}");
                if (value.Success) game ??= value.Value; // blobs are newest-first, so keep the first hit
            }
            // x-vec is a printable run ending in " <unix-ms>"; the largest timestamp is the latest rotation.
            foreach (Match key in Regex.Matches(text, "x-vec"))
            {
                var window = text.Substring(key.Index + 5, Math.Min(280, text.Length - key.Index - 5));
                foreach (Match run in Regex.Matches(window, "[\\x20-\\x7e]{20,240}"))
                {
                    var stampMatch = Regex.Match(run.Value, " (\\d{10,16})$");
                    if (stampMatch.Success && long.TryParse(stampMatch.Groups[1].Value, out var stamp) && stamp > vectorStamp)
                    { vector = run.Value; vectorStamp = stamp; }
                }
            }
            // No early exit: the device id is the first (newest-file) hit, but the latest vector rotation
            // may live in any file, so every blob is scanned for a higher timestamp.
        }

        if (game is null || vector is null) return null;
        if (game.Length is 0 or > 4096 || vector.Length is 0 or > 4096) return null;
        return new JsonObject { ["x-game"] = game, ["x-vec"] = vector };
    }

    private static byte[] ReadShared(string path)
    {
        // The official client may be running and holding the LevelDB open.
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var buffer = new MemoryStream();
        stream.CopyTo(buffer);
        return buffer.ToArray();
    }
}
