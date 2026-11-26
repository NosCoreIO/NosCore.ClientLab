using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Updates;

/// <summary>A release's launcher executable for one runtime, as listed by release-manifest.json.</summary>
public sealed record ReleaseAsset(string Name, string Sha256);

/// <summary>The release-manifest.json attached to every published release.</summary>
public sealed record ReleaseManifest(string Version, IReadOnlyDictionary<string, ReleaseAsset> Assets)
{
    public static ReleaseManifest Parse(string json)
    {
        var root = JsonNode.Parse(json) as JsonObject ?? throw new InvalidDataException("The release manifest is not an object.");
        var version = root["version"]?.GetValue<string>() ?? throw new InvalidDataException("The release manifest has no version.");
        if (!LauncherUpdate.TryParseVersion(version, out _)) throw new InvalidDataException("The release manifest version is not a semantic version.");
        var assets = new Dictionary<string, ReleaseAsset>(StringComparer.Ordinal);
        if (root["assets"] is JsonObject list)
            foreach (var (runtime, node) in list)
            {
                var name = node?["name"]?.GetValue<string>() ?? "";
                var sha = node?["sha256"]?.GetValue<string>()?.ToLowerInvariant() ?? "";
                // Asset names become local file names: no directories, no traversal.
                if (name.Length == 0 || name != Path.GetFileName(name) || name.Contains("..") || name.IndexOfAny(['/', '\\', ':']) >= 0)
                    throw new InvalidDataException("The release manifest has an unsafe asset name.");
                if (sha.Length != 64 || !sha.All(Uri.IsHexDigit)) throw new InvalidDataException("The release manifest has an invalid checksum.");
                assets[runtime] = new ReleaseAsset(name, sha);
            }
        return new ReleaseManifest(version, assets);
    }
}

/// <summary>A downloaded, checksum-verified launcher waiting for the bootstrap to swap it in.</summary>
public sealed record StagedUpdate(string Version, string Runtime, string File, string Sha256);

/// <summary>
/// Self-update shared by the bootstrap (which swaps its own executable) and the launcher (which downloads in the
/// background). The launcher only ever stages; the bootstrap is the only process that replaces an executable.
/// </summary>
public static class LauncherUpdate
{
    public const string ManifestUrl = "https://github.com/NosCoreIO/NosCore.ClientLab/releases/latest/download/release-manifest.json";
    /// <summary>Assets are fetched from the manifest's own release (<c>v{version}/</c>), so a release published between the
    /// two requests cannot turn the download into a 404.</summary>
    public const string AssetBaseUrl = "https://github.com/NosCoreIO/NosCore.ClientLab/releases/download/";
    /// <summary>The launcher exits with this code to ask the bootstrap to apply the staged update and start again.</summary>
    public const int RestartExitCode = 75;
    private const string StagedRecord = "staged.json";

    public static string CacheDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NosCore.ClientLab", "launcher");
    public static string UpdatesDirectory(string cache) => Path.Combine(cache, "updates");

    public static bool TryParseVersion(string text, out (int Major, int Minor, int Patch, string Pre) version)
    {
        version = default;
        text = text.Trim().TrimStart('v');
        var plus = text.IndexOf('+'); if (plus >= 0) text = text[..plus];
        var dash = text.IndexOf('-');
        var pre = dash >= 0 ? text[(dash + 1)..] : "";
        var parts = (dash >= 0 ? text[..dash] : text).Split('.');
        if (parts.Length != 3 || !int.TryParse(parts[0], out var major) || !int.TryParse(parts[1], out var minor) || !int.TryParse(parts[2], out var patch) ||
            major < 0 || minor < 0 || patch < 0 || (dash >= 0 && pre.Length == 0)) return false;
        version = (major, minor, patch, pre);
        return true;
    }

    /// <summary>Semantic-version order; a prerelease sorts before its release. Unparseable versions sort first.</summary>
    public static int CompareVersions(string left, string right)
    {
        var okLeft = TryParseVersion(left, out var a); var okRight = TryParseVersion(right, out var b);
        if (!okLeft || !okRight) return okLeft.CompareTo(okRight);
        var order = a.Major.CompareTo(b.Major);
        if (order == 0) order = a.Minor.CompareTo(b.Minor);
        if (order == 0) order = a.Patch.CompareTo(b.Patch);
        if (order != 0) return order;
        if (a.Pre.Length == 0 || b.Pre.Length == 0) return b.Pre.Length.CompareTo(a.Pre.Length);
        var x = a.Pre.Split('.'); var y = b.Pre.Split('.');
        for (var i = 0; i < Math.Min(x.Length, y.Length); i++)
        {
            var numeric = int.TryParse(x[i], out var p) & int.TryParse(y[i], out var q);
            var part = numeric ? p.CompareTo(q) : string.CompareOrdinal(x[i], y[i]);
            if (part != 0) return part;
        }
        return x.Length.CompareTo(y.Length);
    }

