using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// <c>ClientLab --serve</c>: the game server alone, without the launcher window, for Playwright and other browser
/// automation. It serves an existing build (a launcher-prepared runtime by default, or a dev build) with the same
/// <see cref="GameHost"/> the desktop window uses, and prints <c>CLIENTLAB_URL=&lt;url&gt;</c> once it listens.
/// </summary>
public sealed record ServeOptions
{
    public const int DefaultPort = 8077;
    public string? Game { get; init; }
    public string? Build { get; init; }
    public string? ClientExe { get; init; }
    public int Port { get; init; } = DefaultPort;
    public string Provider { get; init; } = "gameforge";
    public string? User { get; init; }
    public string? Password { get; init; }
    public string AuthUrl { get; init; } = "https://127.0.0.1:7001";
    public string? AuthCode { get; init; }
    public string RelayHost { get; init; } = "";
    public string Remap { get; init; } = "";
    public string? InstallationId { get; init; }
    public string? IdentityFile { get; init; }
    public string? UserAgent { get; init; }
    public bool PacketLog { get; init; }
    public bool Open { get; init; }

    private static readonly HashSet<string> Flags = ["--serve", "--packet-log", "--open"];
    private static readonly HashSet<string> Valued = ["--game", "--build", "--client-exe", "--port", "--provider", "--auth-provider", "--user",
        "--password", "--auth-url", "--auth-code", "--host", "--remap", "--gf-installation-id", "--gf-identity-file", "--gf-user-agent",
        "--data-dir", "--source-root"];

    /// <summary>Parses the <c>--serve</c> command line. <paramref name="environment"/> supplies CLIENTLAB_AUTH_CODE,
    /// TNT_INSTALLATION_ID, GF_IDENTITY_FILE and GF_USER_AGENT, as the Python dev server read them.</summary>
    public static ServeOptions Parse(IReadOnlyList<string> args, Func<string, string?> environment)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        var remaps = new List<string>();
        var flags = new HashSet<string>(StringComparer.Ordinal);
        for (var index = 0; index < args.Count; index++)
        {
            var name = args[index];
            if (Flags.Contains(name)) { flags.Add(name); continue; }
            if (!Valued.Contains(name)) throw new ArgumentException($"Unknown --serve option {name}.");
            if (index + 1 >= args.Count || args[index + 1].StartsWith("--", StringComparison.Ordinal))
                throw new ArgumentException($"{name} needs a value.");
            var value = args[++index];
            if (name == "--remap") remaps.Add(value);
            else values[name == "--auth-provider" ? "--provider" : name] = value;
        }
        string? Value(string name) => values.TryGetValue(name, out var value) && value.Length > 0 ? value : null;
        string? Env(string name) => environment(name) is { Length: > 0 } value ? value : null;

