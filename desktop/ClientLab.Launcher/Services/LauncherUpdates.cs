using NosCore.ClientLab.Updates;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Downloads newer launcher releases in the background and stages them for the bootstrap, which swaps its own
/// executable on the next start (or when the player chooses to restart). Only runs for a released launcher started
/// through its bootstrap: <c>dotnet run</c> and development builds never update themselves. Game client updates do
/// not need a launcher release; they come from the official patch manifest and the analysis published in the repo.
/// </summary>
public sealed class LauncherUpdates : IDisposable
{
    private static readonly TimeSpan Interval = TimeSpan.FromHours(6);
    private readonly HttpMessageHandler? handler;
    private readonly CancellationTokenSource stopped = new();
    private readonly Action<string> log;
    private readonly string manifestUrl, assetBaseUrl;
    public string Updates { get; }
    public string? BootstrapPath { get; }
    public string Runtime { get; }
    public string CurrentVersion { get; }
    public StagedUpdate? Staged { get; private set; }
    public event Action<StagedUpdate>? StagedChanged;

    public LauncherUpdates(Action<string> log, string? bootstrapPath = null, string? runtime = null, string? currentVersion = null,
        string? updatesDirectory = null, HttpMessageHandler? handler = null, string manifestUrl = LauncherUpdate.ManifestUrl, string assetBaseUrl = LauncherUpdate.AssetBaseUrl)
    {
        this.log = log; this.handler = handler; this.manifestUrl = manifestUrl; this.assetBaseUrl = assetBaseUrl;
        BootstrapPath = bootstrapPath ?? Environment.GetEnvironmentVariable("CLIENTLAB_BOOTSTRAP_PATH");
        Runtime = runtime ?? Environment.GetEnvironmentVariable("CLIENTLAB_RUNTIME_ID") ?? "";
        CurrentVersion = currentVersion ?? Environment.GetEnvironmentVariable("CLIENTLAB_LAUNCHER_VERSION") ?? "";
        Updates = updatesDirectory ?? LauncherUpdate.UpdatesDirectory(LauncherUpdate.CacheDirectory);
    }

    public bool Enabled => !string.IsNullOrEmpty(BootstrapPath) && File.Exists(BootstrapPath) && Runtime.Length > 0 &&
        LauncherUpdate.TryParseVersion(CurrentVersion, out _);

    /// <summary>Checks now, then every few hours, never throwing: network failures only reach the Activity log.</summary>
    public void StartBackground()
    {
        if (!Enabled) return;
        _ = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(15), stopped.Token); // Startup, sign-in and game checks go first.
                while (!stopped.IsCancellationRequested)
                {
                    await CheckAsync(stopped.Token);
                    await Task.Delay(Interval, stopped.Token);
                }
            }
            catch (OperationCanceledException) { }
        });
    }

    /// <summary>Stages the latest release when it is newer than this launcher. Returns the staged update, if any.</summary>
    public async Task<StagedUpdate?> CheckAsync(CancellationToken token)
    {
        if (!Enabled) return null;
        try
        {
            if (Staged is null && LauncherUpdate.FindStaged(Updates, CurrentVersion, Runtime) is { } existing) Publish(existing);
            using var http = handler is null ? new HttpClient() : new HttpClient(handler, disposeHandler: false);
            http.DefaultRequestHeaders.UserAgent.ParseAdd("NosCore.ClientLab/" + CurrentVersion);
            http.Timeout = TimeSpan.FromMinutes(30); // The download; the manifest has its own short deadline below.
            ReleaseManifest manifest;
            using (var quick = CancellationTokenSource.CreateLinkedTokenSource(token))
            {
                quick.CancelAfter(TimeSpan.FromSeconds(20));
                manifest = ReleaseManifest.Parse(await http.GetStringAsync(manifestUrl, quick.Token));
            }
            if (LauncherUpdate.CompareVersions(manifest.Version, CurrentVersion) <= 0) return Staged;
            if (Staged is { } staged && LauncherUpdate.CompareVersions(staged.Version, manifest.Version) >= 0) return Staged;
            if (!manifest.Assets.TryGetValue(Runtime, out var asset))
            {
                log($"Launcher {manifest.Version} is available, but not for {Runtime}.");
                return Staged;
            }
            log($"Downloading launcher {manifest.Version} in the background…");
            Directory.CreateDirectory(Updates);
            var part = Path.Combine(Updates, asset.Name + ".part");
            using (var response = await http.GetAsync(assetBaseUrl + "v" + Uri.EscapeDataString(manifest.Version) + "/" + Uri.EscapeDataString(asset.Name), HttpCompletionOption.ResponseHeadersRead, token))
            {
                response.EnsureSuccessStatusCode();
                await using var output = File.Create(part);
                await response.Content.CopyToAsync(output, token);
            }
            var result = LauncherUpdate.Stage(Updates, part, manifest, Runtime);
            log($"Launcher {result.Version} is ready. It is used the next time ClientLab starts.");
            Publish(result);
            return result;
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { throw; }
        catch (Exception error)
        {
            log("Launcher update check skipped: " + error.Message);
            return Staged;
        }
    }

    private void Publish(StagedUpdate update)
    {
        Staged = update;
        StagedChanged?.Invoke(update);
    }

    public void Dispose() { stopped.Cancel(); stopped.Dispose(); }
}
