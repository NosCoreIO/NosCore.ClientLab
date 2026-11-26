using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>Which toolchain compiles the game module.</summary>
public enum CompilerChoice
{
    /// <summary>Emscripten when it is installed or installs; the WebAssembly toolchain when it cannot be set up.</summary>
    Auto,
    Emscripten,
    /// <summary>clang and wasm-ld as WebAssembly (<see cref="WasiToolchain"/>): no emsdk or Python, about 3x slower to compile.</summary>
    WebAssembly,
}

public static class WasmCompiler
{
    // The game is a side module (nostale-game.wasm) and the runtime + portable host a main module (nostale.js/.wasm)
    // that finds the game's label table by name (RT_HOST_MODULE), so both halves are position-independent. The host
    // module does not depend on the client at all: releases ship it prebuilt (HostBundle), and a local build relinks it
    // only when its own code changes.
    private static readonly string[] GameCompileFlags = ["-O2", "-w", "-c", "-fPIC"];
    private static readonly string[] EmccOnlyFlags = ["-sSUPPORT_LONGJMP=0"];
    // SIMD applies to portable host math/state copies.
    private static readonly string[] HostCompileFlags = ["-O3", "-msimd128", "-w", "-c", "-fPIC", "-DRT_HOST_MODULE", "-sSUPPORT_LONGJMP=0"];
    private static readonly string[] GameLinkFlags = ["-O2", "-sSIDE_MODULE=1", "-sSUPPORT_LONGJMP=0"];
    private static readonly string[] HostLinkFlags = ["-O3", "-msimd128", "-sMAIN_MODULE=1", "-sGLOBAL_BASE=536870912", "-sINITIAL_MEMORY=1024MB", "-sALLOW_MEMORY_GROWTH=1",
        "-sMAXIMUM_MEMORY=2048MB", "-sSTACK_SIZE=64MB", "-sEXIT_RUNTIME=1", "-sSUPPORT_LONGJMP=0", "-sENVIRONMENT=worker", "-sFORCE_FILESYSTEM=1", "-sINVOKE_RUN=0",
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV", "--emit-symbol-map"];
    // What emcc adds when it runs clang (`emcc --cflags`), with the sysroot mounted at /sysroot.
    private static readonly string[] WasiClangFlags = ["-target", "wasm32-unknown-emscripten", "-fignore-exceptions", "-fvisibility=default",
        "-mllvm", "-combiner-global-alias-analysis=false", "-mllvm", "-disable-lsr", "--sysroot=/sysroot", "-DEMSCRIPTEN",
        "-Xclang", "-iwithsysroot/include/fakesdl", "-Xclang", "-iwithsysroot/include/compat"];
    // emcc's own wasm-ld line for -sSIDE_MODULE=1. emcc then runs Binaryen's wasm-opt -O2, which has no WebAssembly build
    // here; on the boot scenes the module ran the same without it (docs/WASM-BACKEND.md).
    private static readonly string[] WasiSideLinkFlags = ["-mllvm", "-combiner-global-alias-analysis=false", "-mllvm", "-disable-lsr", "--import-memory",
        "--strip-debug", "--export-dynamic", "--export=__wasm_call_ctors", "--experimental-pic", "--unresolved-symbols=import-dynamic", "-shared"];
    public static readonly string[] HostFiles = ["nostale.js", "nostale.wasm", "nostale.js.symbols"];

    /// <summary>Builds the game module for <paramref name="generated"/> and places the runtime module beside it: the
    /// release's prebuilt one when it was built from these exact sources, otherwise a local one (which needs Emscripten).
    /// <paramref name="emscripten"/> returns emcc's path, installing it if needed; it is called only when Emscripten is used.
    /// <see cref="CompilerChoice.Auto"/> uses the WebAssembly compiler when the prebuilt runtime module fits, unless
    /// Emscripten is already installed (<paramref name="emscriptenInstalled"/>, about 3x faster); either one falls back to the other.</summary>
    public static async Task BuildAsync(string root, string generated, string output, CompilerChoice choice, Func<CancellationToken, Task<string>> emscripten,
        LauncherCoordinator app, CancellationToken token, HostBundle? bundle = null, bool emscriptenInstalled = false)
    {
        Directory.CreateDirectory(output);
        bundle ??= HostBundle.Embedded;
        var stamp = await bundle.ReadStampAsync(token);
        string? unusable = stamp is null ? "this launcher has no prebuilt runtime module"
            : !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("EMCC_CFLAGS")) ? "EMCC_CFLAGS is set"
            : stamp.Fingerprint != HostFingerprint(root, stamp.Emscripten) ? "the runtime sources differ from " + bundle.Description
            : !stamp.HasSysroot ? bundle.Description + " has no Emscripten headers"
            : null;
        var gameSources = Directory.GetFiles(generated, "*.c");
        if (choice == CompilerChoice.WebAssembly && unusable is not null)
            throw new InvalidOperationException($"The WebAssembly compiler needs the runtime module of a release build, but {unusable}.");
        if (choice == CompilerChoice.WebAssembly || choice == CompilerChoice.Auto && unusable is null && !emscriptenInstalled)
        {
            try
            {
                await BuildWithWasiAsync(root, gameSources, output, bundle, stamp!, app, token);
                return;
            }
            catch (Exception error) when (choice == CompilerChoice.Auto && error is not OperationCanceledException)
            {
                app.Log($"The WebAssembly compiler failed ({error.Message}); using Emscripten instead.");
            }
        }
        try { await BuildWithEmscriptenAsync(root, gameSources, output, await emscripten(token), bundle, stamp, app, token); }
        // An installed Emscripten that no longer works (a half-deleted emsdk, a missing Python) falls back too.
        catch (Exception error) when (choice == CompilerChoice.Auto && unusable is null && emscriptenInstalled && error is not OperationCanceledException)
        {
            app.Log($"Emscripten failed ({error.Message}); using the WebAssembly compiler instead.");
            await BuildWithWasiAsync(root, gameSources, output, bundle, stamp!, app, token);
        }
    }

