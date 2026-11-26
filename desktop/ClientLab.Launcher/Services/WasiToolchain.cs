using System.Formats.Tar;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Wasmtime;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// clang and wasm-ld built as WebAssembly (YoWASP <c>@yowasp/clang</c>, a WASI command that picks the tool from
/// <c>argv[0]</c>), run in-process by Wasmtime. It needs no emsdk, Python or native compiler: the launcher downloads one
/// pinned, hash-checked package and compiles it to native code once (cached). Each tool run gets its own store, so
/// several compile in parallel; only the directories a run needs are visible to it.
/// </summary>
public sealed class WasiToolchain : IDisposable
{
    public const string PackageVersion = "22.0.0-git20695-13";
    private const string PackageUrl = "https://registry.npmjs.org/@yowasp/clang/-/clang-" + PackageVersion + ".tgz";
    private const string PackageIntegrity = "sha512-nNIUdoQKrttdSEvO7gpkgczB1fhRtiWEqxYNJHloD9EBf0HBu1bvcYnkTYTmUacXDItRGTFWnogWKnfSe+04Cg==";
    private const long PackageSize = 104_406_128; // unpacked; used only for progress

    private readonly Engine engine;
    private readonly Module module;
    private readonly string usr;

    private WasiToolchain(Engine engine, Module module, string usr) { this.engine = engine; this.module = module; this.usr = usr; }

    /// <summary>Identifies the compiler for object caches: the package and the Wasmtime build that runs it.</summary>
    public static string Identity => $"yowasp-clang-{PackageVersion}/wasmtime-{typeof(Engine).Assembly.GetName().Version}";

