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
    private readonly HttpClient http = new() { Timeout = TimeSpan.FromSeconds(30) };
    private readonly string installation;
    private readonly string identityFile;
    private readonly ConcurrentDictionary<string, LoginSession> sessions = new();
    private readonly ConcurrentDictionary<string, Challenge> challenges = new();
    private sealed record LoginSession(string Token, JsonArray Accounts, DateTimeOffset Created)
    { public string? PendingGsid { get; set; } public string? PendingAccount { get; set; } }
    private sealed record Challenge(string Email, string Password, string Type, string? Id, string UserAgent, DateTimeOffset Created);

    public GameforgeAuthentication(JsonObject settings)
    {
        identityFile = settings["identity_file"]?.GetValue<string>() ?? "";
        installation = settings["installation_id"]?.GetValue<string>() ?? "";
        if (identityFile.Length > 0) installation = new GameforgeIdentity(identityFile, installation).InstallationId;
        if (installation.Length == 0 && OperatingSystem.IsWindows())
        {
            using var machine = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Gameforge4d\GameforgeClient\MainApp");
            using var user = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Gameforge4d\GameforgeClient\MainApp");
            installation = machine?.GetValue("InstallationId") as string ?? user?.GetValue("InstallationId") as string ?? "";
        }
        if (!Guid.TryParse(installation, out _)) throw new InvalidOperationException("Import a Gameforge identity in Settings, or provide a registered installation ID.");
    }

    public sealed class GameforgeException(int status, JsonNode? body, string? challengeId = null) : Exception($"Gameforge returned HTTP {status}: {body?.ToJsonString()}")
    { public int Status { get; } = status; public JsonNode? Body { get; } = body; public string? ChallengeId { get; } = challengeId; }

    private async Task<JsonNode> RequestAsync(string path, string userAgent, HttpMethod? method = null, JsonNode? body = null, string? token = null)
    {
        var versioned = path.StartsWith("v2/") ? path : "v1/" + path;
        using var request = new HttpRequestMessage(method ?? HttpMethod.Get, "https://spark.gameforge.com/api/" + versioned);
        request.Headers.TryAddWithoutValidation("User-Agent", userAgent);
        request.Headers.TryAddWithoutValidation("TNT-Installation-Id", installation);
        if (path.StartsWith("v2/")) request.Headers.TryAddWithoutValidation("GF-Installation-Id", installation);
        if (token is not null) request.Headers.TryAddWithoutValidation("Authorization", "Bearer " + token);
        if (body is not null) request.Content = JsonContent.Create(body);
        using var response = await http.SendAsync(request);
        var text = await response.Content.ReadAsStringAsync();
        JsonNode? value;
        try { value = JsonNode.Parse(text.TrimStart('\uFEFF')); } catch { value = new JsonObject { ["message"] = text[..Math.Min(1200, text.Length)] }; }
        if (!response.IsSuccessStatusCode)
            throw new GameforgeException((int)response.StatusCode, value, response.Headers.TryGetValues("gf-challenge-id", out var ids) ? ids.First().Split(';')[0].Trim() : null);
        return value ?? new JsonObject();
    }

    private void Expire()
    {
        var cutoff = DateTimeOffset.UtcNow.AddMinutes(-15);
        foreach (var item in sessions.Where(item => item.Value.Created < cutoff)) sessions.TryRemove(item.Key, out _);
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
            await RequestAsync("user/me", userAgent, token: token);
            var linked = await RequestAsync("user/accounts", userAgent, token: token); var accounts = new JsonArray();
            if (linked is JsonObject dictionary)
                foreach (var (key, item) in dictionary)
                    if (item is JsonObject account && account["guls"]?["game"]?.GetValue<string>() == "nostale")
                        accounts.Add(new JsonObject { ["id"] = account["id"]?.ToString() ?? key, ["name"] = account["displayName"]?.ToString() ?? account["name"]?.ToString() ?? key });
            else if (linked is JsonArray array)
                foreach (var element in array)
                {
                    var id = element?["platformGameAccountId"]?.ToString() ?? element?["id"]?.ToString() ?? element?["accountId"]?.ToString();
                    if (id is not null) accounts.Add(new JsonObject { ["id"] = id, ["name"] = element?["displayName"]?.ToString() ?? id });
                }
            if (accounts.Count == 0) throw new InvalidOperationException("No linked NosTale accounts were returned.");
            if (flow is not null) challenges.TryRemove(flow, out _);
            var idSession = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32));
            sessions[idSession] = new(token, accounts, DateTimeOffset.UtcNow);
            return new JsonObject { ["sessionId"] = idSession, ["accounts"] = accounts.DeepClone() };
        }
        catch (GameforgeException error)
        {
            var types = error.Body?["errorTypes"]?.AsArray().Select(v => v?.ToString()).ToArray() ?? [];
            if (types.Contains("OTP_REQUIRED") || types.Contains("CHALLENGE_REQUIRED"))
            {
                flow ??= Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(24));
                var type = types.Contains("OTP_REQUIRED") ? "otp" : "captcha";
                if (type == "captcha" && string.IsNullOrEmpty(error.ChallengeId)) throw;
                challenges[flow] = new(email, password, type, error.ChallengeId, userAgent, DateTimeOffset.UtcNow);
                var challenge = new JsonObject { ["challenge"] = type, ["flowId"] = flow };
                if (type == "captcha") challenge["images"] = await CaptchaImagesAsync(error.ChallengeId!);
                return challenge;
            }
            throw;
        }
    }

    public async Task<JsonObject> ChallengeAsync(JsonObject payload)
    {
        Expire(); var flow = payload["flowId"]?.GetValue<string>() ?? "";
        if (!challenges.TryGetValue(flow, out var pending)) throw new InvalidOperationException("Sign-in expired. Please sign in again.");
        if (pending.Type == "captcha")
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, CaptchaUrl(pending.Id!))
            { Content = JsonContent.Create(new { answer = payload["answer"]?.GetValue<int>() ?? -1 }) };
            request.Headers.TryAddWithoutValidation("User-Agent", pending.UserAgent);
            request.Headers.TryAddWithoutValidation("Origin", "spark://www.gameforge.com");
            using var response = await http.SendAsync(request); response.EnsureSuccessStatusCode();
            var result = JsonNode.Parse(await response.Content.ReadAsStringAsync());
            if (result?["status"]?.GetValue<string>() != "solved")
                return new JsonObject { ["challenge"] = "captcha", ["flowId"] = flow, ["images"] = await CaptchaImagesAsync(pending.Id!), ["message"] = "That answer was not accepted. Try again." };
        }
        return await LoginAsync(new JsonObject { ["email"] = pending.Email, ["password"] = pending.Password,
            ["blackbox"] = payload["blackbox"]?.DeepClone(), ["otpCode"] = pending.Type == "otp" ? payload["otpCode"]?.DeepClone() : null }, pending.UserAgent, flow);
    }

    private static string CaptchaUrl(string id) => "https://image-drop-challenge.gameforge.com/challenge/" + Uri.EscapeDataString(id) + "/en-GB";
    private async Task<JsonObject> CaptchaImagesAsync(string id)
    {
        var url = CaptchaUrl(id); var metadata = JsonNode.Parse(await http.GetStringAsync(url))!; var images = new JsonObject();
        foreach (var (key, endpoint) in new[] { ("prompt", "text"), ("icons", "drag-icons"), ("target", "drop-target") })
        {
            using var response = await http.GetAsync(url + "/" + endpoint + "?" + metadata["lastUpdated"]);
            response.EnsureSuccessStatusCode();
            images[key] = "data:" + (response.Content.Headers.ContentType?.MediaType ?? "image/png") + ";base64," + Convert.ToBase64String(await response.Content.ReadAsByteArrayAsync());
        }
        return images;
    }

    public async Task<JsonObject> LaunchAsync(JsonObject payload, string userAgent, bool codeOnly = false)
    {
        Expire(); var id = payload["sessionId"]?.GetValue<string>() ?? ""; var account = payload["accountId"]?.GetValue<string>() ?? "";
        if (!sessions.TryGetValue(id, out var session)) throw new InvalidOperationException("Sign-in expired. Please sign in again.");
        if (!session.Accounts.Any(item => item?["id"]?.GetValue<string>() == account)) throw new InvalidOperationException("Select one of your linked game accounts.");
        GameforgeIdentity? identity = identityFile.Length > 0 ? new(identityFile, installation) : null;
        string gsid; string blackbox;
        if (!codeOnly)
        {
            var gameSession = Guid.NewGuid().ToString(); gsid = gameSession + "-" + RandomNumberGenerator.GetInt32(1000, 9999);
            var iovation = await RequestAsync("auth/iovation", identity?.UserAgent ?? userAgent, HttpMethod.Post,
                new JsonObject { ["accountId"] = account, ["blackbox"] = identity is null ? payload["iovationBlackbox"]?.GetValue<string>() : await identity.CreateAsync(http), ["type"] = "play_now" }, session.Token);
            if (iovation["status"]?.GetValue<string>() != "ok") throw new InvalidOperationException("Gameforge did not accept identity verification.");
            await Task.Delay(1000);
            var extra = new JsonObject { ["features"] = new JsonArray((long)RandomNumberGenerator.GetInt32(1, int.MaxValue) * 2), ["installation"] = installation, ["session"] = gameSession };
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
        sessions.TryRemove(id, out _);
        return new JsonObject { ["code"] = code, ["installationId"] = installation };
    }

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