    private static async Task BuildWithEmscriptenAsync(string root, string[] gameSources, string output, string compiler, HostBundle bundle, HostBundle.Stamp? stamp,
        LauncherCoordinator app, CancellationToken token)
    {
        var prebuilt = stamp is not null && await bundle.TryInstallAsync(stamp, root, compiler, output, app, token);
        var hostSources = prebuilt ? [] : HostSources(root);
        var objects = await CompileAsync(root, gameSources, hostSources, await EmccIdentityAsync(compiler, token),
            (source, obj, game, ct) => app.RunAsync(compiler, (game ? GameCompileFlags.Concat(EmccOnlyFlags) : HostCompileFlags)
                .Concat(["-I", Path.Combine(root, "recomp/runtime"), "-I", Path.Combine(root, "recomp/port"), source, "-o", obj]), ct), app, token);
        app.Status("compiling", "Finishing game preparation…", .9);
        await Task.WhenAll(
            LinkAsync(compiler, GameLinkFlags, gameSources.Order(StringComparer.Ordinal).Select(source => objects[source]), Path.Combine(output, "nostale-game.wasm"), app, token),
            prebuilt ? Task.CompletedTask : HostModuleAsync(compiler, hostSources.Order(StringComparer.Ordinal).Select(source => objects[source]).ToArray(), output, app, token));
        VerifyOutput(output);
    }