    public static async Task<WasiToolchain> PrepareAsync(string dataDirectory, LauncherCoordinator app, CancellationToken token)
    {
        var directory = Path.Combine(dataDirectory, "tools", "yowasp-clang-" + PackageVersion);
        var wasm = Path.Combine(directory, "llvm.core.wasm"); var usr = Path.Combine(directory, "usr");
        if (!File.Exists(Path.Combine(directory, ".complete")))
            await InstallAsync(directory, app, token);
        var config = new Config().WithOptimizationLevel(OptimizationLevel.Speed).WithEpochInterruption(true);
        var engine = new Engine(config);
        try
        {
            var cache = Path.Combine(dataDirectory, "cache", "wasmtime");
            var key = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(
                Identity + "\0" + RuntimeInformation.RuntimeIdentifier + "\0" + new FileInfo(wasm).Length)))[..24];
            var compiled = Path.Combine(cache, key + ".cwasm");
            Module? module = null;
            if (File.Exists(compiled))
            {
                try { module = Module.DeserializeFile(engine, "llvm", compiled); }
                catch (WasmtimeException error) { app.Log("Recompiling the cached WebAssembly compiler: " + error.Message); }
            }
            if (module is null)
            {
                app.Status("tools", "Preparing the WebAssembly compiler for this computer (once)…");
                module = await Task.Run(() => Module.FromFile(engine, wasm), token);
                Directory.CreateDirectory(cache);
                var temporary = compiled + "." + Guid.NewGuid().ToString("N");
                try
                {
                    await File.WriteAllBytesAsync(temporary, module.Serialize(), token);
                    File.Move(temporary, compiled, overwrite: true);
                }
                catch { module.Dispose(); throw; }
                finally { File.Delete(temporary); }
            }
            return new WasiToolchain(engine, module, usr);
        }
        catch { engine.Dispose(); throw; }
    }

    private static async Task InstallAsync(string directory, LauncherCoordinator app, CancellationToken token)
    {
        if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true);
        var staging = directory + "." + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        try
        {
            var archive = Path.Combine(staging, "package.tgz");
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(20) })
            {
                http.DefaultRequestHeaders.UserAgent.ParseAdd("NosCore.ClientLab/0.2");
                using var response = await http.GetAsync(PackageUrl, HttpCompletionOption.ResponseHeadersRead, token);
                response.EnsureSuccessStatusCode();
                var size = response.Content.Headers.ContentLength ?? 0;
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA512);
                await using (var input = await response.Content.ReadAsStreamAsync(token))
                await using (var output = File.Create(archive))
                {
                    var buffer = new byte[1 << 20]; long done = 0; int length;
                    while ((length = await input.ReadAsync(buffer, token)) > 0)
                    {
                        hash.AppendData(buffer, 0, length);
                        await output.WriteAsync(buffer.AsMemory(0, length), token);
                        done += length;
                        if (size > 0) app.Status("tools", $"Downloading the WebAssembly compiler · {done >> 20} of {size >> 20} MB", done / (double)size);
                    }
                }
                if ("sha512-" + Convert.ToBase64String(hash.GetHashAndReset()) != PackageIntegrity)
                    throw new InvalidDataException("The downloaded WebAssembly compiler does not match its published checksum.");
            }
            app.Status("tools", "Unpacking the WebAssembly compiler…");
            await using (var file = File.OpenRead(archive))
            await using (var gzip = new GZipStream(file, CompressionMode.Decompress))
            {
                var reader = new TarReader(gzip);
                while (await reader.GetNextEntryAsync(copyData: false, token) is { } entry)
                {
                    if (entry.DataStream is null) continue;
                    if (entry.Name == "package/gen/llvm.core.wasm")
                    {
                        await using var output = File.Create(Path.Combine(staging, "llvm.core.wasm"));
                        await entry.DataStream.CopyToAsync(output, token);
                    }
                    else if (entry.Name == "package/gen/llvm-resources.tar")
                        await ExtractTarAsync(entry.DataStream, Path.Combine(staging, "usr"), token);
                    else if (entry.Name is "package/LICENSE" or "package/LICENSE.txt" or "package/package.json")
                    {
                        await using var output = File.Create(Path.Combine(staging, Path.GetFileName(entry.Name)));
                        await entry.DataStream.CopyToAsync(output, token);
                    }
                }
            }
            File.Delete(archive);
            if (!File.Exists(Path.Combine(staging, "llvm.core.wasm")) || !Directory.Exists(Path.Combine(staging, "usr")))
                throw new InvalidDataException("The WebAssembly compiler package is incomplete.");
            await File.WriteAllTextAsync(Path.Combine(staging, ".complete"), PackageVersion, token);
            Directory.Move(staging, directory);
        }
        finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
    }

    /// <summary>Extracts regular files and directories only, refusing paths that leave <paramref name="target"/>.</summary>
    public static async Task ExtractTarAsync(Stream tar, string target, CancellationToken token)
    {
        var root = Path.GetFullPath(target) + Path.DirectorySeparatorChar;
        var reader = new TarReader(tar);
        while (await reader.GetNextEntryAsync(copyData: false, token) is { } entry)
        {
            var path = Path.GetFullPath(Path.Combine(root, entry.Name.TrimStart('/')));
            if (!path.StartsWith(root, StringComparison.Ordinal)) throw new InvalidDataException("Unsafe path in the compiler package: " + entry.Name);
            if (entry.EntryType == TarEntryType.Directory) Directory.CreateDirectory(path);
            else if (entry.EntryType is TarEntryType.RegularFile or TarEntryType.V7RegularFile && entry.DataStream is not null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                await using var output = File.Create(path);
                await entry.DataStream.CopyToAsync(output, token);
            }
        }
    }

    /// <summary>A directory visible to the tool at <paramref name="Guest"/> (an absolute WASI path).</summary>
    public readonly record struct Mount(string Host, string Guest, bool Writable = false);

    /// <summary>Runs <c>clang</c> or <c>wasm-ld</c> with the clang resource headers at <c>/usr</c> plus <paramref name="mounts"/>;
    /// throws with the tool's own diagnostics when it fails.</summary>
    public Task RunAsync(string tool, IReadOnlyList<string> arguments, IReadOnlyList<Mount> mounts, LauncherCoordinator app, CancellationToken token) =>
        Task.Factory.StartNew(() =>
        {
            token.ThrowIfCancellationRequested();
            // The tool's diagnostics, collected on Wasmtime's output thread (its own lock, never held around the guest call).
            var output = new MemoryStream();
            void Collect(ReadOnlySpan<byte> data) { lock (output) output.Write(data); }
            using var linker = new Linker(engine);
            linker.DefineWasi();
            using var store = new Store(engine);
            var wasi = new WasiConfiguration().WithArgs([tool, .. arguments]).WithStandardErrorCallback(Collect).WithStandardOutputCallback(Collect)
                .WithPreopenedDirectory(usr, "/usr", false);
            foreach (var mount in mounts) wasi = wasi.WithPreopenedDirectory(mount.Host, mount.Guest, mount.Writable);
            store.SetWasiConfiguration(wasi);
            store.SetEpochDeadline(1);
            var instance = linker.Instantiate(store, module);
            int exit;
            using (token.Register(engine.IncrementEpoch))
            {
                try { instance.GetAction("_start")!(); exit = 0; }
                catch (Exception) when (token.IsCancellationRequested) { throw new OperationCanceledException(token); }
                catch (WasmtimeException error) when (error.ExitCode is { } code) { exit = code; }
            }
            string text;
            lock (output) text = Encoding.UTF8.GetString(output.ToArray()).Trim();
            if (text.Length > 0) foreach (var line in text.Split('\n')) app.Log(line.TrimEnd('\r'));
            if (exit != 0) throw new InvalidOperationException($"{tool} exited with code {exit}. Open Activity for details.");
        }, token, TaskCreationOptions.LongRunning, TaskScheduler.Default);

    public void Dispose() { module.Dispose(); engine.Dispose(); }
}
