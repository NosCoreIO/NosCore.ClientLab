namespace NosCore.ClientLab.Launcher.Services;

/// <summary>Immutable output locations: a new attempt never rewrites a previous build's mapped files.</summary>
public static class BuildWorkspace
{
    public static string Create(string versionDirectory)
    {
        var path = Path.Combine(versionDirectory, "builds", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    public static async Task<FileStream> AcquireLeaseAsync(string dataDirectory, CancellationToken token)
    {
        var path = Path.Combine(dataDirectory, "build.lock");
        while (true)
        {
            token.ThrowIfCancellationRequested();
            try { return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            // Held by another launcher: Windows sharing/lock violation (32, 33), Linux EWOULDBLOCK (11),
            // macOS/BSD EWOULDBLOCK (35).
            catch (IOException error) when ((error.HResult & 0xFFFF) is 32 or 33 or 11 or 35)
            { await Task.Delay(250, token); }
        }
    }
}
