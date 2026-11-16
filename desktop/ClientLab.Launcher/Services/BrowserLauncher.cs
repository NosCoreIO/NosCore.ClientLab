using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>Browser transport for the same C# coordinator and sign-in flow used by the desktop.</summary>
public sealed class BrowserLauncher : IAsyncDisposable
{
    private readonly HttpListener listener = new();
    private readonly LauncherCoordinator app;
    private readonly SemaphoreSlim actions = new(1);
    private readonly CancellationTokenSource stopped = new();
    private readonly string secret = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
    private readonly string authority;
    private readonly object stateGate = new();
    private JsonObject signInState = new() { ["stage"] = "NeedCredentials" };
    private GameforgeBrowserIdentity? identity;
    private GameforgeSignIn? signIn;
    private string? accountId, privateUser, privatePassword;
    private TaskCompletionSource? visible;
    private string? collectionId;
    private volatile bool collecting;
    private readonly Task<NewsEntry[]> news;
    public string Url { get; }
    public string BrowserUrl => Url + "#" + secret;

    public BrowserLauncher(string root, string dataDirectory, int port = 0, bool prepare = true)
    {
        app = new LauncherCoordinator(root, dataDirectory);
        if (port == 0) { var socket = new TcpListener(IPAddress.Loopback, 0); socket.Start(); port = ((IPEndPoint)socket.LocalEndpoint).Port; socket.Stop(); }
        authority = "127.0.0.1:" + port; Url = "http://" + authority + "/";
        listener.Prefixes.Add(Url); listener.Start();
        news = OfficialNews.LoadAsync(dataDirectory);
        _ = ListenAsync();
        if (prepare) app.Start("startup");
    }

    private JsonObject Settings => app.Snapshot()["settings"]!.AsObject();
    private string Provider => Settings["provider"]!.GetValue<string>();
    private CredentialStore Vault => new(Provider == "gameforge" ? app.DataDirectory : Path.Combine(app.DataDirectory, "private-server"));
    private void SetSignIn(JsonObject value) { lock (stateGate) signInState = value; }
    private void RefreshSignIn()
    {
        var state = new JsonObject { ["stage"] = privateUser is not null || accountId is not null ? "Ready" : signIn?.Stage.ToString() ?? "NeedCredentials",
            ["account"] = privateUser, ["accounts"] = signIn?.Accounts.DeepClone(), ["message"] = signIn?.ChallengeMessage };
        if (signIn?.Captcha is { } captcha)
            state["captcha"] = new JsonObject { ["prompt"] = captcha.Prompt, ["icons"] = captcha.Icons, ["target"] = captcha.Target, ["version"] = signIn.CaptchaVersion };
        if (accountId is not null)
            state["account"] = signIn?.Accounts.FirstOrDefault(item => item?["id"]?.ToString() == accountId)?["name"]?.ToString() ?? accountId;
        SetSignIn(state);
    }

    private GameforgeBrowserIdentity EnsureIdentity()
    {
        if (identity is not null) return identity;
        identity = app.CreateBrowserIdentity();
        identity.PrepareCollection = async () =>
        {
            var signal = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            visible = signal; collectionId = Guid.NewGuid().ToString("N"); collecting = true;
            await signal.Task.WaitAsync(TimeSpan.FromSeconds(15), stopped.Token);
        };
        identity.CollectionFinished = () => { collecting = false; visible = null; };
        return identity;
    }

    private async Task ListenAsync()
    {
        try { while (!stopped.IsCancellationRequested) _ = HandleAsync(await listener.GetContextAsync()); }
        catch (Exception error) when (error is HttpListenerException or ObjectDisposedException) { }
    }

