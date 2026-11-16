using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace NosCore.ClientLab.Bootstrap;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NosCore.ClientLab", "launcher");
        try
        {
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
            var executable = Path.Combine(folder, OperatingSystem.IsWindows() ? "ClientLab.exe" : "ClientLab");
            var start = new ProcessStartInfo(executable) { UseShellExecute = false, WorkingDirectory = folder };
            foreach (var argument in args) start.ArgumentList.Add(argument);
            using var child = Process.Start(start) ?? throw new IOException("Could not start the launcher.");
            await child.WaitForExitAsync();
            return child.ExitCode;
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
