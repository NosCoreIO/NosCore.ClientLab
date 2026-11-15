using System.Reflection;
using System.Security.Cryptography;

namespace NosCore.ClientLab.Launcher.Services;

internal static class SourceAssets
{
    public static string Resolve()
    {
        if (Program.Option("--source-root") is { } configured) return Path.GetFullPath(configured);
        var assembly = Assembly.GetExecutingAssembly();
        var names = assembly.GetManifestResourceNames().Where(name => name.StartsWith("Source/", StringComparison.Ordinal)).Order().ToArray();
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var name in names)
        {
            using var resource = assembly.GetManifestResourceStream(name)!;
            using var memory = new MemoryStream(); resource.CopyTo(memory); hash.AppendData(memory.ToArray());
        }
        var fingerprint = Convert.ToHexStringLower(hash.GetHashAndReset())[..16];
        var root = Path.Combine(Program.DataDirectory, "sources", fingerprint);
        foreach (var name in names)
        {
            var target = Path.Combine(root, name[7..].Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(target)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using var resource = assembly.GetManifestResourceStream(name)!;
            using var output = File.Create(target); resource.CopyTo(output);
        }
        return root;
    }
}