        var port = DefaultPort;
        if (Value("--port") is { } portText && (!int.TryParse(portText, out port) || port is < 0 or > 65535))
            throw new ArgumentException("--port must be between 0 and 65535 (0 picks an available port).");
        var user = Value("--user");
        var password = Value("--password");
        if (user is not null && password is null) throw new ArgumentException("--user needs --password.");
        var provider = Value("--provider") ?? (user is not null || Value("--auth-code") is not null ? "noscore" : "gameforge");
        if (provider is not ("noscore" or "gameforge")) throw new ArgumentException("--provider must be noscore or gameforge.");
        if (provider == "gameforge" && user is not null)
            throw new ArgumentException("--user/--password sign in to a NosCore server; Gameforge sign-in happens in the page.");
        foreach (var mapping in remaps)
        {
            var pair = mapping.Split('=');
            if (pair.Length != 2 || !int.TryParse(pair[0], out var from) || !int.TryParse(pair[1], out var to) || from is < 1 or > 65535 || to is < 1 or > 65535)
                throw new ArgumentException($"--remap expects FROM=TO ports, got {mapping}.");
        }
        if ((remaps.Count > 0 || Value("--host") is not null) && provider != "noscore")
            throw new ArgumentException("--host and --remap redirect a private server's connections; use them with --provider noscore.");
        if (Value("--build") is not null && Value("--game") is null)
            throw new ArgumentException("--build needs --game: a dev build has no prepared game folder.");
        return new ServeOptions
        {
            Game = Value("--game"), Build = Value("--build"), ClientExe = Value("--client-exe"), Port = port, Provider = provider,
            User = user, Password = password, AuthUrl = Value("--auth-url") ?? "https://127.0.0.1:7001",
            AuthCode = Value("--auth-code") ?? Env("CLIENTLAB_AUTH_CODE"),
            RelayHost = Value("--host") ?? "", Remap = string.Join(',', remaps),
            InstallationId = Value("--gf-installation-id") ?? Env("TNT_INSTALLATION_ID"),
            IdentityFile = Value("--gf-identity-file") ?? Env("GF_IDENTITY_FILE"),
            UserAgent = Value("--gf-user-agent") ?? Env("GF_USER_AGENT"),
            PacketLog = flags.Contains("--packet-log"), Open = flags.Contains("--open"),
        };
    }

    /// <summary>The build GameHost serves: the given game/build folders, or the launcher's prepared runtime.</summary>
    public JsonObject ResolveActive(string dataDirectory)
    {
        string game, build, executable;
        if (Game is null)
        {
            var activePath = Path.Combine(dataDirectory, "active.json");
            if (!File.Exists(activePath))
                throw new InvalidOperationException($"No prepared game in {dataDirectory}. Prepare it in the launcher first, or pass --game and --build.");
            var active = JsonNode.Parse(File.ReadAllText(activePath))!.AsObject();
            game = active["client"]?.GetValue<string>() ?? throw new InvalidDataException("active.json has no client folder.");
            build = active["runtime"]?.GetValue<string>() ?? throw new InvalidDataException("active.json has no runtime folder.");
            executable = ClientExe ?? active["executable"]?.GetValue<string>() ?? throw new InvalidDataException("active.json has no executable.");
        }
        else
        {
            game = Game;
            build = Build ?? Path.Combine(Environment.GetEnvironmentVariable("NOSRECOMP_WORK") is { Length: > 0 } work ? work
                : OperatingSystem.IsWindows() ? @"C:\tools\re\work" : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "re/work"), "wasm", "web");
            // The executable the build was recompiled from is mounted as NosCore.exe, as in the desktop launcher.
            executable = ClientExe ?? new[] { "NosCore.exe", "NostaleClientX.exe" }.Select(name => Path.Combine(game, name)).FirstOrDefault(File.Exists)
                ?? throw new InvalidOperationException($"No NosCore.exe or NostaleClientX.exe in {game}; pass --client-exe.");
        }
        if (!Directory.Exists(game)) throw new InvalidOperationException($"Game folder not found: {game}");
        if (!File.Exists(Path.Combine(build, "nostale.js"))) throw new InvalidOperationException($"No browser build (nostale.js) in {build}.");
        if (!File.Exists(executable)) throw new InvalidOperationException($"Client executable not found: {executable}");
        return new JsonObject { ["client"] = Path.GetFullPath(game), ["runtime"] = Path.GetFullPath(build), ["executable"] = Path.GetFullPath(executable) };
    }

    /// <summary>GameHost settings: the launcher's saved settings (Gameforge identity, packet plugin) under the options.</summary>
    public JsonObject Settings(string dataDirectory)
    {
        var settingsPath = Path.Combine(dataDirectory, "settings.json");
        var settings = File.Exists(settingsPath) ? JsonNode.Parse(File.ReadAllText(settingsPath))?.AsObject() ?? [] : [];
        settings["provider"] = Provider;
        settings["private_server_host"] = RelayHost;
        settings["private_port_remap"] = Remap;
        settings["packet_logger"] = PacketLog ? "true" : "false";
        if (!PacketLog) settings["packet_plugin"] = "";
        if (InstallationId is not null) settings["installation_id"] = InstallationId;
        if (IdentityFile is not null) settings["identity_file"] = IdentityFile;
        return settings;
    }

    public GameHostOptions HostOptions()
    {
        Func<CancellationToken, Task<string>>? code = null;
        if (AuthCode is { } fixedCode) code = _ => Task.FromResult(fixedCode);
        else if (User is { } user)
            // A fresh code per page load, like the NosCore launcher: codes are single-use.
            code = _ => PrivateServerAuthentication.SignInAsync(AuthUrl, user, Password!);
        return new GameHostOptions(Port, code, UserAgent);
    }
}

public static class ServeCommand
{
    public static async Task<int> RunAsync(string[] args, string dataDirectory)
    {
        ServeOptions options;
        JsonObject active, settings;
        try
        {
            options = ServeOptions.Parse(args, Environment.GetEnvironmentVariable);
            active = options.ResolveActive(dataDirectory);
            settings = options.Settings(dataDirectory);
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or InvalidDataException or IOException)
        {
            Console.Error.WriteLine("clientlab --serve: " + error.Message);
            return 2;
        }
        var root = SourceAssets.Resolve();
        await using var host = await GameHost.StartAsync(root, active, settings, line => Console.WriteLine(line),
            new GameSettingsStore(dataDirectory), options.HostOptions());
        // One machine-readable line for scripts, then a human hint.
        Console.WriteLine("CLIENTLAB_URL=" + host.Url);
        Console.WriteLine(options.Provider == "gameforge" && options.AuthCode is null
            ? $"Game: {active["client"]}. Open {host.Url}?login=gameforge to sign in with Gameforge, or pass ?auth=<code>."
            : $"Game: {active["client"]}. Open {host.Url}");
        if (options.Open)
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(host.Url + (options.Provider == "gameforge" && options.AuthCode is null ? "?login=gameforge" : "")) { UseShellExecute = true });
        using var stop = new CancellationTokenSource();
        ConsoleCancelEventHandler handler = (_, e) => { e.Cancel = true; stop.Cancel(); };
        Console.CancelKeyPress += handler;
        try { await Task.Delay(Timeout.Infinite, stop.Token); } catch (OperationCanceledException) { }
        finally { Console.CancelKeyPress -= handler; }
        return 0;
    }
}
