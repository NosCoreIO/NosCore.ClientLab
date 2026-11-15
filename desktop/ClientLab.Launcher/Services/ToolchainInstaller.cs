using System.IO.Compression;
using System.Security.Cryptography;

namespace NosCore.ClientLab.Launcher.Services;

internal static class ToolchainInstaller
{
    private const string EmsdkVersion = "4.0.19";

    public static async Task<Dictionary<string, string>> InstallAsync(string directory, LauncherCoordinator app, CancellationToken token)
    {
        var tools = Path.Combine(directory, "tools"); Directory.CreateDirectory(tools);
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(20) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("NosCore.ClientLab/0.1");
        app.Status("tools", "Setting up the compiler's private runtime…");
        var python = await PythonBootstrap.EnsureAsync(directory, text => app.Status("tools", text));
        token.ThrowIfCancellationRequested();
        var sdk = Path.Combine(tools, "emsdk-" + EmsdkVersion);
        if (!Directory.Exists(sdk))
        {
            app.Status("tools", "Installing the WebAssembly compiler…");
            var archive = Path.Combine(tools, "emsdk.zip");
            await DownloadAsync(http, $"https://github.com/emscripten-core/emsdk/archive/refs/tags/{EmsdkVersion}.zip", archive, null, app, token);
            await ExtractAsync(archive, tools, token);
        }
        var environment = new Dictionary<string, string> { ["EMSDK_PYTHON"] = python };
        app.Status("tools", "Preparing the WebAssembly compiler…");
        await app.RunAsync(python, [Path.Combine(sdk, "emsdk.py"), "install", EmsdkVersion], token, environment);
        await app.RunAsync(python, [Path.Combine(sdk, "emsdk.py"), "activate", EmsdkVersion], token, environment);
        var wrapper = Path.Combine(tools, OperatingSystem.IsWindows() ? "emcc.cmd" : "emcc");
        var compiler = Path.Combine(sdk, "upstream/emscripten/emcc.py");
        if (OperatingSystem.IsWindows())
            await File.WriteAllTextAsync(wrapper, $"@echo off\r\n\"{python}\" \"{compiler}\" %*\r\n", token);
        else
        {
            static string Quote(string value) => "'" + value.Replace("'", "'\\''") + "'";
            await File.WriteAllTextAsync(wrapper, $"#!/bin/sh\nexec {Quote(python)} {Quote(compiler)} \"$@\"\n", token);
            File.SetUnixFileMode(wrapper, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        }
        return new() { ["python"] = python, ["emcc"] = wrapper };
    }

    private static async Task DownloadAsync(HttpClient http, string url, string path, string? checksum, LauncherCoordinator app, CancellationToken token)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.Scheme != "https") throw new InvalidDataException("Build tools must be downloaded over HTTPS.");
        using var response = await http.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, token); response.EnsureSuccessStatusCode();
        await using (var output = File.Create(path + ".part"))
        await using (var input = await response.Content.ReadAsStreamAsync(token))
        {
            var buffer = new byte[1024 * 1024]; long done = 0; var size = response.Content.Headers.ContentLength ?? 0;
            int length;
            while ((length = await input.ReadAsync(buffer, token)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, length), token); done += length;
                app.Progress(size > 0 ? done / (double)size : 0);
            }
        }
        if (!string.IsNullOrWhiteSpace(checksum))
        {
            await using var input = File.OpenRead(path + ".part");
            if (Convert.ToHexStringLower(await SHA256.HashDataAsync(input, token)) != checksum)
                throw new InvalidDataException("The downloaded tool checksum did not match.");
        }
        File.Move(path + ".part", path, overwrite: true);
    }

    private static async Task ExtractAsync(string archive, string directory, CancellationToken token)
    {
        Directory.CreateDirectory(directory);
        if (archive.EndsWith(".zip", StringComparison.Ordinal))
        {
            using var zip = ZipFile.OpenRead(archive);
            var root = Path.GetFullPath(directory) + Path.DirectorySeparatorChar;
            foreach (var entry in zip.Entries)
            {
                token.ThrowIfCancellationRequested();
                var path = Path.GetFullPath(Path.Combine(directory, entry.FullName.Replace('\\', '/')));
                if (!path.StartsWith(root, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)) throw new InvalidDataException("Unsafe tool archive path.");
                var mode = entry.ExternalAttributes >> 16;
                if ((mode & 0xF000) == 0xA000) throw new InvalidDataException("Tool archive contains a symbolic link.");
                if (entry.FullName.EndsWith('/')) { Directory.CreateDirectory(path); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                await using (var input = entry.Open()) await using (var output = File.Create(path)) await input.CopyToAsync(output, token);
                if (!OperatingSystem.IsWindows() && (mode & 0x49) != 0)
                    File.SetUnixFileMode(path, (UnixFileMode)(mode & 0x1FF));
            }
        }
        else
        {
            await using var file = File.OpenRead(archive); await using var gzip = new GZipStream(file, CompressionMode.Decompress);
            await System.Formats.Tar.TarFile.ExtractToDirectoryAsync(gzip, directory, overwriteFiles: true, cancellationToken: token);
        }
    }
}
