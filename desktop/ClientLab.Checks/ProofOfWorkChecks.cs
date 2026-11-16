using System.Net;
using System.Text;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class ProofOfWorkChecks
{
    public static async Task RunAsync()
    {
        foreach (var otp in new[] { false, true })
        {
            var server = new Server { RequireOtp = otp };
            var identity = new Identity();
            using var signIn = new GameforgeSignIn(new JsonObject(), identity, server);
            await signIn.SignInAsync("test@example.invalid", "mock-only");
            if (otp)
            {
                Check(signIn.Stage == SignInStage.NeedOtp, "OTP was not requested before proof-of-work.");
                await signIn.SubmitOtpAsync("123456");
            }
            Check(signIn.Stage == SignInStage.ChooseAccount && signIn.Accounts.Count == 1 && identity.Verifications == 1, "Proof-of-work did not continue to account selection exactly once.");
            Check(server.Logins == (otp ? 3 : 2), "Proof-of-work sent extra login attempts.");
        }
        var failedIdentity = new Identity { Fail = true }; var failedServer = new Server();
        using (var signIn = new GameforgeSignIn(new JsonObject(), failedIdentity, failedServer))
        {
            try { await signIn.SignInAsync("test@example.invalid", "mock-only"); throw new Exception("Failed work resumed credentials."); }
            catch (InvalidOperationException error) when (error.Message == "verification failed") { }
            Check(failedServer.Logins == 1, "Credentials were retried after failed work.");
        }
        var repeating = new Server { AlwaysRequireProof = true }; var repeatedIdentity = new Identity();
        using (var signIn = new GameforgeSignIn(new JsonObject(), repeatedIdentity, repeating))
        {
            try { await signIn.SignInAsync("test@example.invalid", "mock-only"); throw new Exception("Repeated proof had no bound."); }
            catch (InvalidOperationException error) when (error.Message.Contains("still requesting")) { }
            Check(repeatedIdentity.Verifications == 2 && repeating.Logins == 3, "Proof retries were not bounded.");
        }
        Console.WriteLine("PASS: official proof-of-work resumes credentials, preserves MFA, stops on failure, and bounds repeated challenges.");
    }

    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    private sealed class Identity : IGameforgeIdentity, IGameforgeVerification
    {
        public string InstallationId => "aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb";
        public string UserAgent => "Test-UA";
        public int Verifications; public bool Fail;
        public Task<string> CreateAsync(HttpClient http, JsonObject? request = null) => Task.FromResult("tra:fresh-test");
        public Task CompleteProofOfWorkAsync(string id)
        { Check(id == Server.Id, "Challenge ID changed."); Verifications++; if (Fail) throw new InvalidOperationException("verification failed"); return Task.CompletedTask; }
    }
    private sealed class Server : HttpMessageHandler
    {
        public const string Id = "12345678-1111-2222-3333-444444444444";
        public int Logins; public bool RequireOtp, AlwaysRequireProof;
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            if (request.RequestUri!.Host == "challenge.gameforge.com") return Json(200, "{\"type\":\"gf-pow-captcha\",\"script\":\"https://pow-captcha.gameforge.com/js/index.js\"}");
            var path = request.RequestUri.AbsolutePath;
            if (path.EndsWith("/sessions"))
            {
                Logins++;
                var payload = JsonNode.Parse(await request.Content!.ReadAsStringAsync(cancellationToken))!;
                if (RequireOtp && Logins == 1) return Json(409, "{\"errorTypes\":[\"OTP_REQUIRED\"]}");
                if (RequireOtp) Check(payload["otpCode"]?.ToString() == "123456", "MFA code was lost after proof-of-work.");
                if (AlwaysRequireProof || Logins == (RequireOtp ? 2 : 1))
                {
                    var response = Json(409, "{\"errorTypes\":[\"CHALLENGE_REQUIRED\"]}"); response.Headers.Add("gf-challenge-id", Id); return response;
                }
                return Json(201, "{\"token\":\"mock-token\"}");
            }
            if (path.EndsWith("/me")) return Json(200, "{}");
            if (path.EndsWith("/accounts")) return Json(200, "{\"test\":{\"id\":\"test\",\"displayName\":\"Test\",\"guls\":{\"game\":\"nostale\"}}}");
            throw new Exception("Unexpected proof request: " + path);
        }
        private static HttpResponseMessage Json(int status, string json) => new((HttpStatusCode)status) { Content = new StringContent(json, Encoding.UTF8, "application/json") };
    }
}
