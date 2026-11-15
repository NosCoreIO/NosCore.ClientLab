using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public sealed class LauncherCoordinator : IAsyncDisposable
{
    private readonly object gate = new();
    private readonly HttpClient http = new() { Timeout = TimeSpan.FromMinutes(5) };
    private readonly string root;
    private readonly string directory;
    private JsonObject settings;
    private readonly JsonObject state;
    private JsonObject? manifest;
    private CancellationTokenSource? cancellation;
    private Task? job;
    private Process? process;
    private GameHost? game;
    private string? launchCode;
    private string? launchInstallationId;
    public string? GameUrl { get; private set; }
    // The launcher recompiles with the committed per-build symbols plus the recompiler's own relocation and
    // call-graph discovery, so only the WebAssembly compiler is needed on this machine; Ghidra stays a dev tool.
    private static readonly string[] ToolKeys = ["emcc"];

    public LauncherCoordinator(string sourceRoot, string dataDirectory)
    {
        root = sourceRoot; directory = dataDirectory;
        Directory.CreateDirectory(directory);
        settings = ReadJson("settings.json") ?? new JsonObject();
        foreach (var key in new[] { "import_dir", "identity_file", "installation_id", "python", "ghidra", "java", "emcc" })
            if (!settings.ContainsKey(key)) settings[key] = "";
        settings["provider"] ??= "gameforge";
        if (Setting("import_dir").Length == 0 && DetectInstall() is { Length: > 0 } detected) settings["import_dir"] = detected;
        state = new JsonObject { ["busy"] = false, ["stage"] = "idle", ["progress"] = 0d,
            ["message"] = "Make yourself at home.", ["logs"] = new JsonArray(), ["running"] = false,
            ["active"] = ReadJson("active.json") };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("NosCore.ClientLab/0.1");
    }

    private static string DetectInstall()
    {
        string[] candidates = OperatingSystem.IsWindows()
            ? [@"C:\Program Files (x86)\Nostale", @"C:\Program Files\Nostale", @"C:\Program Files (x86)\Gameforge\NosTale", @"C:\Gameforge\NosTale"]
            : OperatingSystem.IsMacOS()
                ? [Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library/Application Support/NosTale")]
                : [Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".nostale"), "/opt/nostale"];
        return candidates.FirstOrDefault(path => File.Exists(Path.Combine(path, "NosCore.exe")) || File.Exists(Path.Combine(path, "NostaleClientX.exe"))) ?? "";
    }

    private JsonObject? ReadJson(string name) => File.Exists(Path.Combine(directory, name)) ? JsonNode.Parse(File.ReadAllText(Path.Combine(directory, name)))?.AsObject() : null;
    private string Setting(string key) => settings[key]?.GetValue<string>() ?? "";
    private void SaveJson(string path, JsonNode value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, value.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        File.Move(temporary, path, overwrite: true);
    }
    public JsonObject Snapshot()
    {
        lock (gate)
        {
            var result = state.DeepClone().AsObject(); result["settings"] = settings.DeepClone();
            result["data_dir"] = directory;
            var tools = new JsonObject();
            foreach (var key in ToolKeys) tools[key] = File.Exists(Setting(key)) || Directory.Exists(Setting(key));
            result["tools"] = tools; return result;
        }
    }
    public void Log(string text)
    {
        lock (gate)
        {
            var logs = state["logs"]!.AsArray(); logs.Add(text);
            while (logs.Count > 500) logs.RemoveAt(0);
        }
    }
    public void Status(string stage, string message, double progress = 0)
    { lock (gate) { state["stage"] = stage; state["message"] = message; state["progress"] = progress; } }
    public void Progress(double progress) { lock (gate) state["progress"] = progress; }
    public void SaveSettings(JsonObject values)
    {
        lock (gate)
        {
            if (state["busy"]!.GetValue<bool>() || game is not null) throw new InvalidOperationException("Close the game or wait for the current task before changing settings.");
            foreach (var key in settings.Select(pair => pair.Key).ToArray())
                if (values.TryGetPropertyValue(key, out var value)) settings[key] = value?.GetValue<string>().Trim() ?? "";
            if (Setting("provider") is not ("gameforge" or "noscore")) throw new InvalidOperationException("Unknown sign-in provider.");
            SaveJson(Path.Combine(directory, "settings.json"), settings);
        }
    }
    public void Start(string action)
    {
        if (action is not ("check" or "prepare" or "tools")) throw new InvalidOperationException("Unknown task.");
        lock (gate)
        {
            if (state["busy"]!.GetValue<bool>() || game is not null) throw new InvalidOperationException("Finish the current task or close the game first.");
            state["busy"] = true; state["error"] = null;
            cancellation = new CancellationTokenSource(); var token = cancellation.Token;
            job = Task.Run(async () =>
            {
                try
                {
                    if (action == "check") await CheckAsync(token);
                    else if (action == "prepare") await PrepareAsync(token);
                    else
                    {
                        var tools = await ToolchainInstaller.InstallAsync(directory, this, token);
                        lock (gate) { foreach (var item in tools) settings[item.Key] = item.Value; SaveJson(Path.Combine(directory, "settings.json"), settings); }
                        Status("idle", "Build tools are ready.", 1);
                    }
                }
                catch (OperationCanceledException) { Status("idle", "Task cancelled. Your previous build is still available."); }
                catch (Exception error) { Log(error.Message); lock (gate) state["error"] = error.Message; Status("error", "This step needs attention."); }
                finally { lock (gate) state["busy"] = false; }
            }, CancellationToken.None);
        }
    }
    public void Cancel()
    {
        cancellation?.Cancel();
        try { if (process is { HasExited: false }) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
    }

    public async Task RunAsync(string executable, IEnumerable<string> arguments, CancellationToken token, Dictionary<string, string>? environment = null)
    {
        token.ThrowIfCancellationRequested();
        var info = new ProcessStartInfo(executable) { WorkingDirectory = directory, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        if (environment is not null) foreach (var item in environment) info.Environment[item.Key] = item.Value;
        Log("Running " + Path.GetFileName(executable));
        using var child = Process.Start(info) ?? throw new InvalidOperationException("Could not start " + executable);
        process = child;
        async Task Drain(StreamReader stream) { while (await stream.ReadLineAsync(token) is { } line) Log(line); }
        await Task.WhenAll(Drain(child.StandardOutput), Drain(child.StandardError), child.WaitForExitAsync(token));
        process = null; token.ThrowIfCancellationRequested();
        if (child.ExitCode != 0) throw new InvalidOperationException($"{Path.GetFileName(executable)} exited with code {child.ExitCode}. Open Activity for details.");
    }

    public static string SafeRelativePath(string value)
    {
        value = value.Replace('\\', '/');
        if (string.IsNullOrWhiteSpace(value) || value.StartsWith('/') || value.Split('/').Any(part => part is ".." or "." or "" || part.Contains(':') || part.EndsWith('.') || part.EndsWith(' ')))
            throw new InvalidDataException("The patch manifest contains an unsafe file path.");
        return value;
    }
    private static async Task<string> HashAsync(string path, string algorithm, CancellationToken token)
    {
        await using var file = File.OpenRead(path);
        var bytes = algorithm switch
        {
            "sha1" => await SHA1.HashDataAsync(file, token),
            "md5" => await MD5.HashDataAsync(file, token),
            _ => await SHA256.HashDataAsync(file, token),
        };
        return Convert.ToHexStringLower(bytes);
    }

    private async Task CheckAsync(CancellationToken token)
    {
        Status("checking", "Checking the latest game files…");
        var json = await http.GetStringAsync("https://spark.gameforge.com/api/v1/patching/download/latest/nostale/default?locale=en&architecture=x64&branchToken", token);
        var response = JsonNode.Parse(json)!.AsObject(); var entries = new JsonArray(); var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var item in response["entries"]!.AsArray())
        {
            if (item?["folder"]?.GetValue<bool>() == true) continue;
            var name = SafeRelativePath(item!["file"]!.GetValue<string>());
            var checksum = item["sha1"]!.GetValue<string>().ToLowerInvariant();
            var path = item["path"]!.GetValue<string>(); var size = item["size"]!.GetValue<long>();
            if (!names.Add(name) || checksum.Length != 40 || !checksum.All(Uri.IsHexDigit) || !path.StartsWith('/') || path.StartsWith("//") || size < 0)
                throw new InvalidDataException("Invalid entry in the patch manifest.");
            entries.Add(new JsonObject { ["file"] = name, ["sha1"] = checksum, ["path"] = path, ["size"] = size });
        }
        if (entries.Count == 0) throw new InvalidDataException("Gameforge returned an empty patch manifest.");
        var id = Convert.ToHexStringLower(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(entries.ToJsonString())))[..20];
        manifest = new JsonObject { ["id"] = id, ["build"] = response["build"]?.ToString() ?? id, ["entries"] = entries };
        SaveJson(Path.Combine(directory, "latest.json"), manifest);
        var total = entries.Sum(item => item!["size"]!.GetValue<long>());
        var missing = entries.Where(item => !File.Exists(Path.Combine(directory, "content", item!["sha1"]!.GetValue<string>()))).Sum(item => item!["size"]!.GetValue<long>());
        lock (gate)
        {
            state["remote"] = new JsonObject { ["id"] = id, ["build"] = manifest["build"]!.GetValue<string>() };
            state["plan"] = new JsonObject { ["total_bytes"] = total, ["download_bytes"] = missing, ["files"] = entries.Count };
        }
        Status("idle", "Latest version found. Ready when you are.", 1);
    }

    private async Task<string> DownloadAsync(JsonNode entry, CancellationToken token)
    {
        var digest = entry["sha1"]!.GetValue<string>(); var name = entry["file"]!.GetValue<string>();
        var size = entry["size"]!.GetValue<long>(); var store = Path.Combine(directory, "content", digest);
        Directory.CreateDirectory(Path.GetDirectoryName(store)!);
        if (File.Exists(store) && new FileInfo(store).Length == size && await HashAsync(store, "sha1", token) == digest) return store;
        var temporary = store + ".part";
        var imported = Path.Combine(Setting("import_dir"), name);
        if (Setting("import_dir").Length > 0 && File.Exists(imported) && new FileInfo(imported).Length == size && await HashAsync(imported, "sha1", token) == digest)
            File.Copy(imported, temporary, overwrite: true);
        else
        {
            using var response = await http.GetAsync("https://patches.gameforge.com" + entry["path"]!.GetValue<string>(), HttpCompletionOption.ResponseHeadersRead, token);
            response.EnsureSuccessStatusCode();
            await using var output = File.Create(temporary);
            await response.Content.CopyToAsync(output, token);
        }
        if (new FileInfo(temporary).Length != size || await HashAsync(temporary, "sha1", token) != digest)
            throw new InvalidDataException($"Checksum mismatch for {name}. The downloaded file was not installed.");
        File.Move(temporary, store, overwrite: true); return store;
    }

    private async Task PrepareAsync(CancellationToken token)
    {
        if (!ToolKeys.All(key => File.Exists(Setting(key)) || Directory.Exists(Setting(key)))) throw new InvalidOperationException("Set up build tools first.");
        string version, client, pinned, buildVersionId, buildLabel;
        // The runtime targets the NosCore-patched client; fall back to the stock executable only if it is absent.
        var installExe = Setting("import_dir").Length > 0
            ? new[] { "NosCore.exe", "NostaleClientX.exe" }.Select(name => Path.Combine(Setting("import_dir"), name)).FirstOrDefault(File.Exists)
            : null;
        if (installExe is not null)
        {
            // Install mode: build and play straight from the existing local NosTale install; no Gameforge download.
            Status("downloading", "Using your installed NosTale files…");
            buildVersionId = (await HashAsync(installExe, "sha1", token))[..16];
            buildLabel = "installed-" + buildVersionId[..6];
            version = Path.Combine(directory, "versions", buildVersionId); Directory.CreateDirectory(version);
            client = Setting("import_dir");
            pinned = Path.Combine(version, "compiled-client.exe"); File.Copy(installExe, pinned, overwrite: true);
        }
        else
        {
            // Download mode: fetch the client from Gameforge (needs a reachable patch host).
            await CheckAsync(token);
            buildVersionId = manifest!["id"]!.GetValue<string>(); buildLabel = manifest["build"]!.GetValue<string>();
            version = Path.Combine(directory, "versions", buildVersionId);
            client = Path.Combine(version, "client"); Directory.CreateDirectory(client);
            var entries = manifest["entries"]!.AsArray(); var total = Math.Max(1, entries.Sum(item => item!["size"]!.GetValue<long>())); long completed = 0;
            foreach (var entry in entries)
            {
                token.ThrowIfCancellationRequested(); Status("downloading", "Preparing " + entry!["file"]!.GetValue<string>(), completed / (double)total);
                var source = await DownloadAsync(entry, token); var target = Path.Combine(client, entry["file"]!.GetValue<string>());
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                if (!File.Exists(target) || await HashAsync(target, "sha1", token) != entry["sha1"]!.GetValue<string>())
                { File.Copy(source, target + ".installing", overwrite: true); File.Move(target + ".installing", target, overwrite: true); }
                completed += entry["size"]!.GetValue<long>();
            }
            pinned = Path.Combine(version, "compiled-client.exe"); File.Copy(Path.Combine(client, "NostaleClientX.exe"), pinned, overwrite: true);
        }
        var image = new NosCore.ClientLab.Recompiler.ExecutableImage(pinned);
        var digest = await HashAsync(pinned, "sha256", token);
        var buildId = await HashAsync(pinned, "md5", token);
        Status("analysing", "Preparing the client…");
        // Seeds come from the symbols committed for this exact build; the recompiler adds the rest through its own
        // relocation and call-graph discovery. No Ghidra/Java analysis runs here (that stays a dev-only pipeline).
        var seedAddresses = new List<string> { image.EntryPoint.ToString("x8") };
        var symbolsDirectory = Path.Combine(root, "symbols", buildId);
        if (Directory.Exists(symbolsDirectory))
        {
            foreach (var file in Directory.EnumerateFiles(symbolsDirectory, "*.txt"))
                foreach (var line in File.ReadLines(file))
                {
                    var text = line.Trim();
                    if (text.Length == 0 || text.StartsWith('#')) continue;
                    seedAddresses.Add(text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)[0]);
                }
            Log($"Using committed symbols for client build {buildId} ({seedAddresses.Count} seeds).");
        }
        else
            Log($"No committed symbols for client build {buildId}; relying on automatic discovery.");
        var functions = Path.Combine(version, "functions.txt"); await File.WriteAllTextAsync(functions, "", token);
        var seeds = Path.Combine(version, "seeds.txt");
        await File.WriteAllTextAsync(seeds, string.Join('\n', seedAddresses.Distinct()) + "\n", token);
        var generated = Path.Combine(version, "generated"); Status("translating", "Translating the client with the C# recompiler…");
        await RunAsync(Environment.ProcessPath!, ["--recompile", pinned, functions, generated, "--seeds", seeds, "--allow-unsupported"], token);
        var report = JsonNode.Parse(File.ReadAllText(Path.Combine(generated, "recompiler-report.json")))!;
        if (report["imageSha256"]!.GetValue<string>() != digest) throw new InvalidDataException("Recompiler image hash does not match the selected client.");
        var unsupported = report["unsupported"]!.AsArray().Count;
        if (unsupported > 0) Log($"Compatibility notice: {unsupported} unimplemented instruction sites; this build is experimental.");
        Status("compiling", "Compiling the game runtime…");
        var build = Path.Combine(version, "wasm/web");
        await WasmCompiler.BuildAsync(root, generated, build, Setting("emcc"), this, token);
        var record = new JsonObject { ["id"] = buildVersionId, ["build"] = buildLabel,
            ["executable_sha256"] = digest, ["client"] = client, ["executable"] = pinned, ["runtime"] = build,
            ["created"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds(), ["unsupported_sites"] = unsupported };
        SaveJson(Path.Combine(version, "build.json"), record); SaveJson(Path.Combine(directory, "active.json"), record);
        lock (gate) state["active"] = record;
        Status("ready", "Your build is ready to launch.", 1);
    }

    public async Task<string> LaunchAsync()
    {
        JsonObject active;
        lock (gate)
        {
            if (state["busy"]!.GetValue<bool>()) throw new InvalidOperationException("Wait for the current task to finish.");
            if (game is not null) return GameUrl!;
            active = state["active"]?.DeepClone().AsObject() ?? throw new InvalidOperationException("Prepare a client build first.");
        }
        if (await HashAsync(active["executable"]!.GetValue<string>(), "sha256", default) != active["executable_sha256"]!.GetValue<string>())
            throw new InvalidDataException("The client executable changed. Prepare a matching runtime before playing.");
        game = await GameHost.StartAsync(root, active, settings);
        if (launchCode is { Length: > 0 })
        {
            // Signed in natively: hand the game the launch code through the page URL; no browser login needed.
            GameUrl = game.Url + "?auth=" + Uri.EscapeDataString(launchCode);
            if (launchInstallationId is { Length: > 0 }) GameUrl += "&NC_INSTALLATION_ID=" + Uri.EscapeDataString(launchInstallationId);
        }
        else GameUrl = game.Url + (Setting("provider") == "gameforge" ? "?login=gameforge" : "");
        lock (gate) state["running"] = true;
        return GameUrl;
    }

    public GameforgeSignIn CreateSignIn() => new(settings);

    public void UseLaunchCode(string code, string? installationId)
    {
        lock (gate) { launchCode = code; launchInstallationId = installationId; }
    }

    public void ClearLaunchCode() { lock (gate) { launchCode = null; launchInstallationId = null; } }

    public async Task CloseGameAsync()
    {
        if (game is not null) await game.DisposeAsync();
        game = null; GameUrl = null;
        lock (gate) state["running"] = false;
    }
    public async ValueTask DisposeAsync()
    {
        Cancel();
        if (job is not null) try { await job.WaitAsync(TimeSpan.FromSeconds(8)); } catch (TimeoutException) { }
        await CloseGameAsync(); http.Dispose(); cancellation?.Dispose();
    }
}
