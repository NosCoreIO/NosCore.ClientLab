using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public sealed class LauncherCoordinator : IAsyncDisposable
{
    private readonly object gate = new();
    private readonly HttpClient http;
    private readonly string root;
    private readonly string directory;
    private JsonObject settings;
    private readonly AsyncLocal<JsonObject?> taskSettings = new();
    private readonly JsonObject state;
    private JsonObject? manifest;
    private CancellationTokenSource? cancellation;
    private Task? job;
    private GameHost? game;
    private string? launchCode;
    private string? launchInstallationId;
    private Task? updateLoop;
    private readonly CancellationTokenSource stopping = new();
    /// <summary>How often an open launcher looks for a new client or a newly published analysis for it.</summary>
    public TimeSpan UpdateInterval { get; init; } = TimeSpan.FromMinutes(20);
    /// <summary>Shorter while a new client waits for its analysis, which the repository publishes within hours.</summary>
    public TimeSpan AnalysisRetryInterval { get; init; } = TimeSpan.FromMinutes(5);
    public string? GameUrl { get; private set; }
    public string DataDirectory => directory;
    // The launcher recompiles with the committed per-build symbols plus the recompiler's own relocation and
    // call-graph discovery, so only the WebAssembly compiler is needed on this machine; Ghidra stays a dev tool.
    private static readonly string[] ToolKeys = ["emcc"];

    public LauncherCoordinator(string sourceRoot, string dataDirectory, HttpMessageHandler? handler = null)
    {
        http = handler is null ? new HttpClient() : new HttpClient(handler);
        http.Timeout = TimeSpan.FromMinutes(5);
        root = sourceRoot; directory = dataDirectory;
        Directory.CreateDirectory(directory);
        settings = ReadJson("settings.json") ?? new JsonObject();
        foreach (var key in new[] { "import_dir", "identity_file", "installation_id", "python", "ghidra", "java", "emcc" })
            if (!settings.ContainsKey(key)) settings[key] = "";
        settings["provider"] ??= "gameforge";
        settings["private_auth_url"] ??= "https://127.0.0.1:7001";
        settings["private_server_host"] ??= "";
        settings["private_port_remap"] ??= "";
        settings["packet_logger"] ??= "false";
        settings["packet_plugin"] ??= "";
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

    // The game picks its resolution from its own settings; the window opens at the last one it reported.
    public (int Width, int Height)? LastGameResolution()
    {
        try
        {
            var saved = ReadJson("game-window.json");
            return saved?["width"]?.GetValue<int>() is int w and >= 320 && saved["height"]?.GetValue<int>() is int h and >= 240 ? (w, h) : null;
        }
        catch (Exception) { return null; }
    }

    public void RememberGameResolution(int width, int height) =>
        SaveJson(Path.Combine(directory, "game-window.json"), new JsonObject { ["width"] = width, ["height"] = height });
    private string Setting(string key) => (taskSettings.Value ?? settings)[key]?.GetValue<string>() ?? "";
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
    /// <summary>Every Activity line as it is logged (the command-line modes print them).</summary>
    public event Action<string>? Logged;

    public void Log(string text)
    {
        lock (gate)
        {
            var logs = state["logs"]!.AsArray(); logs.Add(text);
            while (logs.Count > 500) logs.RemoveAt(0);
        }
        Logged?.Invoke(text);
    }
    public void Status(string stage, string message, double progress = 0)
    { lock (gate) { state["stage"] = stage; state["message"] = message; state["progress"] = progress; } }
    public void Progress(double progress) { lock (gate) state["progress"] = progress; }
    public Task<GameOptions> ReadGameOptionsAsync()
    {
        var snapshot = Snapshot();
        return new GameSettingsStore(directory).ReadOptionsAsync([
            snapshot["settings"]?["import_dir"]?.GetValue<string>() ?? "",
            snapshot["active"]?["client"]?.GetValue<string>() ?? ""]);
    }

    public Task SaveGameOptionsAsync(GameOptions options)
    {
        lock (gate)
            if (game is not null) throw new InvalidOperationException("Close the game before changing its settings.");
        return new GameSettingsStore(directory).UpdateOptionsAsync(options, [Setting("import_dir")]);
    }
    public void SaveSettings(JsonObject values)
    {
        lock (gate)
        {
            var next = settings.DeepClone().AsObject();
            foreach (var key in settings.Select(pair => pair.Key).ToArray())
                if (values.TryGetPropertyValue(key, out var value)) next[key] = value?.GetValue<string>().Trim() ?? "";
            if (next["provider"]?.GetValue<string>() is not ("gameforge" or "noscore")) throw new InvalidOperationException("Unknown sign-in provider.");
            SaveJson(Path.Combine(directory, "settings.json"), next);
            settings = next;
        }
    }
    public void Start(string action) => Start(action, background: false);

    private void Start(string action, bool background)
    {
        if (action is not ("check" or "prepare" or "tools" or "startup")) throw new InvalidOperationException("Unknown task.");
        lock (gate)
        {
            if (action == "startup") updateLoop ??= Task.Run(() => KeepUpdatedAsync(stopping.Token));
            if (stopping.IsCancellationRequested) throw new InvalidOperationException("ClientLab is closing.");
            if (state["busy"]!.GetValue<bool>() || game is not null) throw new InvalidOperationException("Finish the current task or close the game first.");
            state["busy"] = true; state["error"] = null;
            cancellation = new CancellationTokenSource(); var token = cancellation.Token;
            var preparationSettings = settings.DeepClone().AsObject();
            job = Task.Run(async () =>
            {
                taskSettings.Value = preparationSettings;
                try
                {
                    Status("checking", "Checking for updates…");
                    using var lease = await BuildWorkspace.AcquireLeaseAsync(directory, token);
                    if (action is "prepare" or "startup" && ReadJson("active.json") is { } latest)
                        lock (gate) state["active"] = latest;
                    if (action == "check") await CheckAsync(token);
                    else if (action is "prepare" or "startup") await PrepareAsync(token);
                    else
                    {
                        await EnsureToolsAsync(token);
                        Status("idle", "Build tools are ready.", 1);
                    }
                }
                catch (Exception error) when (background && !token.IsCancellationRequested && error is HttpRequestException or TimeoutException or TaskCanceledException && state["active"] is not null)
                {
                    // An unattended check that cannot reach Gameforge must not turn a playable launcher into an error.
                    Log("Background update check failed: " + error.Message);
                    Status("ready", "Ready to play. ClientLab will check for game updates again shortly.", 1);
                }
                catch (OperationCanceledException) { Status("idle", "Task cancelled. Your previous build is still available."); }
                catch (Exception error) { Log(error.Message); lock (gate) state["error"] = error.Message; Status("error", "This step needs attention."); }
                finally { taskSettings.Value = null; lock (gate) state["busy"] = false; }
            }, CancellationToken.None);
        }
    }
    /// <summary>Game patches need no launcher release: while the launcher is open and idle (no task, no game running) it
    /// re-runs the startup preparation, which only fetches the manifest when nothing changed.</summary>
    private async Task KeepUpdatedAsync(CancellationToken token)
    {
        try
        {
            while (true)
            {
                bool waiting;
                lock (gate) waiting = state["awaiting_analysis"] is not null;
                await Task.Delay(waiting ? AnalysisRetryInterval : UpdateInterval, token);
                // A preparation that failed (not a network blip) waits for the player instead of repeating every interval.
                // Waiting for a client's analysis is not a failure: that wait keeps retrying.
                lock (gate) if (state["busy"]!.GetValue<bool>() || game is not null || state["error"] is not null && state["awaiting_analysis"] is null) continue;
                try { Start("startup", background: true); }
                catch (InvalidOperationException) { } // A task or the game started in between; try at the next interval.
            }
        }
        catch (OperationCanceledException) { }
    }

    public void Cancel()
    {
        lock (gate) cancellation?.Cancel(); // Under the lock: a job Start is creating right now is cancelled too.
    }

    public async Task CancelAndWaitAsync()
    {
        Task? pending;
        lock (gate) { cancellation?.Cancel(); pending = job; }
        if (pending is not null) await pending;
    }

    public async Task StartAndWaitAsync(string action)
    {
        Start(action);
        var pending = job!;
        var token = cancellation!.Token;
        await pending;
        token.ThrowIfCancellationRequested();
        lock (gate)
            if (state["error"]?.GetValue<string>() is { } error) throw new InvalidOperationException(error);
    }

    public async Task RunAsync(string executable, IEnumerable<string> arguments, CancellationToken token, Dictionary<string, string>? environment = null)
    {
        token.ThrowIfCancellationRequested();
        var info = new ProcessStartInfo(executable) { WorkingDirectory = directory, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        if (environment is not null) foreach (var item in environment) info.Environment[item.Key] = item.Value;
        Log("Running " + Path.GetFileName(executable));
        using var child = Process.Start(info) ?? throw new InvalidOperationException("Could not start " + executable);
        // Every compiler has its own registration: compilation runs several children in parallel.
        // Disposing Process alone does not terminate it when cancellation interrupts output draining.
        using var registration = token.Register(() =>
        {
            try { if (!child.HasExited) child.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        });
        async Task Drain(StreamReader stream) { while (await stream.ReadLineAsync(token) is { } line) Log(line); }
        try { await Task.WhenAll(Drain(child.StandardOutput), Drain(child.StandardError), child.WaitForExitAsync(token)); }
        finally
        {
            if (token.IsCancellationRequested) await child.WaitForExitAsync(CancellationToken.None);
        }
        token.ThrowIfCancellationRequested();
        if (child.ExitCode != 0) throw new InvalidOperationException($"{Path.GetFileName(executable)} exited with code {child.ExitCode}. Open Activity for details.");
    }

    public static string SafeRelativePath(string value)
    {
        value = value.Replace('\\', '/');
        if (string.IsNullOrWhiteSpace(value) || value.StartsWith('/') || value.Split('/').Any(part => part is ".." or "." or "" || part.Contains(':') || part.EndsWith('.') || part.EndsWith(' ')))
            throw new InvalidDataException("The patch manifest contains an unsafe file path.");
        return value;
    }
    public const string PatchBaseUrl = "http://patches.gameforge.com";
    public const string AnalysisUrl ="https://raw.githubusercontent.com/NosCoreIO/NosCore.ClientLab/main/symbols/";
    private static readonly string[] AnalysisFiles = ["ghidra-functions.txt", "ghidra-seeds.txt"];

    private static bool HasAnalysis(string symbols)
    {
        var functions = Path.Combine(symbols, "ghidra-functions.txt");
        return File.Exists(functions) && NosCore.ClientLab.Recompiler.FunctionDiscovery.ReadFunctions(functions).Count > 0;
    }

    /// <summary>The analysis for an exact client executable (its MD5): bundled with this launcher, fetched earlier, or
    /// fetched now from the repository, where a scheduled workflow commits it for every client patch.</summary>
    public async Task<string?> AnalysisDirectoryAsync(string buildId, CancellationToken token)
    {
        var bundled = Path.Combine(root, "symbols", buildId);
        if (HasAnalysis(bundled)) return bundled;
        var fetched = Path.Combine(directory, "symbols", buildId);
        if (!HasAnalysis(fetched)) await DownloadAnalysisAsync(buildId, fetched, token);
        return HasAnalysis(fetched) ? fetched : null;
    }

    /// <summary>Downloads the analysis metadata (function ranges and seeds, no client bytes) committed for this exact
    /// executable. Leaves nothing behind when it is not there yet or the download fails.</summary>
    private async Task DownloadAnalysisAsync(string buildId, string target, CancellationToken token)
    {
        if (!System.Text.RegularExpressions.Regex.IsMatch(buildId, "^[0-9a-f]{32}$")) return;
        Status("analysing", "Fetching the analysis for this client version…");
        var staging = target + ".download";
        try
        {
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            Directory.CreateDirectory(staging);
            foreach (var file in AnalysisFiles)
            {
                using var response = await http.GetAsync(AnalysisUrl + buildId + "/" + file, HttpCompletionOption.ResponseHeadersRead, token);
                if (response.StatusCode == System.Net.HttpStatusCode.NotFound)
                {
                    if (file != AnalysisFiles[0]) continue;
                    Log($"No published analysis for client build {buildId} yet.");
                    return;
                }
                response.EnsureSuccessStatusCode();
                if (response.Content.Headers.ContentLength > 32L * 1024 * 1024) throw new InvalidDataException("The published analysis is unexpectedly large.");
                await File.WriteAllBytesAsync(Path.Combine(staging, file), await response.Content.ReadAsByteArrayAsync(token), token);
            }
            if (!HasAnalysis(staging)) throw new InvalidDataException("The published analysis contains no functions.");
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Directory.Move(staging, target);
            Log($"Using the published analysis for client build {buildId}.");
        }
        catch (Exception error) when (error is HttpRequestException or InvalidDataException or IOException)
        {
            Log($"Could not fetch the analysis for client build {buildId}: {error.Message}");
        }
        finally { if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true); }
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
        // CDN paths and manifest ordering can change without any game file changing.
        var content = new JsonArray(entries.OrderBy(item => item!["file"]!.GetValue<string>(), StringComparer.Ordinal)
            .Select(item => (JsonNode)new JsonObject { ["file"] = item!["file"]!.GetValue<string>(), ["sha1"] = item["sha1"]!.GetValue<string>(), ["size"] = item["size"]!.GetValue<long>() }).ToArray());
        var id = Convert.ToHexStringLower(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(content.ToJsonString())))[..20];
        manifest = new JsonObject { ["id"] = id, ["build"] = response["build"]?.ToString() ?? id, ["entries"] = entries };
        SaveJson(Path.Combine(directory, "latest.json"), manifest);
        var total = entries.Sum(item => item!["size"]!.GetValue<long>());
        var current = Path.Combine(directory, "versions", id, "client");
        var missing = VerifiedClientFiles.IsCurrent(current, id, entries) ? 0 : GameFiles.DownloadBytes(entries, GameFiles.Donors(KnownClients()));
        lock (gate)
        {
            state["remote"] = new JsonObject { ["id"] = id, ["build"] = manifest["build"]!.GetValue<string>() };
            state["plan"] = new JsonObject { ["total_bytes"] = total, ["download_bytes"] = missing, ["files"] = entries.Count };
        }
        Status("idle", "Latest version found. Ready when you are.", 1);
    }

    /// <summary>Game folders of every prepared version, newest first: the sources unchanged files are linked from.</summary>
    private List<string> KnownClients()
    {
        var clients = new List<string>();
        JsonNode? active;
        lock (gate) active = state["active"]?.DeepClone();
        if (active?["client"]?.GetValue<string>() is { Length: > 0 } activeClient) clients.Add(activeClient);
        var versions = Path.Combine(directory, "versions");
        if (Directory.Exists(versions))
            clients.AddRange(new DirectoryInfo(versions).EnumerateDirectories().OrderByDescending(item => item.LastWriteTimeUtc)
                .Select(item => Path.Combine(item.FullName, "client")).Where(Directory.Exists));
        return clients;
    }

    // The patch CDN's certificate does not cover patches.gameforge.com, so like the official launcher this uses
    // plain HTTP; every file is checked against the SHA-1 of the manifest, which comes over HTTPS.
    private Task<HttpResponseMessage> FetchPatchFileAsync(JsonNode entry, CancellationToken token) =>
        http.GetAsync(PatchBaseUrl + entry["path"]!.GetValue<string>(), HttpCompletionOption.ResponseHeadersRead, token);

    /// <summary>Removes what a newer layout or a newer version made redundant: the old content-addressed store (every
    /// game file kept a second time), versions nothing points at any more, and abandoned build attempts. Anything in use
    /// by another process stays and is retried after the next preparation.</summary>
    private void Prune(string currentVersion)
    {
        JsonNode? active;
        lock (gate) active = state["active"]?.DeepClone();
        var keep = new[] { active?["client"], active?["runtime"], active?["executable"] }
            .Select(value => value?.GetValue<string>()).Where(value => value is { Length: > 0 }).Select(value => Path.GetFullPath(value!)).ToList();
        bool Kept(string path) => keep.Any(item => item.StartsWith(Path.GetFullPath(path) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));
        void Remove(string path)
        {
            try { Directory.Delete(path, recursive: true); Log("Removed " + Path.GetRelativePath(directory, path) + "."); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
        var legacy = Path.Combine(directory, "content");
        if (Directory.Exists(legacy)) Remove(legacy);
        var versions = Path.Combine(directory, "versions");
        if (!Directory.Exists(versions)) return;
        foreach (var version in Directory.EnumerateDirectories(versions))
        {
            var isCurrent = Path.GetFileName(version) == currentVersion;
            if (!isCurrent && !Kept(version)) { Remove(version); continue; }
            var builds = Path.Combine(version, "builds");
            if (Directory.Exists(builds))
                foreach (var build in Directory.EnumerateDirectories(builds))
                    if (!Kept(build)) Remove(build);
        }
    }

    private async Task PrepareAsync(CancellationToken token)
    {
        string version, client, pinned, buildVersionId, buildLabel;
        // Both sign-in modes play the current official client: the host points its connections at the private
        // server, and files the local install already has are copied rather than downloaded.
        await CheckAsync(token);
        buildVersionId = manifest!["id"]!.GetValue<string>(); buildLabel = manifest["build"]!.GetValue<string>();
        // A complete current runtime can be reused on startup; failed/partial builds are never selected.
        lock (gate) state["awaiting_analysis"] = null;
        if (await CanReuseBuildAsync(buildVersionId, token))
        { Prune(buildVersionId); Status("ready", "Your game is up to date.", 1); return; }
        var versionDirectory = Path.Combine(directory, "versions", buildVersionId);
        client = Path.Combine(versionDirectory, "client");
        var entries = manifest["entries"]!.AsArray();
        var files = new GameFiles(FetchPatchFileAsync, Log, (text, progress) => Status(text.StartsWith("Downloading") ? "downloading" : "checking", text, progress));
        await files.SyncAsync(client, buildVersionId, entries, KnownClients(), Setting("import_dir"), token);
        var executable = Path.Combine(client, "NostaleClientX.exe");
        // Asset-only patches still install, but do not invalidate compiled machine code.
        JsonObject? previous;
        lock (gate) previous = state["active"]?.DeepClone().AsObject();
        if (previous is not null && await CanReuseBuildAsync(previous["id"]!.GetValue<string>(), token) &&
            await HashAsync(executable, "sha256", token) == previous["executable_sha256"]!.GetValue<string>())
        {
            previous["id"] = buildVersionId; previous["build"] = buildLabel; previous["client"] = client;
            SaveJson(Path.Combine(versionDirectory, "build.json"), previous);
            SaveJson(Path.Combine(directory, "active.json"), previous);
            lock (gate) state["active"] = previous;
            Log("Game files updated; the executable is unchanged, so the compiled runtime was reused.");
            Prune(buildVersionId);
            Status("ready", "Your game is up to date.", 1);
            return;
        }
        // Checked before a build workspace exists, so waiting for the analysis leaves nothing behind on each retry.
        var buildId = await HashAsync(Path.Combine(client, "NostaleClientX.exe"), "md5", token);
        var symbolsDirectory = await AnalysisDirectoryAsync(buildId, token);
        if (symbolsDirectory is null)
        {
            // The repository's client-analysis workflow publishes it within hours of a patch; the background update check
            // picks it up then, without a launcher release. Until then the previous build stays playable.
            const string waiting = "Gameforge released a new client. ClientLab is preparing support for it and will update automatically.";
            lock (gate) state["awaiting_analysis"] = buildId;
            if (previous is null) throw new InvalidOperationException(waiting);
            Log($"No published analysis for client build {buildId} yet; keeping the previous build and retrying in the background.");
            Status("ready", waiting, 1);
            return;
        }
        version = BuildWorkspace.Create(versionDirectory);
        pinned = Path.Combine(version, "compiled-client.exe"); File.Copy(Path.Combine(client, "NostaleClientX.exe"), pinned, overwrite: true);
        var image = new NosCore.ClientLab.Recompiler.ExecutableImage(pinned);
        var digest = await HashAsync(pinned, "sha256", token);
        if (await HashAsync(pinned, "md5", token) != buildId) throw new InvalidDataException("The client executable changed while it was being prepared.");
        Status("analysing", "Preparing the client…");
        // Seeds come from the symbols committed for this exact build; the recompiler adds the rest through its own
        // relocation and call-graph discovery. No Ghidra/Java analysis runs here (that stays a dev-only pipeline).
        var seedAddresses = new List<string> { image.EntryPoint.ToString("x8") };
        var ghidraFunctions = Path.Combine(symbolsDirectory, "ghidra-functions.txt");
        await EnsureToolsAsync(token);
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
        // Preserve the complete Ghidra bodies, including discontiguous ranges, as the
        // established Python pipeline does. Entry seeds alone lose this information.
        var functions = Path.Combine(version, "functions.txt");
        File.Copy(ghidraFunctions, functions, overwrite: true);
        Log("Using full Ghidra function ranges for this exact client build.");
        var seeds = Path.Combine(version, "seeds.txt");
        await File.WriteAllTextAsync(seeds, string.Join('\n', seedAddresses.Distinct()) + "\n", token);
        var generated = Path.Combine(version, "generated"); Status("translating", "Translating the client with the C# recompiler…");
        var recompilerArguments = new List<string>();
        if (string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath), "dotnet", StringComparison.OrdinalIgnoreCase))
            recompilerArguments.Add(Path.Combine(AppContext.BaseDirectory, "ClientLab.dll"));
        recompilerArguments.AddRange(["--recompile", pinned, functions, generated, "--seeds", seeds, "--allow-unsupported"]);
        await RunAsync(Environment.ProcessPath!, recompilerArguments, token);
        var report = JsonNode.Parse(File.ReadAllText(Path.Combine(generated, "recompiler-report.json")))!;
        if (report["imageSha256"]!.GetValue<string>() != digest) throw new InvalidDataException("Recompiler image hash does not match the selected client.");
        var unsupported = report["unsupported"]!.AsArray().Count;
        if (unsupported > 0) Log($"Compatibility notice: {unsupported} unimplemented instruction sites; this build is experimental.");
        Status("compiling", "Compiling the game runtime…");
        var build = Path.Combine(version, "wasm/web");
        await WasmCompiler.BuildAsync(root, generated, build, Setting("emcc"), this, token);
        var record = new JsonObject { ["id"] = buildVersionId, ["build"] = buildLabel,
            ["executable_sha256"] = digest, ["client"] = client, ["executable"] = pinned, ["runtime"] = build,
            ["created"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds(), ["unsupported_sites"] = unsupported,
            ["runtime_revision"] = NosCore.ClientLab.Recompiler.Program.RuntimeRevision };
        SaveJson(Path.Combine(version, "build.json"), record); SaveJson(Path.Combine(directory, "active.json"), record);
        lock (gate) state["active"] = record;
        Prune(buildVersionId);
        Status("ready", "Your build is ready to launch.", 1);
    }

    private async Task EnsureToolsAsync(CancellationToken token)
    {
        if (ToolKeys.All(key => File.Exists(Setting(key)) || Directory.Exists(Setting(key)))) return;
        var tools = await ToolchainInstaller.InstallAsync(directory, this, token);
        lock (gate)
        {
            foreach (var item in tools)
            {
                var previous = Setting(item.Key);
                if (settings[item.Key]?.GetValue<string>() == previous) settings[item.Key] = item.Value;
                if (taskSettings.Value is { } snapshot) snapshot[item.Key] = item.Value;
            }
            SaveJson(Path.Combine(directory, "settings.json"), settings);
        }
    }

    private async Task<bool> CanReuseBuildAsync(string id, CancellationToken token)
    {
        JsonNode? active;
        lock (gate) active = state["active"]?.DeepClone();
        return active?["id"]?.GetValue<string>() == id &&
            active["runtime_revision"]?.GetValue<int>() == NosCore.ClientLab.Recompiler.Program.RuntimeRevision &&
            active["executable"]?.GetValue<string>() is { } executable && File.Exists(executable) &&
            active["runtime"]?.GetValue<string>() is { } runtime &&
            new[] { "nostale.js", "nostale.wasm" }.All(file => File.Exists(Path.Combine(runtime, file)) && new FileInfo(Path.Combine(runtime, file)).Length > 0) &&
            await HashAsync(executable, "sha256", token) == active["executable_sha256"]?.GetValue<string>();
    }

    public async Task<string> LaunchAsync()
    {
        JsonObject active;
        lock (gate)
        {
            if (state["busy"]!.GetValue<bool>()) throw new InvalidOperationException("Wait for the current task to finish.");
            if (game is not null) return GameUrl!;
            active = state["active"]?.DeepClone().AsObject() ?? throw new InvalidOperationException("Prepare a client build first.");
            // Reserved until the game runs, so a background preparation cannot prune the files it is about to serve.
            state["busy"] = true;
        }
        try
        {
            if (await HashAsync(active["executable"]!.GetValue<string>(), "sha256", default) != active["executable_sha256"]!.GetValue<string>())
                throw new InvalidDataException("The client executable changed. Prepare a matching runtime before playing.");
            if (Setting("packet_logger") == "true")
            {
                var sites = NosCore.ClientLab.Recompiler.PacketHooks.Find(new NosCore.ClientLab.Recompiler.ExecutableImage(active["executable"]!.GetValue<string>()));
                if (sites.Count != 3) throw new InvalidOperationException("This client does not match all packet logger hooks. Disable packet logging to play, or update its analysis.");
                Log("DeveloperTools packet hooks: " + string.Join(", ", sites.Select(site => $"{site.Value}@{site.Key:x8}")));
            }
            game = await GameHost.StartAsync(root, active, settings, Log, new GameSettingsStore(directory));
            if (launchCode is { Length: > 0 })
            {
                // Signed in natively: hand the game the launch code through the page URL; no browser login needed.
                GameUrl = game.Url + "?auth=" + Uri.EscapeDataString(launchCode);
                if (launchInstallationId is { Length: > 0 }) GameUrl += "&NC_INSTALLATION_ID=" + Uri.EscapeDataString(launchInstallationId);
            }
            else GameUrl = game.Url + (Setting("provider") == "gameforge" ? "?login=gameforge" : "");
            if (Setting("packet_logger") == "true") GameUrl += (GameUrl.Contains('?') ? "&" : "?") + "PORT_PACKET_LOG=1";
            if (Setting("packet_logger") == "true" && Setting("packet_plugin").Length > 0) GameUrl += "&PORT_PLUGIN=1";
            lock (gate) state["running"] = true;
            return GameUrl;
        }
        finally { lock (gate) state["busy"] = false; }
    }

    public string GameforgeInstallationId { get { using var auth = new GameforgeAuthentication(settings); return auth.InstallationId; } }
    public GameforgeSignIn CreateSignIn(IGameforgeIdentity? identity = null) => new(settings, identity);
    public GameforgeBrowserIdentity CreateBrowserIdentity() => new(GameforgeInstallationId, directory, Setting("identity_file"));

    public void UseLaunchCode(string code, string? installationId)
    {
        lock (gate)
        {
            launchCode = Setting("provider") == "gameforge" ? GameforgeAuthentication.ClientTicket(code) : code;
            launchInstallationId = installationId;
        }
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
        stopping.Cancel();
        Cancel();
        if (job is not null) try { await job.WaitAsync(TimeSpan.FromSeconds(8)); } catch (TimeoutException) { }
        await CloseGameAsync(); http.Dispose(); cancellation?.Dispose();
    }
}