    private static async Task BuildWithWasiAsync(string root, string[] gameSources, string output, HostBundle bundle, HostBundle.Stamp stamp, LauncherCoordinator app, CancellationToken token)
    {
        await bundle.InstallAsync(stamp, output, token);
        var sysroot = await bundle.ExtractSysrootAsync(stamp, Path.Combine(app.DataDirectory, "tools"), token);
        using var wasi = await WasiToolchain.PrepareAsync(app.DataDirectory, app, token);
        app.Log($"Compiling with the WebAssembly compiler ({WasiToolchain.Identity}) and {bundle.Description} (Emscripten {stamp.Emscripten}).");
        WasiToolchain.Mount[] includes = [new(sysroot, "/sysroot"), new(Path.Combine(root, "recomp/runtime"), "/inc/runtime"), new(Path.Combine(root, "recomp/port"), "/inc/port")];
        var objects = await CompileAsync(root, gameSources, [], "wasi:" + WasiToolchain.Identity + "\0" + stamp.Sysroot + "\0" + string.Join(' ', WasiClangFlags), (source, obj, _, ct) =>
            wasi.RunAsync("clang", [.. WasiClangFlags, .. GameCompileFlags, "-I/inc/runtime", "-I/inc/port", "/src/" + Path.GetFileName(source), "-o", "/out/" + Path.GetFileName(obj)],
                [.. includes, new(Path.GetDirectoryName(source)!, "/src"), new(Path.GetDirectoryName(obj)!, "/out", Writable: true)], app, ct), app, token);
        app.Status("compiling", "Finishing game preparation…", .9);
        // Objects live in the content-addressed cache: mount each directory once and list them in a response file.
        var ordered = gameSources.Order(StringComparer.Ordinal).Select(source => objects[source]).ToArray();
        var directories = ordered.Select(Path.GetDirectoryName).Distinct(StringComparer.Ordinal).Select((path, index) => (Host: path!, Guest: "/obj" + index)).ToArray();
        var guest = directories.ToDictionary(item => item.Host, item => item.Guest, StringComparer.Ordinal);
        var responseFile = Path.Combine(output, "nostale-game.rsp");
        await File.WriteAllLinesAsync(responseFile, ordered.Select(obj => guest[Path.GetDirectoryName(obj)!] + "/" + Path.GetFileName(obj)), token);
        try
        {
            await wasi.RunAsync("wasm-ld", ["-o", "/out/nostale-game.wasm", "--whole-archive", "@/out/nostale-game.rsp", "--no-whole-archive", .. WasiSideLinkFlags],
                [.. directories.Select(item => new WasiToolchain.Mount(item.Host, item.Guest)), new(output, "/out", Writable: true)], app, token);
        }
        finally { File.Delete(responseFile); }
        VerifyOutput(output);
    }

    private static void VerifyOutput(string output)
    {
        foreach (var file in new[] { "nostale.js", "nostale.wasm", "nostale-game.wasm" })
            if (!File.Exists(Path.Combine(output, file)) || new FileInfo(Path.Combine(output, file)).Length == 0) throw new InvalidDataException("The compiler did not produce a complete runtime.");
    }

    /// <summary>Builds only the runtime module, plus the <c>host.json</c> stamp that lets a launcher with the same sources
    /// use it instead of compiling its own (<c>ClientLab --build-host</c>, run by the release workflow). Emscripten's
    /// sysroot headers ride along, so a launcher can compile the game module without Emscripten.</summary>
    public static async Task BuildHostAsync(string root, string output, string compiler, LauncherCoordinator app, CancellationToken token)
    {
        var version = await EmscriptenVersionAsync(compiler, token)
            ?? throw new InvalidOperationException($"Could not read the Emscripten version of {compiler}.");
        if (version != ToolchainInstaller.EmsdkVersion)
            throw new InvalidOperationException($"The runtime module must be built with Emscripten {ToolchainInstaller.EmsdkVersion}, the version launchers install; {compiler} is {version}.");
        Directory.CreateDirectory(output);
        var hostSources = HostSources(root);
        var objects = await CompileAsync(root, [], hostSources, await EmccIdentityAsync(compiler, token), (source, obj, _, ct) => app.RunAsync(compiler,
            HostCompileFlags.Concat(["-I", Path.Combine(root, "recomp/runtime"), "-I", Path.Combine(root, "recomp/port"), source, "-o", obj]), ct), app, token);
        await HostModuleAsync(compiler, hostSources.Order(StringComparer.Ordinal).Select(source => objects[source]).ToArray(), output, app, token);
        // emcc installs its sysroot headers on first use; this compile made sure they exist.
        var sysroot = await SysrootAsync(compiler, token) ?? throw new InvalidOperationException("Could not find Emscripten's sysroot.");
        var archive = Path.Combine(output, HostBundle.SysrootFile);
        if (File.Exists(archive)) File.Delete(archive);
        ZipFile.CreateFromDirectory(Path.Combine(sysroot, "include"), archive, CompressionLevel.Optimal, includeBaseDirectory: false);
        var stamp = new JsonObject { ["fingerprint"] = HostFingerprint(root, version), ["emscripten"] = version };
        foreach (var file in HostFiles.Append(HostBundle.SysrootFile))
            stamp[file] = Convert.ToHexStringLower(SHA256.HashData(await File.ReadAllBytesAsync(Path.Combine(output, file), token)));
        await File.WriteAllTextAsync(Path.Combine(output, HostBundle.StampFile), stamp.ToJsonString(new() { WriteIndented = true }), token);
    }

