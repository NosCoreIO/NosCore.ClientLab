using System.Diagnostics;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>Preserves the client's own Config.ini bytes independently of patches and runtime versions.</summary>
public sealed class GameSettingsStore(string dataDirectory)
{
    public const int MaximumBytes = 1024 * 1024;
    private readonly string directory = Path.Combine(Path.GetFullPath(dataDirectory), "game-settings");
    public string FilePath => Path.Combine(directory, "Config.ini");

    public async Task<byte[]?> ReadAsync(IEnumerable<string>? gameDirectories = null)
    {
        if (File.Exists(FilePath)) return await ReadFileAsync(FilePath);
        foreach (var gameDirectory in gameDirectories ?? [])
        {
            if (string.IsNullOrWhiteSpace(gameDirectory)) continue;
            foreach (var candidate in CandidateFiles(gameDirectory))
            {
                if (!File.Exists(candidate)) continue;
                var bytes = await ReadFileAsync(candidate);
                await WriteAsync(bytes, onlyIfMissing: true);
                return await ReadFileAsync(FilePath);
            }
        }
        return null;
    }

    public Task WriteAsync(ReadOnlyMemory<byte> bytes) => WriteAsync(bytes, onlyIfMissing: false);

    public async Task<GameOptions> ReadOptionsAsync(IEnumerable<string>? gameDirectories = null) => GameOptionsIni.Read(await ReadAsync(gameDirectories));

    public async Task UpdateOptionsAsync(GameOptions options, IEnumerable<string>? gameDirectories = null)
    {
        await ReadAsync(gameDirectories);
        Directory.CreateDirectory(directory);
        await using var lease = await AcquireLeaseAsync();
        var current = File.Exists(FilePath) ? await ReadFileAsync(FilePath) : null;
        await ReplaceAsync(GameOptionsIni.Update(current, options));
    }

    private async Task WriteAsync(ReadOnlyMemory<byte> bytes, bool onlyIfMissing)
    {
        if (bytes.Length > MaximumBytes) throw new InvalidDataException("The game's settings file is too large.");
        Directory.CreateDirectory(directory);
        await using var lease = await AcquireLeaseAsync();
        if (onlyIfMissing && File.Exists(FilePath)) return;
        await ReplaceAsync(bytes);
    }

    private async Task ReplaceAsync(ReadOnlyMemory<byte> bytes)
    {
        if (bytes.Length > MaximumBytes) throw new InvalidDataException("The game's settings file is too large.");
        var temporary = Path.Combine(directory, Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            await File.WriteAllBytesAsync(temporary, bytes.ToArray());
            File.Move(temporary, FilePath, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private async Task<FileStream> AcquireLeaseAsync()
    {
        var timeout = Stopwatch.StartNew();
        while (true)
        {
            try { return new FileStream(Path.Combine(directory, ".write.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException error) when ((error.HResult & 0xFFFF) is 32 or 33 or 11 && timeout.Elapsed < TimeSpan.FromSeconds(5))
            { await Task.Delay(10); }
        }
    }

    private static async Task<byte[]> ReadFileAsync(string path)
    {
        // Readers allow the completed file to be replaced atomically by another
        // launcher/game process; they never observe a half-written INI file.
        await using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (file.Length > MaximumBytes) throw new InvalidDataException("The game's settings file is too large.");
        var result = new byte[(int)file.Length];
        await file.ReadExactlyAsync(result);
        return result;
    }

    private static IEnumerable<string> CandidateFiles(string gameDirectory)
    {
        var full = Path.GetFullPath(gameDirectory);
        yield return Path.Combine(full, "Config.ini");
        if (!OperatingSystem.IsWindows()) yield return Path.Combine(full, "config.ini");
        else if (Path.GetPathRoot(full) is { Length: > 0 } root)
        {
            // A non-elevated official Windows client can be redirected here when
            // its installation is under Program Files. Import only Config.ini.
            yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "VirtualStore", full[root.Length..], "Config.ini");
        }
    }
}