    private async Task HandleAsync(HttpListenerContext context)
    {
        try
        {
            var request = context.Request;
            if (request.UserHostName != authority) { await Reply(context, 403, new { error = "Invalid host." }); return; }
            var path = request.Url!.AbsolutePath;
            if (path == "/logo.png" && request.HttpMethod == "GET")
            {
                using var logo = typeof(BrowserLauncher).Assembly.GetManifestResourceStream("LauncherLogo.png")!;
                context.Response.ContentType = "image/png"; await logo.CopyToAsync(context.Response.OutputStream); return;
            }
            if (path == "/" && request.HttpMethod == "GET")
            {
                using var page = typeof(BrowserLauncher).Assembly.GetManifestResourceStream("BrowserLauncher.html")!;
                context.Response.ContentType = "text/html; charset=utf-8"; context.Response.Headers["Cache-Control"] = "no-store";
                context.Response.Headers["X-Content-Type-Options"] = "nosniff";
                await page.CopyToAsync(context.Response.OutputStream); return;
            }
            var supplied = request.Headers["X-ClientLab-Token"] ?? "";
            if (supplied.Length != secret.Length || !CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(supplied), Encoding.UTF8.GetBytes(secret)) ||
                request.Headers["Sec-Fetch-Site"] == "cross-site" || request.Headers["Origin"] is { } origin && origin != Url.TrimEnd('/'))
            { await Reply(context, 403, new { error = "Unauthorized launcher request." }); return; }
            if (request.HttpMethod == "GET")
            {
                object result = path switch
                {
                    "/api/state" => Snapshot(),
                    "/api/news" => await news,
                    "/api/game-options" => await app.ReadGameOptionsAsync(),
                    "/api/identity" => new { url = EnsureIdentity().Url },
                    "/api/credentials" => (object?)await Vault.ReadAsync() ?? new { Email = "", Password = "" },
                    _ => throw new InvalidOperationException("Unknown launcher endpoint.")
                };
                await Reply(context, 200, result); return;
            }
            if (request.HttpMethod != "POST" || request.ContentLength64 is < 0 or > 32768)
            { await Reply(context, 400, new { error = "Invalid launcher request." }); return; }
            using var reader = new StreamReader(request.InputStream);
            var text = await reader.ReadToEndAsync();
            if (text.Length > 32768) { await Reply(context, 413, new { error = "Request too large." }); return; }
            var payload = JsonNode.Parse(string.IsNullOrWhiteSpace(text) ? "{}" : text) as JsonObject ?? throw new InvalidDataException("Expected a JSON object.");
            if (path == "/api/identity-visible")
            {
                if (payload["id"]?.GetValue<string>() == collectionId) visible?.TrySetResult();
                await Reply(context, 200, new { ok = true }); return;
            }
            if (path == "/api/cancel") { app.Cancel(); await Reply(context, 200, new { ok = true }); return; }
            await actions.WaitAsync(stopped.Token);
            try { await Reply(context, 200, await ActionAsync(path, payload)); }
            finally { actions.Release(); }
        }
        catch (Exception error)
        { try { collecting = false; RefreshSignIn(); await Reply(context, 400, new { error = error.Message }); } catch { } }
        finally { try { context.Response.OutputStream.Close(); } catch { } }
    }

    public JsonObject Snapshot()
    {
        var state = app.Snapshot();
        lock (stateGate) state["signIn"] = signInState.DeepClone();
        state["collecting"] = collecting; state["collectionId"] = collectionId; state["identityUrl"] = identity?.Url;
        return state;
    }

    private async Task<object> ActionAsync(string path, JsonObject payload)
    {
        string Value(string key) => payload[key]?.GetValue<string>() ?? "";
        switch (path)
        {
            case "/api/game-options":
                await app.SaveGameOptionsAsync(new GameOptions(
                    payload["Music"]!.GetValue<bool>(), payload["SoundEffects"]!.GetValue<bool>(),
                    payload["SpatialSound"]!.GetValue<bool>(), payload["FullScreen"]!.GetValue<bool>()));
                return await app.ReadGameOptionsAsync();
            case "/api/settings":
                var previous = Provider;
                if (payload["provider"]?.GetValue<string>() is { } nextMode && nextMode != previous)
                    await app.CancelAndWaitAsync();
                app.SaveSettings(payload);
                if (previous != Provider) ResetSignIn();
                break;
            case "/api/task": app.Start(Value("action")); break;
            case "/api/cancel": app.Cancel(); break;
            case "/api/signout": await Vault.DeleteAsync(); ResetSignIn(); break;
            case "/api/forget": await Vault.DeleteAsync(); break;
            case "/api/signin":
                var username = Value("email"); var password = Value("password");
                if (payload["remember"]?.GetValue<bool>() == true) await Vault.SaveAsync(username, password); else await Vault.DeleteAsync();
                if (Provider == "noscore")
                {
                    await PrivateServerAuthentication.SignInAsync(Settings["private_auth_url"]!.GetValue<string>(), username, password);
                    privateUser = username; privatePassword = password;
                }
                else
                {
                    signIn?.Dispose(); accountId = null;
                    var collector = EnsureIdentity(); await collector.WaitUntilReadyAsync();
                    signIn = app.CreateSignIn(collector); await signIn.SignInAsync(username, password);
                }
                AutoSelectAccount(); break;
            case "/api/challenge":
                if (signIn is null) throw new InvalidOperationException("Sign in first.");
                if (signIn.Stage == SignInStage.NeedOtp) await signIn.SubmitOtpAsync(Value("otp"));
                else if (signIn.Stage == SignInStage.NeedCaptcha)
                {
                    if (Value("version") != signIn.CaptchaVersion) throw new InvalidOperationException("The CAPTCHA changed. Use the current images.");
                    await signIn.SubmitCaptchaAsync(payload["answer"]!.GetValue<int>());
                }
                else throw new InvalidOperationException("No verification answer is needed.");
                AutoSelectAccount(); break;
            case "/api/account":
                if (signIn is null || !signIn.Accounts.Any(item => item?["id"]?.ToString() == Value("id"))) throw new InvalidOperationException("Choose a linked game account.");
                accountId = Value("id"); break;
            case "/api/play":
                if (!app.Snapshot()["running"]!.GetValue<bool>())
                {
                    if (Provider == "noscore")
                    {
                        if (privateUser is null || privatePassword is null) throw new InvalidOperationException("Sign in to the private server first.");
                        app.UseLaunchCode(await PrivateServerAuthentication.SignInAsync(Settings["private_auth_url"]!.GetValue<string>(), privateUser, privatePassword), null);
                    }
                    else
                    {
                        if (signIn is null || accountId is null) throw new InvalidOperationException("Sign in and select your game account first.");
                        var expired = false;
                        try { await signIn.ChooseAccountAsync(accountId); }
                        catch (GameforgeAuthentication.SessionExpiredException) { expired = true; }
                        if (expired)
                        {
                            if (!await RenewSignInAsync(accountId)) throw new GameforgeAuthentication.SessionExpiredException();
                            await signIn!.ChooseAccountAsync(accountId);
                        }
                        app.UseLaunchCode(signIn.LaunchCode!, signIn.InstallationId);
                    }
                }
                return new { url = await app.LaunchAsync() };
            case "/api/close-game": await app.CloseGameAsync(); break;
            default: throw new InvalidOperationException("Unknown launcher action.");
        }
        RefreshSignIn(); return Snapshot();
    }

    /// <summary>Signs in again with the saved account after Gameforge's session lapsed; false leaves the sign-in stage
    /// (credentials, MFA or CAPTCHA) for the user.</summary>
    private async Task<bool> RenewSignInAsync(string account)
    {
        var saved = await Vault.ReadAsync();
        signIn?.Dispose(); signIn = null; accountId = null;
        if (saved is null) return false;
        var collector = EnsureIdentity(); await collector.WaitUntilReadyAsync();
        signIn = app.CreateSignIn(collector); await signIn.SignInAsync(saved.Email, saved.Password);
        if (signIn.Stage != SignInStage.ChooseAccount || !signIn.Accounts.Any(item => item?["id"]?.ToString() == account)) return false;
        accountId = account;
        return true;
    }

    private void AutoSelectAccount()
    { if (signIn?.Stage == SignInStage.ChooseAccount && signIn.Accounts.Count == 1) accountId = signIn.Accounts[0]!["id"]!.ToString(); }
    private void ResetSignIn()
    { signIn?.Dispose(); signIn = null; identity?.Dispose(); identity = null; accountId = privateUser = privatePassword = null; collecting = false; visible?.TrySetCanceled(); visible = null; collectionId = null; app.ClearLaunchCode(); RefreshSignIn(); }
    private static async Task Reply(HttpListenerContext context, int status, object value)
    {
        var bytes = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(value);
        context.Response.StatusCode = status; context.Response.ContentType = "application/json; charset=utf-8";
        context.Response.Headers["Cache-Control"] = "no-store"; context.Response.Headers["X-Content-Type-Options"] = "nosniff";
        context.Response.ContentLength64 = bytes.Length; await context.Response.OutputStream.WriteAsync(bytes);
    }
    public async ValueTask DisposeAsync()
    { stopped.Cancel(); listener.Close(); ResetSignIn(); await app.DisposeAsync(); stopped.Dispose(); }
}