    private static string[] HostSources(string root) =>
        [Path.Combine(root, "recomp/runtime/rt.c"), .. Directory.GetFiles(Path.Combine(root, "recomp/port"), "*.c")];

    /// <summary>Identifies a runtime module by everything it is built from: the runtime and portable host sources (line
    /// endings normalised, so a Windows checkout matches a Linux build), the flags and the Emscripten version.</summary>
    public static string HostFingerprint(string root, string emscriptenVersion)
    {
        using var fingerprint = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        fingerprint.AppendData(Encoding.UTF8.GetBytes("host-module-v1\0" + emscriptenVersion + "\0" + string.Join(' ', HostCompileFlags) + "\0" + string.Join(' ', HostLinkFlags) + "\0"));
        foreach (var directory in new[] { "recomp/runtime", "recomp/port" })
            foreach (var file in Directory.GetFiles(Path.Combine(root, directory)).Where(path => Path.GetExtension(path) is ".c" or ".h" or ".inc")
                         .Order(StringComparer.Ordinal))
            {
                fingerprint.AppendData(Encoding.UTF8.GetBytes(directory + "/" + Path.GetFileName(file) + "\0"));
                fingerprint.AppendData(File.ReadAllBytes(file).Where(value => value != (byte)'\r').ToArray());
                fingerprint.AppendData([0]);
            }
        return Convert.ToHexStringLower(fingerprint.GetHashAndReset());
    }

    /// <summary>The version emcc reports, or null when it cannot be run.</summary>
    public static async Task<string?> EmscriptenVersionAsync(string compiler, CancellationToken token)
    {
        var match = Regex.Match(await EmccOutputAsync(compiler, "--version", token) ?? "", @"emcc \(Emscripten[^)]*\) (\d+\.\d+\.\d+)");
        return match.Success ? match.Groups[1].Value : null;
    }

    private static async Task<string?> SysrootAsync(string compiler, CancellationToken token)
    {
        // emcc quotes the whole argument when the path needs it: '--sysroot=C:\...\sysroot'.
        var match = Regex.Match(await EmccOutputAsync(compiler, "--cflags", token) ?? "", @"'--sysroot=([^']+)'|""--sysroot=([^""]+)""|--sysroot=(\S+)");
        if (!match.Success) return null;
        var path = new[] { match.Groups[1], match.Groups[2], match.Groups[3] }.First(group => group.Success).Value;
        return Directory.Exists(Path.Combine(path, "include")) ? path : null;
    }

