using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public static class VerifiedClientFiles
{
    private static string IndexPath(string client) => Path.Combine(Path.GetDirectoryName(client)!, "client-files.json");

    public static bool IsCurrent(string client, string manifestId, JsonArray entries)
    {
        try
        {
            if (JsonNode.Parse(File.ReadAllText(IndexPath(client))) is not JsonObject index ||
                index["manifest"]?.GetValue<string>() != manifestId || index["files"] is not JsonObject files) return false;
            foreach (var entry in entries)
            {
                var name = entry!["file"]!.GetValue<string>();
                var file = new FileInfo(Path.Combine(client, name));
                var stamp = files[name];
                if (!file.Exists || stamp is null || stamp["sha1"]?.GetValue<string>() != entry["sha1"]!.GetValue<string>() ||
                    file.Length != entry["size"]!.GetValue<long>() || file.Length != stamp["size"]?.GetValue<long>() ||
                    file.LastWriteTimeUtc.Ticks != stamp["modified"]?.GetValue<long>()) return false;
            }
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidOperationException or KeyNotFoundException)
        { return false; }
    }

    /// <summary>The files of a verified folder whose size and timestamp still match the verification: name, path, SHA-1.</summary>
    public static IEnumerable<(string Name, string Path, string Sha1)> Intact(string client)
    {
        JsonObject? files;
        try { files = JsonNode.Parse(File.ReadAllText(IndexPath(client)))?["files"] as JsonObject; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidOperationException)
        { yield break; }
        if (files is null) yield break;
        foreach (var (name, stamp) in files)
        {
            if (stamp is not JsonObject value || value["sha1"]?.GetValue<string>() is not { } sha1) continue;
            var path = Path.Combine(client, name);
            var file = new FileInfo(path);
            if (file.Exists && file.Length == value["size"]?.GetValue<long>() && file.LastWriteTimeUtc.Ticks == value["modified"]?.GetValue<long>())
                yield return (name, path, sha1);
        }
    }

    // Commit only after the caller has verified/copied every file against the publisher's checksum.
    public static void MarkVerified(string client, string manifestId, JsonArray entries)
    {
        var files = new JsonObject();
        foreach (var entry in entries)
        {
            var name = entry!["file"]!.GetValue<string>();
            var file = new FileInfo(Path.Combine(client, name));
            files[name] = new JsonObject { ["sha1"] = entry["sha1"]!.GetValue<string>(), ["size"] = file.Length, ["modified"] = file.LastWriteTimeUtc.Ticks };
        }
        var target = IndexPath(client); var temporary = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
        File.WriteAllText(temporary, new JsonObject { ["manifest"] = manifestId, ["files"] = files }.ToJsonString());
        File.Move(temporary, target, overwrite: true);
    }
}
