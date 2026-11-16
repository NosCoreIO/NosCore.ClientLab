using System.Security.Cryptography;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class VerifiedClientChecks
{
    public static void Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-verified-check-" + Guid.NewGuid().ToString("N"));
        var client = Path.Combine(root, "client"); Directory.CreateDirectory(client);
        try
        {
            var file = Path.Combine(client, "data.bin"); File.WriteAllBytes(file, [1, 2, 3]);
            var entries = new JsonArray(new JsonObject { ["file"] = "data.bin", ["size"] = 3L, ["sha1"] = Convert.ToHexStringLower(SHA1.HashData([1, 2, 3])) });
            if (VerifiedClientFiles.IsCurrent(client, "patch-a", entries)) throw new Exception("Unverified files were treated as current.");
            VerifiedClientFiles.MarkVerified(client, "patch-a", entries);
            if (!VerifiedClientFiles.IsCurrent(client, "patch-a", entries) || VerifiedClientFiles.IsCurrent(client, "patch-b", entries))
                throw new Exception("Verified patch identity was not respected.");
            File.WriteAllBytes(file, [3, 2, 1]); File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddSeconds(3));
            if (VerifiedClientFiles.IsCurrent(client, "patch-a", entries)) throw new Exception("Changed installed files were not invalidated.");
            File.Delete(file);
            if (VerifiedClientFiles.IsCurrent(client, "patch-a", entries)) throw new Exception("Missing installed file was accepted.");
            foreach (var malformed in new[] { "null", "{}", "{\"manifest\":\"patch-a\",\"files\":null}" })
            {
                File.WriteAllText(Path.Combine(root, "client-files.json"), malformed);
                if (VerifiedClientFiles.IsCurrent(client, "patch-a", entries)) throw new Exception("Malformed verification index was accepted.");
            }
            Console.WriteLine("PASS: verified game-file metadata skips unchanged patches and invalidates changed or missing files.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