    private static async Task<string?> EmccOutputAsync(string compiler, string argument, CancellationToken token)
    {
        try
        {
            var info = new ProcessStartInfo(compiler, argument) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            using var child = Process.Start(info);
            if (child is null) return null;
            var stdout = child.StandardOutput.ReadToEndAsync(token); var stderr = child.StandardError.ReadToEndAsync(token);
            await child.WaitForExitAsync(token);
            var text = await stdout + "\n" + await stderr;
            return child.ExitCode == 0 ? text : null;
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { return null; }
    }

    private static async Task<string> EmccIdentityAsync(string compiler, CancellationToken token)
    {
        await using var input = File.OpenRead(compiler);
        return "emcc:" + Path.GetFullPath(compiler) + "\0" + Convert.ToHexStringLower(await SHA256.HashDataAsync(input, token)) + "\0" +
            System.Runtime.InteropServices.RuntimeInformation.RuntimeIdentifier + "\0" + Environment.GetEnvironmentVariable("EMCC_CFLAGS");
    }

    private delegate Task CompileOne(string source, string obj, bool game, CancellationToken token);

    private static async Task<ConcurrentDictionary<string, string>> CompileAsync(string root, string[] gameSources, string[] hostSources, string compilerIdentity,
        CompileOne compile, LauncherCoordinator app, CancellationToken token)
    {
        var runtime = Path.Combine(root, "recomp/runtime"); var port = Path.Combine(root, "recomp/port");
        var game = gameSources.ToHashSet(StringComparer.Ordinal);
        var sources = gameSources.Concat(hostSources).OrderByDescending(path => new FileInfo(path).Length).ThenBy(path => path, StringComparer.Ordinal).ToArray();
        static string[] Headers(string directory) => Directory.GetFiles(directory).Where(path => Path.GetExtension(path) is ".h" or ".inc").ToArray();
        string[] runtimeHeaders = Headers(runtime), portHeaders = Headers(port);
        async Task<string> IdentityAsync(IEnumerable<string> dependencies, string[] compilerOptions)
        {
            using var fingerprint = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            fingerprint.AppendData(Encoding.UTF8.GetBytes("wasm-objects-v2\0" + string.Join(' ', compilerOptions) + "\0" + compilerIdentity));
            foreach (var dependency in dependencies.Order(StringComparer.Ordinal))
            {
                fingerprint.AppendData(Encoding.UTF8.GetBytes(Path.GetFileName(dependency) + "\0"));
                await using var input = File.OpenRead(dependency);
                fingerprint.AppendData(await SHA256.HashDataAsync(input, token));
            }
            return Convert.ToHexStringLower(fingerprint.GetHashAndReset());
        }
        // Generated code includes only rt.h, so its objects (nearly all of the compile time) survive changes to the
        // portable host's headers, such as the renderer; host objects depend on both sets of headers.
        var cacheRoot = Path.Combine(app.DataDirectory, "cache", "wasm-objects");
        var gameCache = new CompilationCache(cacheRoot, await IdentityAsync(runtimeHeaders, GameCompileFlags));
        var hostCache = new CompilationCache(cacheRoot, await IdentityAsync(portHeaders.Concat(runtimeHeaders), HostCompileFlags));
        var memory = GC.GetGCMemoryInfo();
        var budget = Math.Max(0, memory.TotalAvailableMemoryBytes - memory.MemoryLoadBytes);
        var workers = Math.Clamp(Math.Min(Environment.ProcessorCount - 2, (int)(budget / (3L * 1024 * 1024 * 1024))), 1, 8);
        var objects = new ConcurrentDictionary<string, string>(StringComparer.Ordinal);
        var completed = 0; var reused = 0;
        var elapsed = Stopwatch.StartNew();
        using var heartbeat = new CancellationTokenSource();
        var progress = Task.Run(async () =>
        {
            using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
            try
            {
                while (await timer.WaitForNextTickAsync(heartbeat.Token))
                    app.Status("compiling", $"Preparing game: {Volatile.Read(ref completed)}/{sources.Length} files · {(int)elapsed.Elapsed.TotalMinutes}:{elapsed.Elapsed.Seconds:00}", .9 * Volatile.Read(ref completed) / sources.Length);
            }
            catch (OperationCanceledException) { }
        });
        app.Log($"Compiling {sources.Length} files with {workers} workers; completed objects are cached across build attempts.");
        try
        {
            await Parallel.ForEachAsync(sources, new ParallelOptions { MaxDegreeOfParallelism = workers, CancellationToken = token }, async (source, cancellation) =>
            {
                var watch = Stopwatch.StartNew();
                var gameSource = game.Contains(source);
                var result = await (gameSource ? gameCache : hostCache).GetOrCompileAsync(source, (obj, ct) => compile(source, obj, gameSource, ct), cancellation);
                objects[source] = result.Path;
                if (result.Reused) Interlocked.Increment(ref reused);
                app.Log($"{(result.Reused ? "Reused" : "Compiled")} {Path.GetFileName(source)} ({watch.Elapsed.TotalSeconds:F1}s)");
                Interlocked.Increment(ref completed);
            });
        }
        finally { heartbeat.Cancel(); await progress; }
        app.Log($"Reused {reused} cached files; compiled {sources.Length - reused} files in {elapsed.Elapsed.TotalSeconds:F0}s.");
        return objects;
    }

    /// <summary>The runtime main module, linked once per distinct host build: its objects are content-addressed, so their
    /// names identify it. A client patch reuses it as is.</summary>
    private static async Task HostModuleAsync(string compiler, string[] hostObjects, string output, LauncherCoordinator app, CancellationToken token)
    {
        var key = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(string.Join('\n', HostLinkFlags.Concat(hostObjects.Select(ObjectIdentity)).Prepend(Path.GetFullPath(compiler))))))[..24];
        var cached = Path.Combine(app.DataDirectory, "cache", "host-module", key);
        if (!File.Exists(Path.Combine(cached, "nostale.wasm")))
        {
            var staging = cached + "." + Guid.NewGuid().ToString("N");
            Directory.CreateDirectory(staging);
            try
            {
                await LinkAsync(compiler, HostLinkFlags, hostObjects, Path.Combine(staging, "nostale.js"), app, token);
                try { Directory.Move(staging, cached); }
                catch (IOException) when (File.Exists(Path.Combine(cached, "nostale.wasm"))) { } // Another build linked it first.
            }
            finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
        }
        else app.Log("Reused the runtime module; only the game module was linked.");
        foreach (var file in HostFiles.Where(file => File.Exists(Path.Combine(cached, file))))
            File.Copy(Path.Combine(cached, file), Path.Combine(output, file), overwrite: true);
    }

    /// <summary>An object's cache directory (its headers, flags and compiler) and file name (its source).</summary>
    private static string ObjectIdentity(string obj) => Path.GetFileName(Path.GetDirectoryName(obj)) + "/" + Path.GetFileName(obj);

    private static async Task LinkAsync(string compiler, IEnumerable<string> flags, IEnumerable<string> objects, string target, LauncherCoordinator app, CancellationToken token)
    {
        var responseFile = target + ".rsp";
        await File.WriteAllLinesAsync(responseFile, flags.Concat(objects).Concat(["-o", target]).Select(value => "\"" + value.Replace('\\', '/').Replace("\"", "\\\"") + "\""), token);
        try { await app.RunAsync(compiler, ["@" + responseFile], token); }
        finally { File.Delete(responseFile); }
    }
}

