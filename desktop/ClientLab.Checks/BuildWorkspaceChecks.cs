using System.IO.MemoryMappedFiles;
using NosCore.ClientLab.Launcher.Services;

internal static class BuildWorkspaceChecks
{
    public static async Task RunAsync()
    {
        var directory = Path.Combine(Path.GetTempPath(), "ClientLab-workspace-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var version = Path.Combine(directory, "versions", "same-client-version");
            var first = BuildWorkspace.Create(version);
            var mappedPath = Path.Combine(first, "fn_004b0000.c");
            await File.WriteAllTextAsync(mappedPath, "previous generated file");
            using (var mapped = MemoryMappedFile.CreateFromFile(mappedPath, FileMode.Open, null, 0, MemoryMappedFileAccess.Read))
            using (var view = mapped.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read))
            {
                var next = BuildWorkspace.Create(version);
                if (first == next) throw new Exception("A new build reused mapped output.");
                await File.WriteAllTextAsync(Path.Combine(next, "fn_004b0000.c"), "replacement generated file");
                if (await File.ReadAllTextAsync(mappedPath) != "previous generated file") throw new Exception("A new build changed the mapped old output.");
            }
            using (var firstLease = await BuildWorkspace.AcquireLeaseAsync(directory, default))
            {
                using var cancellation = new CancellationTokenSource(150);
                try
                {
                    using var secondLease = await BuildWorkspace.AcquireLeaseAsync(directory, cancellation.Token);
                    throw new Exception("Two launcher instances acquired the build lease.");
                }
                catch (OperationCanceledException) { }
            }
            using var releasedLease = await BuildWorkspace.AcquireLeaseAsync(directory, default);
        }
        finally { Directory.Delete(directory, recursive: true); }
        Console.WriteLine("PASS: build retry with mapped previous C output, exclusive build lease and cancellation while waiting.");
    }
}
