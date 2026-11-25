using System.Security.Cryptography;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

/// <summary>The prebuilt runtime module is used only when it was built from the same sources with the same Emscripten
/// version, and only after its files match their checksums.</summary>
internal static class HostBundleChecks
{
    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-host-bundle-check-" + Guid.NewGuid().ToString("N"));
        try
        {
            var sources = Path.Combine(root, "sources");
            Directory.CreateDirectory(Path.Combine(sources, "recomp/runtime")); Directory.CreateDirectory(Path.Combine(sources, "recomp/port"));
            await File.WriteAllTextAsync(Path.Combine(sources, "recomp/runtime/rt.h"), "int rt_value;\r\nint rt_other;\r\n");
            await File.WriteAllTextAsync(Path.Combine(sources, "recomp/port/web.c"), "#include \"rt.h\"\r\n");
            await File.WriteAllTextAsync(Path.Combine(sources, "recomp/port/notes.txt"), "not a source");

            // A Linux checkout (LF) and a Windows checkout (CRLF) of the same sources are the same module.
            var unix = Path.Combine(root, "unix");
            foreach (var file in Directory.GetFiles(sources, "*", SearchOption.AllDirectories))
            {
                var copy = Path.Combine(unix, Path.GetRelativePath(sources, file));
                Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
                await File.WriteAllTextAsync(copy, (await File.ReadAllTextAsync(file)).Replace("\r\n", "\n"));
            }
            var fingerprint = WasmCompiler.HostFingerprint(sources, "4.0.19");
            Check(fingerprint == WasmCompiler.HostFingerprint(unix, "4.0.19"), "Line endings changed the runtime module fingerprint.");
            Check(fingerprint != WasmCompiler.HostFingerprint(sources, "4.0.20"), "The Emscripten version is not part of the runtime module fingerprint.");
            await File.WriteAllTextAsync(Path.Combine(unix, "recomp/port/notes.txt"), "changed, but not a source");
            Check(fingerprint == WasmCompiler.HostFingerprint(unix, "4.0.19"), "A non-source file changed the runtime module fingerprint.");

            var compiler = await FakeCompilerAsync(root, "4.0.19");
            Check(await WasmCompiler.EmscriptenVersionAsync(compiler, default) == "4.0.19", "The Emscripten version was not read from emcc --version.");
            Check(await WasmCompiler.EmscriptenVersionAsync(Path.Combine(root, "missing-emcc"), default) is null, "A missing compiler reported a version.");

            var bundle = Path.Combine(root, "bundle");
            Directory.CreateDirectory(bundle);
            var stamp = new JsonObject { ["fingerprint"] = fingerprint, ["emscripten"] = "4.0.19" };
            foreach (var file in WasmCompiler.HostFiles)
            {
                await File.WriteAllTextAsync(Path.Combine(bundle, file), "prebuilt " + file);
                stamp[file] = Convert.ToHexStringLower(SHA256.HashData(await File.ReadAllBytesAsync(Path.Combine(bundle, file))));
            }
            await File.WriteAllTextAsync(Path.Combine(bundle, HostBundle.StampFile), stamp.ToJsonString());

            await using var app = new LauncherCoordinator(sources, Path.Combine(root, "data"));
            async Task<(bool Used, string Output)> InstallAsync(string sourceRoot, string emcc)
            {
                var output = Path.Combine(root, "out-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(output);
                return (await HostBundle.FromDirectory(bundle).TryInstallAsync(sourceRoot, emcc, output, app, default), output);
            }

            var (used, output) = await InstallAsync(unix, compiler);
            Check(used, "A matching prebuilt runtime module was not used.");
            foreach (var file in WasmCompiler.HostFiles)
                Check(await File.ReadAllTextAsync(Path.Combine(output, file)) == "prebuilt " + file, $"{file} was not installed from the prebuilt runtime module.");
            Check(!Directory.EnumerateFiles(output, "*.part").Any(), "Installing the prebuilt runtime module left partial files.");

            Check(!(await InstallAsync(sources, await FakeCompilerAsync(Path.Combine(root, "newer"), "4.0.20"))).Used,
                "A runtime module built with another Emscripten version was used.");
            await File.AppendAllTextAsync(Path.Combine(unix, "recomp/runtime/rt.h"), "int rt_added;\n");
            Check(!(await InstallAsync(unix, compiler)).Used, "A runtime module built from other sources was used.");
            Check(!(await InstallAsync(sources, Path.Combine(root, "missing-emcc"))).Used, "The prebuilt runtime module was used without a working compiler.");
            Check(!await HostBundle.FromDirectory(Path.Combine(root, "no-bundle")).TryInstallAsync(sources, compiler, root, app, default),
                "A launcher without a prebuilt runtime module claimed to have one.");

            await File.WriteAllTextAsync(Path.Combine(bundle, "nostale.wasm"), "tampered");
            try
            {
                await InstallAsync(sources, compiler);
                throw new Exception("A prebuilt runtime module that fails its checksum was installed.");
            }
            catch (InvalidDataException) { }
            Console.WriteLine("Prebuilt runtime module checks passed.");
        }
        finally { try { Directory.Delete(root, recursive: true); } catch (IOException) { } }
    }

    /// <summary>An emcc stand-in that only answers --version, like the real one does.</summary>
    private static async Task<string> FakeCompilerAsync(string directory, string version)
    {
        Directory.CreateDirectory(directory);
        var line = $"emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) {version} (0000000000000000000000000000000000000000)";
        if (OperatingSystem.IsWindows())
        {
            var script = Path.Combine(directory, "emcc.cmd");
            await File.WriteAllTextAsync(script, $"@echo off\r\necho {line}\r\n");
            return script;
        }
        var shell = Path.Combine(directory, "emcc");
        await File.WriteAllTextAsync(shell, $"#!/bin/sh\necho '{line}'\n");
        File.SetUnixFileMode(shell, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        return shell;
    }

    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
}