/// <summary>A runtime module built ahead of time (by the release workflow) with its <c>host.json</c> stamp, and the
/// Emscripten sysroot headers it was built against.</summary>
public sealed class HostBundle(Func<string, Stream?> open, string description)
{
    public const string StampFile = "host.json";
    public const string SysrootFile = "sysroot-include.zip";
    private const string ResourcePrefix = "Prebuilt/host/";

    public sealed record Stamp(string Fingerprint, string Emscripten, JsonNode Json)
    {
        public bool HasSysroot => Json[SysrootFile] is not null;
        public string Sysroot => Json[SysrootFile]?.GetValue<string>() ?? "";
    }

    public string Description => description;

    /// <summary>The module embedded in this launcher build; empty in local developer builds.</summary>
    public static HostBundle Embedded { get; } = new(name => Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourcePrefix + name), "the launcher's bundled runtime module");

    public static HostBundle FromDirectory(string directory) =>
        new(name => File.Exists(Path.Combine(directory, name)) ? File.OpenRead(Path.Combine(directory, name)) : null, directory);

    public async Task<Stamp?> ReadStampAsync(CancellationToken token)
    {
        await using var stream = open(StampFile);
        if (stream is null) return null;
        var json = await JsonNode.ParseAsync(stream, cancellationToken: token);
        return json?["fingerprint"]?.GetValue<string>() is { } fingerprint && json["emscripten"]?.GetValue<string>() is { } version
            ? new Stamp(fingerprint, version, json) : null;
    }

    /// <summary>Copies the module into <paramref name="output"/> when it matches these sources and this emcc, and says why not otherwise.</summary>
    public async Task<bool> TryInstallAsync(string root, string compiler, string output, LauncherCoordinator app, CancellationToken token) =>
        await ReadStampAsync(token) is { } stamp && await TryInstallAsync(stamp, root, compiler, output, app, token);

    public async Task<bool> TryInstallAsync(Stamp stamp, string root, string compiler, string output, LauncherCoordinator app, CancellationToken token)
    {
        if (!string.IsNullOrEmpty(Environment.GetEnvironmentVariable("EMCC_CFLAGS")))
        {
            app.Log("EMCC_CFLAGS is set, so the runtime module is built locally instead of using " + description + ".");
            return false;
        }
        var version = await WasmCompiler.EmscriptenVersionAsync(compiler, token);
        if (version is null || version != stamp.Emscripten)
        {
            app.Log($"The compiler is Emscripten {version ?? "(unknown)"}, but {description} needs {stamp.Emscripten}; building the runtime module locally.");
            return false;
        }
        if (stamp.Fingerprint != WasmCompiler.HostFingerprint(root, version))
        {
            app.Log($"The runtime sources differ from {description}; building the runtime module locally.");
            return false;
        }
        await InstallAsync(stamp, output, token);
        app.Log($"Using {description} (Emscripten {version}); only the game module is compiled here.");
        return true;
    }

    /// <summary>Copies the runtime module's files, each checked against the stamp.</summary>
    public async Task InstallAsync(Stamp stamp, string output, CancellationToken token)
    {
        foreach (var file in WasmCompiler.HostFiles)
            await CopyVerifiedAsync(stamp, file, Path.Combine(output, file), token);
    }

    /// <summary>Unpacks the sysroot headers once under <paramref name="tools"/> and returns the sysroot directory.</summary>
    public async Task<string> ExtractSysrootAsync(Stamp stamp, string tools, CancellationToken token)
    {
        var sysroot = Path.Combine(tools, $"emscripten-sysroot-{stamp.Emscripten}-{stamp.Sysroot[..16]}");
        if (File.Exists(Path.Combine(sysroot, ".complete"))) return sysroot;
        var staging = sysroot + "." + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        try
        {
            var archive = Path.Combine(staging, SysrootFile);
            await CopyVerifiedAsync(stamp, SysrootFile, archive, token);
            await ZipFile.ExtractToDirectoryAsync(archive, Path.Combine(staging, "include"), token);
            File.Delete(archive);
            await File.WriteAllTextAsync(Path.Combine(staging, ".complete"), stamp.Sysroot, token);
            if (Directory.Exists(sysroot)) Directory.Delete(sysroot, recursive: true);
            Directory.Move(staging, sysroot);
        }
        finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
        return sysroot;
    }

    private async Task CopyVerifiedAsync(Stamp stamp, string file, string target, CancellationToken token)
    {
        await using var input = open(file) ?? throw new InvalidDataException($"{description} is missing {file}.");
        string actual;
        await using (var copy = File.Create(target + ".part"))
        using (var hasher = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
        {
            var buffer = new byte[1 << 20];
            int length;
            while ((length = await input.ReadAsync(buffer, token)) > 0)
            {
                hasher.AppendData(buffer, 0, length);
                await copy.WriteAsync(buffer.AsMemory(0, length), token);
            }
            actual = Convert.ToHexStringLower(hasher.GetHashAndReset());
        }
        if (actual != stamp.Json[file]?.GetValue<string>())
        {
            File.Delete(target + ".part");
            throw new InvalidDataException($"{file} in {description} does not match its checksum.");
        }
        File.Move(target + ".part", target, overwrite: true);
    }
}