    public static string Sha256Of(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(file));
    }

    /// <summary>Records a verified download as staged. Rejects files whose checksum does not match the manifest.</summary>
    public static StagedUpdate Stage(string updates, string downloaded, ReleaseManifest manifest, string runtime)
    {
        var asset = manifest.Assets[runtime];
        if (Sha256Of(downloaded) != asset.Sha256)
        {
            File.Delete(downloaded);
            throw new InvalidDataException($"The downloaded launcher {manifest.Version} does not match its published checksum.");
        }
        Directory.CreateDirectory(updates);
        var target = Path.Combine(updates, asset.Name);
        if (!string.Equals(Path.GetFullPath(downloaded), Path.GetFullPath(target), StringComparison.Ordinal)) File.Move(downloaded, target, overwrite: true);
        var staged = new StagedUpdate(manifest.Version, runtime, asset.Name, asset.Sha256);
        var record = Path.Combine(updates, StagedRecord);
        File.WriteAllText(record + ".tmp", JsonSerializer.Serialize(new JsonObject
        {
            ["version"] = staged.Version, ["runtime"] = staged.Runtime, ["file"] = staged.File, ["sha256"] = staged.Sha256
        }));
        File.Move(record + ".tmp", record, overwrite: true);
        return staged;
    }

    /// <summary>The staged update, when it is newer than <paramref name="currentVersion"/>, for this runtime, and its file
    /// still matches the checksum recorded when it was staged. Anything else is ignored (and stale stages removed).</summary>
    public static StagedUpdate? FindStaged(string updates, string currentVersion, string runtime)
    {
        try
        {
            var record = Path.Combine(updates, StagedRecord);
            if (!File.Exists(record)) return null;
            var node = JsonNode.Parse(File.ReadAllText(record));
            var staged = new StagedUpdate(node?["version"]?.GetValue<string>() ?? "", node?["runtime"]?.GetValue<string>() ?? "",
                node?["file"]?.GetValue<string>() ?? "", node?["sha256"]?.GetValue<string>() ?? "");
            var file = Path.Combine(updates, staged.File);
            if (staged.File != Path.GetFileName(staged.File) || staged.Runtime != runtime || CompareVersions(staged.Version, currentVersion) <= 0 ||
                !File.Exists(file) || Sha256Of(file) != staged.Sha256)
            {
                Discard(updates);
                return null;
            }
            return staged;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
        { return null; }
    }

    public static void Discard(string updates)
    {
        try { if (Directory.Exists(updates)) Directory.Delete(updates, recursive: true); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>Replaces <paramref name="target"/> (normally the running bootstrap) with the staged executable.
    /// Windows lets a running executable be renamed but not overwritten, so it moves aside to <c>.old</c>; Unix replaces
    /// the directory entry atomically, and the running process keeps its old inode. Returns false when the target's
    /// folder is not writable (a mounted disk image, a read-only install), leaving everything as it was.</summary>
    public static bool Apply(string updates, StagedUpdate staged, string target)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(target))!;
        if (!IsWritable(directory)) return false;
        var source = Path.Combine(updates, staged.File);
        var incoming = target + ".new";
        File.Copy(source, incoming, overwrite: true);
        if (Sha256Of(incoming) != staged.Sha256) { File.Delete(incoming); throw new InvalidDataException("The staged launcher changed after it was verified."); }
        if (OperatingSystem.IsWindows())
        {
            // A unique name: the bootstrap moved aside by an earlier update in this session may still be running.
            var old = target + "." + Guid.NewGuid().ToString("N")[..8] + ".old";
            File.Move(target, old);
            try { File.Move(incoming, target); }
            catch { File.Move(old, target); throw; }
        }
        else
        {
            File.SetUnixFileMode(incoming, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                UnixFileMode.GroupRead | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
            File.Move(incoming, target, overwrite: true);
        }
        Discard(updates);
        return true;
    }

    /// <summary>Removes the executable a previous Windows update moved aside, once it is no longer running.</summary>
    public static void CleanupOld(string target)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(target))!;
        foreach (var old in Directory.EnumerateFiles(directory, Path.GetFileName(target) + ".*old"))
        {
            try { File.Delete(old); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { } // Still running; removed next time.
        }
        try { if (File.Exists(target + ".new")) File.Delete(target + ".new"); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    private static bool IsWritable(string directory)
    {
        try
        {
            var probe = Path.Combine(directory, ".clientlab-write-" + Guid.NewGuid().ToString("N"));
            File.WriteAllText(probe, "");
            File.Delete(probe);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }
    }
}
