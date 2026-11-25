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
            await SysrootAndChoiceChecksAsync(root, sources, app);
            Console.WriteLine("Prebuilt runtime module checks passed.");
        }
        finally { try { Directory.Delete(root, recursive: true); } catch (IOException) { } }
    }

    /// <summary>The Emscripten headers that let the WebAssembly toolchain build without Emscripten, and how the compiler is chosen.</summary>
    private static async Task SysrootAndChoiceChecksAsync(string root, string sources, LauncherCoordinator app)
    {
        var bundle = Path.Combine(root, "sysroot-bundle");
        Directory.CreateDirectory(bundle);
        var headers = Path.Combine(root, "headers");
        Directory.CreateDirectory(Path.Combine(headers, "compat"));
        await File.WriteAllTextAsync(Path.Combine(headers, "compat", "stdio.h"), "/* sysroot header */");
        System.IO.Compression.ZipFile.CreateFromDirectory(headers, Path.Combine(bundle, HostBundle.SysrootFile));
        var stamp = new JsonObject
        {
            ["fingerprint"] = WasmCompiler.HostFingerprint(sources, "4.0.19"), ["emscripten"] = "4.0.19",
            [HostBundle.SysrootFile] = Convert.ToHexStringLower(SHA256.HashData(await File.ReadAllBytesAsync(Path.Combine(bundle, HostBundle.SysrootFile)))),
        };
        await File.WriteAllTextAsync(Path.Combine(bundle, HostBundle.StampFile), stamp.ToJsonString());
        var host = HostBundle.FromDirectory(bundle);
        var read = await host.ReadStampAsync(default) ?? throw new Exception("A valid runtime module stamp was not read.");
        Check(read.HasSysroot, "The stamp's Emscripten headers were not recognised.");
        var tools = Path.Combine(root, "tools");
        var sysroot = await host.ExtractSysrootAsync(read, tools, default);
        Check(await File.ReadAllTextAsync(Path.Combine(sysroot, "include", "compat", "stdio.h")) == "/* sysroot header */", "The Emscripten headers were not unpacked under include/.");
        File.Delete(Path.Combine(sysroot, "include", "compat", "stdio.h"));
        Check(await host.ExtractSysrootAsync(read, tools, default) == sysroot && !File.Exists(Path.Combine(sysroot, "include", "compat", "stdio.h")),
            "Unpacked Emscripten headers were unpacked again instead of reused.");
        await File.WriteAllTextAsync(Path.Combine(bundle, HostBundle.SysrootFile), "tampered");
        try
        {
            await host.ExtractSysrootAsync(read, Path.Combine(root, "tools-2"), default);
            throw new Exception("Emscripten headers that fail their checksum were unpacked.");
        }
        catch (InvalidDataException) { }
        Check(!Directory.EnumerateDirectories(Path.Combine(root, "tools-2")).Any(), "A rejected header archive left a partial sysroot.");

        // Choosing the compiler.
        Check(LauncherCoordinator.CompilerSetting("wasm") == CompilerChoice.WebAssembly && LauncherCoordinator.CompilerSetting(" EMCC ") == CompilerChoice.Emscripten &&
              LauncherCoordinator.CompilerSetting(null) == CompilerChoice.Auto && LauncherCoordinator.CompilerSetting("other") == CompilerChoice.Auto,
            "The compiler setting was not understood.");
        var generated = Path.Combine(root, "generated");
        Directory.CreateDirectory(generated);
        await File.WriteAllTextAsync(Path.Combine(generated, "fn_00401000.c"), "int x;");
        var noBundle = HostBundle.FromDirectory(Path.Combine(root, "no-bundle"));
        try
        {
            await WasmCompiler.BuildAsync(sources, generated, Path.Combine(root, "out-wasm"), CompilerChoice.WebAssembly,
                _ => throw new Exception("Emscripten was set up although the WebAssembly compiler was chosen."), app, default, noBundle);
            throw new Exception("The WebAssembly compiler ran without a prebuilt runtime module.");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("runtime module of a release build")) { }
        try
        {
            await WasmCompiler.BuildAsync(sources, generated, Path.Combine(root, "out-auto"), CompilerChoice.Auto,
                _ => throw new HttpRequestException("emsdk download failed"), app, default, noBundle);
            throw new Exception("A failed Emscripten setup was hidden although nothing could replace it.");
        }
        catch (HttpRequestException) { }

        // The compiler package's resource archive cannot write outside its folder.
        using var tar = new MemoryStream();
        await using (var writer = new System.Formats.Tar.TarWriter(tar, leaveOpen: true))
        {
            var entry = new System.Formats.Tar.PaxTarEntry(System.Formats.Tar.TarEntryType.RegularFile, "../escaped.h") { DataStream = new MemoryStream([1]) };
            await writer.WriteEntryAsync(entry);
        }
        tar.Position = 0;
        try
        {
            await WasiToolchain.ExtractTarAsync(tar, Path.Combine(root, "usr"), default);
            throw new Exception("A compiler archive entry escaped its folder.");
        }
        catch (InvalidDataException) { }
        Check(!File.Exists(Path.Combine(root, "escaped.h")), "A compiler archive entry was written outside its folder.");
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
