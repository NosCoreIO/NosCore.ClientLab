using System.Collections.Concurrent;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.Win32;

namespace NosCore.ClientLab.Launcher.Services;

public sealed class GameforgeAuthentication : IDisposable
{
    private const string GameId = "dd4e22d6-00d1-44b9-8126-d8b40e0cd7c9";
    private readonly HttpClient http;
    private readonly string installation;
    private readonly string identityFile;
    private readonly IGameforgeIdentity? identityProvider;
    public string InstallationId => installation;
    private readonly ConcurrentDictionary<string, LoginSession> sessions = new();
    private readonly ConcurrentDictionary<string, Challenge> challenges = new();
    private sealed record LoginSession(string Token, JsonArray Accounts, DateTimeOffset Created)
    { public string? PendingGsid { get; set; } public string? PendingAccount { get; set; } public bool Retained { get; set; } }
    private sealed record Challenge(string Email, string Password, string Type, string? Id, string UserAgent, DateTimeOffset Created)
    { public string? ImageVersion { get; set; } public string? OtpCode { get; init; } }

    public GameforgeAuthentication(JsonObject settings, HttpMessageHandler? handler = null, IGameforgeIdentity? identity = null)
    {
        http = handler is null ? new HttpClient() : new HttpClient(handler);
        http.Timeout = TimeSpan.FromSeconds(30);
        identityFile = settings["identity_file"]?.GetValue<string>() ?? "";
        identityProvider = identity;
        installation = settings["installation_id"]?.GetValue<string>() ?? "";
        if (identity is not null) installation = identity.InstallationId;
        else if (identityFile.Length > 0) installation = new GameforgeIdentity(identityFile, installation).InstallationId;
        if (installation.Length == 0 && OperatingSystem.IsWindows())
        {
            using var machine = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Gameforge4d\GameforgeClient\MainApp");
            using var user = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Gameforge4d\GameforgeClient\MainApp");
            installation = machine?.GetValue("InstallationId") as string ?? user?.GetValue("InstallationId") as string ?? "";
        }
        if (!Guid.TryParse(installation, out _)) throw new InvalidOperationException("Import a Gameforge identity in Settings, or provide a registered installation ID.");
    }

    public sealed class SessionExpiredException() : InvalidOperationException("Your Gameforge sign-in expired. Please sign in again.");

    public sealed class GameforgeException(int status, JsonNode? body, string? challengeId = null, string operation = "sign-in") : Exception($"Gameforge {operation} returned HTTP {status}.")
    { public int Status { get; } = status; public JsonNode? Body { get; } = body; public string? ChallengeId { get; } = challengeId; public string Operation { get; } = operation; }

    private async Task<JsonNode> RequestAsync(string path, string userAgent, HttpMethod? method = null, JsonNode? body = null, string? token = null)
    {
        var versioned = path.StartsWith("v2/") ? path : "v1/" + path;
        using var request = new HttpRequestMessage(method ?? HttpMethod.Get, "https://spark.gameforge.com/api/" + versioned);
        request.Headers.TryAddWithoutValidation("User-Agent", userAgent);
        request.Headers.Accept.ParseAdd("application/json");
        request.Headers.TryAddWithoutValidation("TNT-Installation-Id", installation);
        if (path.StartsWith("v2/")) request.Headers.TryAddWithoutValidation("GF-Installation-Id", installation);
        if (token is not null) request.Headers.TryAddWithoutValidation("Authorization", "Bearer " + token);
        if (body is not null)
        {
            // Match the native launcher's fixed-length UTF-8 JSON request framing.
            request.Content = new StringContent(body.ToJsonString(), Encoding.UTF8, "application/json");
            request.Content.Headers.ContentType!.CharSet = null;
        }
        using var response = await http.SendAsync(request);
        var text = await response.Content.ReadAsStringAsync();
        JsonNode? value;
        try { value = JsonNode.Parse(text.TrimStart('\uFEFF')); } catch { value = new JsonObject { ["message"] = text[..Math.Min(1200, text.Length)] }; }
        if (!response.IsSuccessStatusCode)
            throw new GameforgeException((int)response.StatusCode, value, response.Headers.TryGetValues("gf-challenge-id", out var ids) ? ids.First().Split(';')[0].Trim() : null, path.StartsWith("v2/") ? "sign-in" : path);
        return value ?? new JsonObject();
    }

