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
/// The values live in the official client's Chromium Local Storage (a LevelDB directory). There is no
/// public API for them, so this performs a tolerant byte scan of the LevelDB <c>.log</c>/<c>.ldb</c>
/// files rather than a full LevelDB parse: the two keys are short ASCII strings, and <c>x-vec</c> ends
/// in the Unix-millisecond timestamp it was last rotated at, which orders candidates by recency.
/// Any failure returns null and the caller falls back to an imported identity file or the browser collector.
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
        // rewrites the one for the version it is running, so prefer the most recently modified file.
        var files = Directory.EnumerateDirectories(clientRoot, "leveldb", SearchOption.AllDirectories)
            .Where(directory => directory.Replace('/', '\\').Contains(@"\Local Storage\", StringComparison.OrdinalIgnoreCase))
            .SelectMany(directory => Directory.EnumerateFiles(directory).Where(file => file.EndsWith(".log", StringComparison.OrdinalIgnoreCase) || file.EndsWith(".ldb", StringComparison.OrdinalIgnoreCase)))
            .Select(file => new FileInfo(file))
            .OrderByDescending(file => file.LastWriteTimeUtc)
            .Select(file => ReadShared(file.FullName))
            .ToArray();
        return Parse(files);
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
