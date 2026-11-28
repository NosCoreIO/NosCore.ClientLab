using System.Net;
using System.Text;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class AuthFlowChecks
{
    public static async Task RunAsync()
    {
        Check(GameforgeAuthentication.ClientTicket("a-b") == "612D62", "Official NoS0577 tickets must encode UTF-8 code bytes as hex rather than send the private-server GUID format.");
        Check(!GameforgeLaunchFeatures.Decode(3453200689u) && !GameforgeLaunchFeatures.Decode(2281003686u), "Native feature reference values did not decode correctly.");
        foreach (var nonce in new uint[] { 0, 1, 2, 9, 10, 99, 100, 999, 1000, 3453200689, uint.MaxValue })
            foreach (var present in new[] { false, true })
                Check(GameforgeLaunchFeatures.Decode(GameforgeLaunchFeatures.Encode(nonce, present)) == present, "Native decimal-parity feature encoding failed.");
        var identity = new Identity(); var server = new Server(identity);
        using var auth = new GameforgeAuthentication(new JsonObject(), server, identity);
        var login = await auth.LoginAsync(new JsonObject { ["email"] = "test@example.invalid", ["password"] = "mock-only", ["blackbox"] = await identity.CreateAsync(new HttpClient()) }, identity.UserAgent);
        var payload = new JsonObject { ["sessionId"] = login["sessionId"]!.DeepClone(), ["accountId"] = Server.Account };
        var result = await auth.LaunchAsync(payload, identity.UserAgent);
        Check(result["code"]?.ToString() == "mock-code" && server.Codes == 1 && identity.Calls == 3, "The same official provider was not used for login, iovation and the launch code.");
        try { await auth.LaunchAsync(payload, identity.UserAgent); throw new Exception("A consumed session was reused."); }
        catch (InvalidOperationException error) when (error.Message.Contains("expired")) { }

        var rejecting = new Server(identity) { RejectIdentity = true };
        using var rejectedAuth = new GameforgeAuthentication(new JsonObject(), rejecting, identity);
        login = await rejectedAuth.LoginAsync(new JsonObject { ["email"] = "test@example.invalid", ["password"] = "mock-only" }, identity.UserAgent);
        payload["sessionId"] = login["sessionId"]!.DeepClone();
        try { await rejectedAuth.LaunchAsync(payload, identity.UserAgent); throw new Exception("Rejected identity was accepted."); }
        catch (GameforgeAuthentication.GameforgeException error) when (error.Status == 403 && error.Operation == "auth/iovation") { }
        Check(rejecting.Codes == 0, "A code was requested after failed device verification.");
        var persistentIdentity = new Identity(); var persistentServer = new Server(persistentIdentity);
        using var persistent = new GameforgeSignIn(new JsonObject(), persistentIdentity, persistentServer);
        await persistent.SignInAsync("test@example.invalid", "mock-only");
        await persistent.ChooseAccountAsync(Server.Account);
        await persistent.ChooseAccountAsync(Server.Account);
        Check(persistentServer.Codes == 2, "The trusted launcher could not mint a fresh code for a second launch.");
        persistentServer.TokenExpired = true;
        for (var attempt = 0; attempt < 2; attempt++)
            try { await persistent.ChooseAccountAsync(Server.Account); throw new Exception("An expired Gameforge token launched."); }
            catch (GameforgeAuthentication.SessionExpiredException) { }
        Check(persistentServer.Codes == 2, "A code was requested with an expired Gameforge token.");
        var refusingIdentity = new Identity(); var refusing = new Server(refusingIdentity) { RefuseCode = true };
        using var refused = new GameforgeSignIn(new JsonObject(), refusingIdentity, refusing);
        await refused.SignInAsync("test@example.invalid", "mock-only");
        try { await refused.ChooseAccountAsync(Server.Account); throw new Exception("A refused launch code launched."); }
        catch (InvalidOperationException error) when (error.Message.Contains("Not allowed to create code") && error.Message.Contains("Request mock-request.") && error.Message.Contains("official Gameforge Client")) { }

        var keptIdentity = new Identity(); var keptServer = new Server(keptIdentity);
        string kept;
        using (var first = new GameforgeSignIn(new JsonObject(), keptIdentity, keptServer))
        {
            await first.SignInAsync("test@example.invalid", "mock-only");
            kept = first.SessionToken!;
        }
        using (var restarted = new GameforgeSignIn(new JsonObject(), keptIdentity, keptServer))
        {
            await restarted.ResumeAsync(kept);
            Check(restarted.Stage == SignInStage.ChooseAccount && keptServer.SignIns == 1, "A kept token did not restore the account list without signing in again.");
            await restarted.ChooseAccountAsync(Server.Account);
            await restarted.ChooseAccountAsync(Server.Account);
            Check(keptServer.Codes == 2, "A restored sign-in could not launch more than once.");
        }
        keptServer.TokenRevoked = true;
        using (var revoked = new GameforgeSignIn(new JsonObject(), keptIdentity, keptServer))
            try { await revoked.ResumeAsync(kept); throw new Exception("A revoked Gameforge token was restored."); }
            catch (GameforgeAuthentication.SessionExpiredException) { }
        Console.WriteLine("PASS: official identity provider is shared through sign-in and account-bound launch, failed verification stops code creation, and consumed sessions cannot be reused.");
        Console.WriteLine("PASS: trusted launcher sign-in survives issuing a code and performs fresh verification for each subsequent launch; a rejected token reports an expired sign-in.");
        Console.WriteLine("PASS: a kept Gameforge token restores the sign-in after a restart without the password, and a revoked one asks to sign in again.");
    }

    private static void Check(bool condition, string error) { if (!condition) throw new Exception(error); }

    private sealed class Identity : IGameforgeIdentity
    {
        public string InstallationId => "aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb";
        public string UserAgent => "Official-Test-Browser";
        public int Calls;
        public JsonObject? Request;
        public Task<string> CreateAsync(HttpClient http, JsonObject? request = null)
        { Calls++; Request = request?.DeepClone().AsObject(); return Task.FromResult("tra:test-" + Calls); }
    }

    private sealed class Server(Identity identity) : HttpMessageHandler
    {
        public const string Account = "25222222-3333-4444-5555-666666666666";
        public int Codes, SignIns;
        public bool RejectIdentity, TokenExpired, RefuseCode, TokenRevoked;
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            JsonNode? body = request.Content is null ? null : JsonNode.Parse(await request.Content.ReadAsStringAsync(cancellationToken));
            if (request.Content is not null)
                Check(request.Content.Headers.ContentLength == (await request.Content.ReadAsByteArrayAsync(cancellationToken)).Length &&
                    request.Content.Headers.ContentType?.ToString() == "application/json", "Auth JSON framing differs from the native launcher.");
            var path = request.RequestUri!.AbsolutePath;
            if (path.EndsWith("/clientversioninfo.json")) return Json(200, "{\"version\":\"3.0.4.2238\"}");
            Check(request.Headers.GetValues("TNT-Installation-Id").Single() == identity.InstallationId, "Provider installation changed.");
            if (path.EndsWith("/sessions")) { SignIns++; return Json(201, "{\"token\":\"mock-token\"}"); }
            if (TokenRevoked) return Json(401, "{\"errorTypes\":[\"UNAUTHORIZED\"]}");
            Check(request.Headers.Authorization?.Parameter == "mock-token", "Session token changed.");
            if (path.EndsWith("/user/me")) return Json(200, "{}");
            if (path.EndsWith("/user/accounts")) return Json(200, "{\"account\":{\"id\":\"" + Account + "\",\"displayName\":\"EN_test\",\"guls\":{\"game\":\"nostale\"}}}");
            if (TokenExpired) return Json(401, "{\"errorTypes\":[\"UNAUTHORIZED\"]}");
            if (path.EndsWith("/iovation"))
            {
                Check(body?["accountId"]?.ToString() == Account && body["type"]?.ToString() == "play_now" && identity.Request is null, "Device verification used the display name or stale launch metadata.");
                return Json(RejectIdentity ? 403 : 200, RejectIdentity ? "{\"status\":\"failed\"}" : "{\"status\":\"ok\"}");
            }
            if (path.EndsWith("/thin/codes"))
            {
                if (RefuseCode)
                {
                    var refusal = Json(403, "{\"error\":{\"message\":\"Not allowed to create code\"}}");
                    refusal.Headers.Add("X-Request-ID", "mock-request");
                    return refusal;
                }
                Codes++;
                var gsid = body!["gsid"]!.ToString();
                Check(body["platformGameAccountId"]?.ToString() == Account && identity.Request?["installation"]?.ToString() == identity.InstallationId && identity.Request?["session"]?.ToString() == gsid[..gsid.LastIndexOf('-')], "Launch code identity was not bound to the selected account/session.");
                Check(body["blackbox"]?.ToString() == GameforgeIdentity.Encrypt("tra:test-" + identity.Calls, gsid, Account), "Launch encryption changed the official collector output.");
                return Json(201, "{\"code\":\"mock-code\"}");
            }
            throw new Exception("Unexpected auth request: " + path);
        }
        private static HttpResponseMessage Json(int status, string body) => new((HttpStatusCode)status) { Content = new StringContent(body, Encoding.UTF8, "application/json") };
    }
}
