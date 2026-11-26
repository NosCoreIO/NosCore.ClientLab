using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using NosCore.ClientLab.Updates;

namespace NosCore.ClientLab.Bootstrap;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var cache = LauncherUpdate.CacheDirectory;
        var self = Environment.ProcessPath ?? "";
        var runtime = RuntimeInformation.RuntimeIdentifier;
        var version = OwnVersion();
        try
        {
            if (self.Length > 0 && args is not ["--verify-payload"])
            {
                LauncherUpdate.CleanupOld(self);
                // A launcher downloaded earlier is applied before anything starts, so the player always runs the newest.
                if (Environment.GetEnvironmentVariable("CLIENTLAB_UPDATED") != "1" && TryApplyUpdate(cache, self, version, runtime))
                    return await RunAsync(self, args, new() { ["CLIENTLAB_UPDATED"] = "1" });
            }
            using var payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("LauncherPayload.zip")
                ?? throw new InvalidDataException("The launcher payload is missing. Download a complete release.");
            var digest = Convert.ToHexStringLower(await SHA256.HashDataAsync(payload));
            payload.Position = 0;
            var folder = Path.Combine(cache, digest);
            Directory.CreateDirectory(cache);
            using (await AcquireLockAsync(Path.Combine(cache, digest + ".lock")))
            {
                if (!File.Exists(Path.Combine(folder, ".complete")))
                    await ExtractAsync(payload, cache, folder);
            }
            if (args is ["--verify-payload"]) return 0;
            PruneOldPayloads(cache, folder);
            var executable = Path.Combine(folder, OperatingSystem.IsWindows() ? "ClientLab.exe" : "ClientLab");
            var exitCode = await RunAsync(executable, args, new()
            {
                ["CLIENTLAB_BOOTSTRAP_PATH"] = self, ["CLIENTLAB_RUNTIME_ID"] = runtime, ["CLIENTLAB_LAUNCHER_VERSION"] = version,
            }, folder);
            // The launcher asks for a restart once it has staged a newer version: swap, then start the new bootstrap.
            if (exitCode == LauncherUpdate.RestartExitCode && self.Length > 0)
            {
                // Reopen even when the swap is impossible (read-only install): the player asked for a restart, not an exit.
                TryApplyUpdate(cache, self, version, runtime);
                StartDetached(self, args);
                return 0;
            }
            return exitCode;
        }
        catch (Exception error)
        {
            var message = "ClientLab could not start: " + error.Message;
            Console.Error.WriteLine(message);
            try { Directory.CreateDirectory(cache); await File.WriteAllTextAsync(Path.Combine(cache, "startup-error.log"), error.ToString()); } catch { }
            if (OperatingSystem.IsWindows()) MessageBox(IntPtr.Zero, message, "ClientLab", 0x10);
            return 1;
        }
    }

    private static string OwnVersion()
    {
        var informational = typeof(Program).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0";
        var plus = informational.IndexOf('+');
        return plus >= 0 ? informational[..plus] : informational;
    }

    private static bool TryApplyUpdate(string cache, string self, string version, string runtime)
    {
        try
        {
            var updates = LauncherUpdate.UpdatesDirectory(cache);
            if (LauncherUpdate.FindStaged(updates, version, runtime) is not { } staged) return false;
            if (!LauncherUpdate.Apply(updates, staged, self)) return false;
            ResealAppBundle(self);
            return true;
        }
        catch (Exception error)
        {
            // An update that cannot be applied never stops the current version from starting.
            try { Directory.CreateDirectory(cache); File.WriteAllText(Path.Combine(cache, "update-error.log"), error.ToString()); } catch { }
            return false;
        }
    }

    /// <summary>Inside a macOS .app the replaced executable breaks the bundle's ad-hoc seal; sign it again.</summary>
    private static void ResealAppBundle(string self)
    {
        if (!OperatingSystem.IsMacOS()) return;
        var macOs = Path.GetDirectoryName(self)!;
        var bundle = Path.GetDirectoryName(Path.GetDirectoryName(macOs)!)!;
        if (Path.GetFileName(macOs) != "MacOS" || !bundle.EndsWith(".app", StringComparison.Ordinal)) return;
        try
        {
            var sign = new ProcessStartInfo("/usr/bin/codesign") { UseShellExecute = false };
            foreach (var argument in new[] { "--force", "--deep", "-s", "-", bundle }) sign.ArgumentList.Add(argument);
            using var process = Process.Start(sign);
            process?.WaitForExit(30000);
        }
        catch (Exception) { }
    }

    private static async Task<int> RunAsync(string executable, string[] args, Dictionary<string, string> environment, string? workingDirectory = null)
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, WorkingDirectory = workingDirectory ?? Path.GetDirectoryName(executable)! };
        foreach (var argument in args) start.ArgumentList.Add(argument);
        foreach (var (key, value) in environment) start.Environment[key] = value;
        using var child = Process.Start(start) ?? throw new IOException("Could not start the launcher.");
        await child.WaitForExitAsync();
        return child.ExitCode;
    }

    private static void StartDetached(string executable, string[] args)
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(executable)! };
        foreach (var argument in args) start.ArgumentList.Add(argument);
        start.Environment["CLIENTLAB_UPDATED"] = "1";
        Process.Start(start)?.Dispose();
    }

    /// <summary>Each release extracts to its own folder (a few hundred MB with Chromium). Keep the current one and the
    /// newest other, which a second launcher instance may still be running, and remove older ones best-effort.</summary>
    private static void PruneOldPayloads(string cache, string current)
    {
        try
        {
            var stale = Directory.EnumerateDirectories(cache)
                .Where(path => !string.Equals(Path.GetFullPath(path), Path.GetFullPath(current), StringComparison.OrdinalIgnoreCase) &&
                    System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(path), "^[0-9a-f]{64}$") && File.Exists(Path.Combine(path, ".complete")))
                .OrderByDescending(path => File.GetLastWriteTimeUtc(Path.Combine(path, ".complete")))
                .Skip(1)
                .ToArray();
            foreach (var folder in stale)
            {
                try { Directory.Delete(folder, recursive: true); File.Delete(folder + ".lock"); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    private static async Task<FileStream> AcquireLockAsync(string path)
    {
        var timeout = Stopwatch.StartNew();
        while (true)
        {
            try { return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException) when (timeout.Elapsed < TimeSpan.FromMinutes(2)) { await Task.Delay(200); }
        }
    }

    private static async Task ExtractAsync(Stream payload, string cache, string folder)
    {
        var temporary = Path.Combine(cache, ".extract-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        try
        {
            using var zip = new ZipArchive(payload, ZipArchiveMode.Read, leaveOpen: true);
            var prefix = Path.GetFullPath(temporary) + Path.DirectorySeparatorChar;
            foreach (var entry in zip.Entries)
            {
                var path = Path.GetFullPath(Path.Combine(temporary, entry.FullName.Replace('\\', '/')));
                if (!path.StartsWith(prefix, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
                    throw new InvalidDataException("The launcher archive contains an unsafe path.");
                if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
                    throw new InvalidDataException("The launcher archive must contain regular files, not symbolic links.");
                if (entry.FullName.EndsWith('/')) { Directory.CreateDirectory(path); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                await using (var input = entry.Open())
                await using (var output = File.Create(path)) await input.CopyToAsync(output);
                if (!OperatingSystem.IsWindows())
                {
                    var mode = (entry.ExternalAttributes >> 16) & 0x1FF;
                    if (mode != 0) File.SetUnixFileMode(path, (UnixFileMode)mode);
                }
            }
            var executable = Path.Combine(temporary, OperatingSystem.IsWindows() ? "ClientLab.exe" : "ClientLab");
            if (!File.Exists(executable)) throw new InvalidDataException("The launcher archive is incomplete.");
            await File.WriteAllTextAsync(Path.Combine(temporary, ".complete"), "1");
            // A failed extraction never replaces a completed payload. Each release has a different digest.
            Directory.Move(temporary, folder);
        }
        finally
        {
            // This exact directory was created above, inside our own launcher cache.
            if (Directory.Exists(temporary)) Directory.Delete(temporary, recursive: true);
        }
    }

    [DllImport("user32.dll", EntryPoint = "MessageBoxW", CharSet = CharSet.Unicode)]
    private static extern int MessageBox(IntPtr owner, string text, string caption, uint type);
}
