using System.Security.Cryptography;
using System.Text;

namespace NosCore.ClientLab.Launcher.Services;

public sealed class CompilationCache(string directory, string identity)
{
    private readonly string cacheDirectory = Path.Combine(directory, Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(identity))));

    public async Task<(string Path, bool Reused)> GetOrCompileAsync(string source, Func<string, CancellationToken, Task> compile, CancellationToken token)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        hash.AppendData(Encoding.UTF8.GetBytes(Path.GetFileName(source) + "\0"));
        await using (var input = File.OpenRead(source))
        {
            var buffer = new byte[65536];
            int length;
            while ((length = await input.ReadAsync(buffer, token)) > 0) hash.AppendData(buffer.AsSpan(0, length));
        }
        var file = Path.Combine(cacheDirectory, Convert.ToHexStringLower(hash.GetHashAndReset()) + ".o");
        Directory.CreateDirectory(cacheDirectory);
        if (File.Exists(file) && new FileInfo(file).Length > 0) return (file, true);
        var temporary = file + "." + Guid.NewGuid().ToString("N") + ".partial.o";
        try
        {
            await compile(temporary, token);
            token.ThrowIfCancellationRequested();
            if (!File.Exists(temporary) || new FileInfo(temporary).Length == 0)
                throw new InvalidDataException("The compiler did not produce a complete object file.");
            File.Move(temporary, file, overwrite: true);
            return (file, false);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); } catch (IOException) { }
        }
    }
}
