using System.Formats.Tar;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

internal static class PythonBootstrap
{
    public static async Task<string> EnsureAsync(string dataDirectory, Action<string> progress)
    {
        var tools = Path.Combine(dataDirectory, "tools");
        Directory.CreateDirectory(tools);
        var python = Path.Combine(tools, "python", OperatingSystem.IsWindows() ? "python.exe" : "bin/python3");
        if (File.Exists(python)) return python;
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(15) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("NosCore.ClientLab/0.1");
        var arch = RuntimeInformation.OSArchitecture == Architecture.Arm64 ? "aarch64" : "x86_64";
        var triple = arch + (OperatingSystem.IsWindows() ? "-pc-windows-msvc" : OperatingSystem.IsMacOS() ? "-apple-darwin" : "-unknown-linux-gnu");
        var release = JsonNode.Parse(await http.GetStringAsync("https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest"))!;
        var asset = release["assets"]!.AsArray().LastOrDefault(a =>
            a?["name"]?.GetValue<string>() is { } name && name.StartsWith("cpython-3.12.", StringComparison.Ordinal) &&
            name.EndsWith(triple + "-install_only_stripped.tar.gz", StringComparison.Ordinal))
            ?? throw new InvalidOperationException("No Python runtime is available for this architecture. Start with --python to use an existing interpreter.");
        var url = asset["browser_download_url"]!.GetValue<string>();
        if (!url.StartsWith("https://github.com/astral-sh/python-build-standalone/", StringComparison.Ordinal))
            throw new InvalidOperationException("Unexpected Python download source.");
        var archive = Path.Combine(tools, "python-download.tar.gz");
        progress("Downloading the private build runtime…");
        using (var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead))
        {
            response.EnsureSuccessStatusCode();
            await using var output = File.Create(archive);
            await response.Content.CopyToAsync(output);
        }
        var digest = asset["digest"]?.GetValue<string>();
        if (digest?.StartsWith("sha256:", StringComparison.Ordinal) == true)
        {
            await using var input = File.OpenRead(archive);
            var actual = Convert.ToHexStringLower(await SHA256.HashDataAsync(input));
            if (actual != digest[7..]) throw new InvalidOperationException("Python package checksum mismatch.");
        }
        progress("Preparing the private build runtime…");
        await using (var file = File.OpenRead(archive))
        await using (var gzip = new GZipStream(file, CompressionMode.Decompress))
            await TarFile.ExtractToDirectoryAsync(gzip, tools, overwriteFiles: false);
        if (!File.Exists(python)) throw new InvalidOperationException("The Python package did not contain the expected executable.");
        return python;
    }
}