    private void Expire()
    {
        var cutoff = DateTimeOffset.UtcNow.AddMinutes(-15);
        // A session kept for repeated Play lives as long as Gameforge accepts its token.
        foreach (var item in sessions.Where(item => !item.Value.Retained && item.Value.Created < cutoff)) sessions.TryRemove(item.Key, out _);
        foreach (var item in challenges.Where(item => item.Value.Created < cutoff)) challenges.TryRemove(item.Key, out _);
    }

    public async Task<JsonObject> LoginAsync(JsonObject payload, string userAgent, string? flow = null)
    {
        Expire();
        var email = payload["email"]?.GetValue<string>() ?? ""; var password = payload["password"]?.GetValue<string>() ?? "";
        if (email.Length == 0 || password.Length == 0) throw new InvalidOperationException("Enter your Gameforge email and password.");
        var credentials = new JsonObject { ["email"] = email, ["password"] = password, ["locale"] = "en-GB", ["blackbox"] = payload["blackbox"]?.GetValue<string>() ?? "" };
        if (payload["otpCode"] is not null) credentials["otpCode"] = payload["otpCode"]!.DeepClone();
        try
        {
            var result = await RequestAsync("v2/authProviders/credentials/sessions", userAgent, HttpMethod.Post, credentials);
            var token = result["token"]?.GetValue<string>() ?? throw new InvalidOperationException("Gameforge did not return a session token.");
            var opened = await OpenSessionAsync(token, userAgent, retained: false);
            if (flow is not null) challenges.TryRemove(flow, out _);
            return opened;
        }
        catch (GameforgeException error)
        {
            var types = error.Body?["errorTypes"]?.AsArray().Select(v => v?.ToString()).ToArray() ?? [];
            if (types.Contains("OTP_REQUIRED") || types.Contains("CHALLENGE_REQUIRED"))
            {
                flow ??= Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(24));
                var type = types.Contains("OTP_REQUIRED") ? "otp" : "captcha";
                if (type == "captcha" && string.IsNullOrEmpty(error.ChallengeId)) throw;
                var pending = new Challenge(email, password, type, error.ChallengeId, userAgent, DateTimeOffset.UtcNow) { OtpCode = payload["otpCode"]?.GetValue<string>() };
                challenges[flow] = pending;
                var challenge = new JsonObject { ["challenge"] = type, ["flowId"] = flow };
                if (type == "captcha")
                {
                    var info = await ChallengeRequestAsync("https://challenge.gameforge.com/challenge/" + Uri.EscapeDataString(pending.Id!), pending.UserAgent, "challenge details");
                    var challengeType = JsonNode.Parse(await info.Content.ReadAsStringAsync())?["type"]?.GetValue<string>();
                    info.Dispose();
                    if (challengeType == "gf-pow-captcha")
                    {
                        challenges[flow] = pending with { Type = "pow" };
                        return new JsonObject { ["challenge"] = "pow", ["flowId"] = flow, ["challengeId"] = pending.Id };
                    }
                    if (challengeType != "gf-image-drop-captcha")
                        throw new InvalidOperationException($"Gameforge requested {challengeType ?? "an unknown verification type"}. This is not an image CAPTCHA; complete this check in the official launcher and retry.");
                    challenge["images"] = await CaptchaImagesAsync(pending);
                }
                return challenge;
            }
            throw;
        }
    }

    /// <summary>Continues a sign-in from a token kept since an earlier one, as the official client does after a restart.</summary>
    public async Task<JsonObject> ResumeAsync(string token, string userAgent)
    {
        Expire();
        try { return await OpenSessionAsync(token, userAgent, retained: true); }
        catch (GameforgeException error) when (error.Status is 401 or 403) { throw new SessionExpiredException(); }
    }

    public string? TokenOf(string sessionId) => sessions.TryGetValue(sessionId, out var session) ? session.Token : null;

    private async Task<JsonObject> OpenSessionAsync(string token, string userAgent, bool retained)
    {
        await RequestAsync("user/me", userAgent, token: token);
        var linked = await RequestAsync("user/accounts", userAgent, token: token); var accounts = new JsonArray();
        if (linked is JsonObject dictionary)
        {
            foreach (var (key, item) in dictionary)
                if (item is JsonObject account && account["guls"]?["game"]?.GetValue<string>() == "nostale")
                    accounts.Add(new JsonObject { ["id"] = account["id"]?.ToString() ?? key, ["name"] = account["displayName"]?.ToString() ?? account["name"]?.ToString() ?? key });
        }
        else if (linked is JsonArray array)
            foreach (var element in array)
            {
                var id = element?["platformGameAccountId"]?.ToString() ?? element?["id"]?.ToString() ?? element?["accountId"]?.ToString();
                if (id is not null) accounts.Add(new JsonObject { ["id"] = id, ["name"] = element?["displayName"]?.ToString() ?? id });
            }
        if (accounts.Count == 0) throw new InvalidOperationException("No linked NosTale accounts were returned.");
        var idSession = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32));
        sessions[idSession] = new(token, accounts, DateTimeOffset.UtcNow) { Retained = retained };
        return new JsonObject { ["sessionId"] = idSession, ["accounts"] = accounts.DeepClone() };
    }

    public async Task<JsonObject> ChallengeAsync(JsonObject payload)
    {
        Expire(); var flow = payload["flowId"]?.GetValue<string>() ?? "";
        if (!challenges.TryGetValue(flow, out var pending)) throw new InvalidOperationException("Sign-in expired. Please sign in again.");
        if (pending.Type == "pow" && payload["verificationComplete"]?.GetValue<bool>() != true)
            throw new InvalidOperationException("Complete Gameforge's proof-of-work verification before continuing.");
        if (pending.Type == "captcha")
        {
            var answer = payload["answer"]?.GetValue<int>() ?? -1;
            if (answer is < 0 or > 3) throw new InvalidOperationException("Choose one of the four CAPTCHA images.");
            if (payload["captchaVersion"]?.GetValue<string>() is { } version && version != pending.ImageVersion)
                return await CaptchaRetryAsync(flow, pending, "The challenge changed. Please use the new images.");
            using var response = await ChallengeRequestAsync(CaptchaUrl(pending.Id!), pending.UserAgent, "CAPTCHA answer", new JsonObject { ["answer"] = answer }, allowRejectedAnswer: true);
            if ((int)response.StatusCode == 422)
                return await CaptchaRetryAsync(flow, pending, "Gameforge rejected the submission (HTTP 422). The challenge has been refreshed; please try the new images.");
            var result = JsonNode.Parse(await response.Content.ReadAsStringAsync());
            if (result?["status"]?.GetValue<string>() != "solved")
                return await CaptchaRetryAsync(flow, pending, "That answer was not accepted. Please try the new images.");
        }
        return await LoginAsync(new JsonObject { ["email"] = pending.Email, ["password"] = pending.Password,
            ["blackbox"] = payload["blackbox"]?.DeepClone(), ["otpCode"] = pending.Type == "otp" ? payload["otpCode"]?.DeepClone() : JsonValue.Create(pending.OtpCode) }, pending.UserAgent, flow);
    }

    private static string CaptchaUrl(string id) => "https://image-drop-challenge.gameforge.com/challenge/" + Uri.EscapeDataString(id) + "/en-GB";
    private async Task<JsonObject> CaptchaRetryAsync(string flow, Challenge pending, string message) =>
        new() { ["challenge"] = "captcha", ["flowId"] = flow, ["images"] = await CaptchaImagesAsync(pending), ["message"] = message };

    private async Task<HttpResponseMessage> ChallengeRequestAsync(string url, string userAgent, string operation, JsonObject? body = null, bool image = false, bool allowRejectedAnswer = false)
    {
        using var request = new HttpRequestMessage(body is null ? HttpMethod.Get : HttpMethod.Post, url);
        request.Headers.TryAddWithoutValidation("User-Agent", userAgent);
        request.Headers.TryAddWithoutValidation("Origin", "spark://www.gameforge.com");
        request.Headers.TryAddWithoutValidation("Accept", image ? "image/webp,image/apng,image/*,*/*;q=0.8" : "*/*");
        if (body is not null) request.Content = JsonContent.Create(body);
        var response = await http.SendAsync(request);
        if (!response.IsSuccessStatusCode && !(allowRejectedAnswer && (int)response.StatusCode == 422))
        {
            var text = await response.Content.ReadAsStringAsync();
            JsonNode? detail;
            try { detail = JsonNode.Parse(text); } catch { detail = null; }
            var status = (int)response.StatusCode;
            response.Dispose();
            throw new GameforgeException(status, detail, operation: operation);
        }
        return response;
    }

    private async Task<JsonObject> CaptchaImagesAsync(Challenge pending)
    {
        var url = CaptchaUrl(pending.Id!);
        using var responseMetadata = await ChallengeRequestAsync(url, pending.UserAgent, "CAPTCHA metadata");
        var metadata = JsonNode.Parse(await responseMetadata.Content.ReadAsStringAsync())!;
        var stamp = metadata["lastUpdated"]?.ToString() ?? throw new InvalidDataException("Gameforge returned a CAPTCHA without an image version.");
        pending.ImageVersion = stamp;
        var images = new JsonObject { ["version"] = stamp };
        foreach (var (key, endpoint) in new[] { ("prompt", "text"), ("icons", "drag-icons"), ("target", "drop-target") })
        {
            using var response = await ChallengeRequestAsync(url + "/" + endpoint + "?" + Uri.EscapeDataString(stamp), pending.UserAgent, "CAPTCHA " + endpoint + " image", image: true);
            images[key] = "data:" + (response.Content.Headers.ContentType?.MediaType ?? "image/png") + ";base64," + Convert.ToBase64String(await response.Content.ReadAsByteArrayAsync());
        }
        return images;
    }

    public async Task<JsonObject> LaunchAsync(JsonObject payload, string userAgent, bool codeOnly = false, bool retainSession = false)
    {
        Expire(); var id = payload["sessionId"]?.GetValue<string>() ?? "";
        if (!sessions.TryGetValue(id, out var session)) throw new SessionExpiredException();
        try { return await LaunchAsync(id, session, payload, userAgent, codeOnly, retainSession); }
        catch (GameforgeException error) when (error.Status == 401)
        {
            sessions.TryRemove(id, out _);
            throw new SessionExpiredException();
        }
    }

    private async Task<JsonObject> LaunchAsync(string id, LoginSession session, JsonObject payload, string userAgent, bool codeOnly, bool retainSession)
    {
        var account = payload["accountId"]?.GetValue<string>() ?? "";
        if (!session.Accounts.Any(item => item?["id"]?.GetValue<string>() == account)) throw new InvalidOperationException("Select one of your linked game accounts.");
        IGameforgeIdentity? identity = identityProvider ?? (identityFile.Length > 0 ? new GameforgeIdentity(identityFile, installation) : null);
        if (retainSession && (identity is null || codeOnly)) throw new InvalidOperationException("Persistent sign-in requires the launcher's identity provider.");
        string gsid; string blackbox;
        if (!codeOnly)
        {
            var gameSession = Guid.NewGuid().ToString(); gsid = gameSession + "-" + RandomNumberGenerator.GetInt32(1000, 9999);
            var iovation = await RequestAsync("auth/iovation", identity?.UserAgent ?? userAgent, HttpMethod.Post,
                new JsonObject { ["accountId"] = account, ["blackbox"] = identity is null ? payload["iovationBlackbox"]?.GetValue<string>() : await identity.CreateAsync(http), ["type"] = "play_now" }, session.Token);
            if (iovation["status"]?.GetValue<string>() != "ok") throw new InvalidOperationException("Gameforge did not accept identity verification.");
            await Task.Delay(1000);
            var extra = new JsonObject { ["features"] = new JsonArray(GameforgeLaunchFeatures.Collect()), ["installation"] = installation, ["session"] = gameSession };
            if (identity is null)
            {
                session.PendingGsid = gsid; session.PendingAccount = account;
                return new JsonObject { ["gsid"] = gsid, ["blackboxRequest"] = extra };
            }
            blackbox = await identity.CreateAsync(http, extra);
        }
        else
        {
            gsid = payload["gsid"]?.GetValue<string>() ?? "";
            if (session.PendingGsid != gsid || session.PendingAccount != account) throw new InvalidOperationException("Launch request expired. Please retry.");
            blackbox = payload["codeBlackbox"]?.GetValue<string>() ?? "";
            var fields = GameforgeIdentity.Decode(blackbox);
            if (fields.Count != 30 || fields[28]?["session"]?.GetValue<string>() != gsid[..gsid.LastIndexOf('-')] || fields[28]?["installation"]?.GetValue<string>() != installation)
                throw new InvalidOperationException("The identity script did not bind the launch session correctly.");
        }
        var metadata = JsonNode.Parse((await http.GetStringAsync("http://dl.tnt.gameforge.com/tnt/final-ms3/clientversioninfo.json")).TrimStart('\uFEFF'))!;
        var userAgentCode = CodeUserAgent("C" + metadata["version"]!.GetValue<string>(), installation, account);
        var result = await RequestAsync("auth/thin/codes", userAgentCode, HttpMethod.Post,
            new JsonObject { ["platformGameAccountId"] = account, ["gsid"] = gsid, ["blackbox"] = GameforgeIdentity.Encrypt(blackbox, gsid, account), ["gameId"] = GameId }, session.Token);
        var code = result["code"]?.GetValue<string>() ?? throw new InvalidOperationException("Gameforge did not return a launch code.");
        session.PendingGsid = session.PendingAccount = null;
        if (retainSession) session.Retained = true; else sessions.TryRemove(id, out _);
        return new JsonObject { ["code"] = code, ["installationId"] = installation };
    }

    // Gameforge's NoS0577 token is the UTF-8 code represented as hex. The NosCore
    // private-server shim accepts its raw GUID instead, so convert only at the
    // Gameforge-to-game boundary, not in the session/code API itself.
    public static string ClientTicket(string code) => Convert.ToHexString(Encoding.UTF8.GetBytes(code));

    public static string CodeUserAgent(string version, string installation, string account)
    {
        static string Hash(string value, bool sha256) => Convert.ToHexStringLower(sha256 ? SHA256.HashData(Encoding.UTF8.GetBytes(value)) : SHA1.HashData(Encoding.UTF8.GetBytes(value)));
        var first = installation.FirstOrDefault(char.IsAsciiDigit); var even = first == default || (first - '0') % 2 == 0;
        var certificate = even ? "99025da70af1ef39d2acd049018887ef5140daebc6f11d80461bcf8d02f2d36b" : "d68f9401f15791cc396d4d6af3b977bc58ad0002";
        var sum = Hash(certificate + Hash(version, !even) + Hash(installation, even) + Hash(account, !even), true);
        return $"Chrome/{version} ({account[..2]}{(even ? sum[..8] : sum[^8..])})";
    }
    public void Dispose() { http.Dispose(); sessions.Clear(); challenges.Clear(); }
}
