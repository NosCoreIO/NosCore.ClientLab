using System.Net;
using System.Text;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class CaptchaChecks
{
    public static async Task RunAsync()
    {
        var settings = new JsonObject { ["installation_id"] = "aaaaaaaa-1111-2222-3333-bbbbbbbbbbbb" };
        var credentials = new JsonObject { ["email"] = "test@example.invalid", ["password"] = "mock-only", ["blackbox"] = "test" };
        var server = new ChallengeServer();
        using (var auth = new GameforgeAuthentication(settings, server))
        {
            var challenge = await auth.LoginAsync(credentials, "ClientLab-Test-UA");
            Require(challenge["challenge"]?.ToString() == "captcha", "Image challenge was not presented.");
            var flow = challenge["flowId"]!.ToString();
            var firstVersion = challenge["images"]!["version"]!.ToString();
            try
            {
                await auth.ChallengeAsync(new JsonObject { ["flowId"] = flow, ["answer"] = -1 });
                throw new Exception("An invalid image index was submitted.");
            }
            catch (InvalidOperationException) { }
            Require(server.Answers == 0, "Invalid answer reached the service.");
            var rejected = await auth.ChallengeAsync(new JsonObject { ["flowId"] = flow, ["answer"] = 2, ["captchaVersion"] = firstVersion });
            Require(rejected["message"]!.ToString().Contains("422"), "Rejected submission lost its HTTP status.");
            Require(rejected["images"]!["version"]!.ToString() != firstVersion, "Rejected submission did not refresh images.");
            await auth.ChallengeAsync(new JsonObject { ["flowId"] = flow, ["answer"] = 2, ["captchaVersion"] = firstVersion });
            Require(server.Answers == 1, "A stale image-set answer was sent.");
            var retry = await auth.ChallengeAsync(new JsonObject { ["flowId"] = flow, ["answer"] = 2 });
            Require(retry["challenge"]?.ToString() == "captcha" && retry["message"] is not null, "Incorrect answer lost retry state.");
            var success = await auth.ChallengeAsync(new JsonObject { ["flowId"] = flow, ["answer"] = 2, ["blackbox"] = "fresh-test" });
            Require(success["accounts"]!.AsArray().Count == 1, "Solved challenge did not resume the credentials session.");
            Require(server.Answers == 3 && server.Logins == 2, "Challenge retry sent duplicate login/answer requests.");
        }
        var pow = new ChallengeServer { Type = "gf-pow-captcha" };
        using (var auth = new GameforgeAuthentication(settings, pow))
        {
            var challenge = await auth.LoginAsync(credentials, "ClientLab-Test-UA");
            Require(challenge["challenge"]?.ToString() == "pow" && challenge["challengeId"]?.ToString() == "mock-challenge", "Proof-of-work challenge metadata was lost.");
            Require(pow.MetadataRequests == 0 && pow.Logins == 1, "Proof-of-work reached the image service or retried credentials prematurely.");
            try { await auth.ChallengeAsync(new JsonObject { ["flowId"] = challenge["flowId"]!.DeepClone() }); throw new Exception("Incomplete verification resumed login."); }
            catch (InvalidOperationException error) when (error.Message.Contains("proof-of-work")) { }
            var success = await auth.ChallengeAsync(new JsonObject { ["flowId"] = challenge["flowId"]!.DeepClone(), ["verificationComplete"] = true, ["blackbox"] = "fresh-pow-test" });
            Require(success["accounts"]!.AsArray().Count == 1 && pow.Logins == 2, "Completed proof-of-work did not resume server-validated login.");
        }
        var unsupported = new ChallengeServer { Type = "gf-unknown-challenge" };
        using (var auth = new GameforgeAuthentication(settings, unsupported))
        {
            try { await auth.LoginAsync(credentials, "ClientLab-Test-UA"); throw new Exception("A non-image challenge was treated as image-drop."); }
            catch (InvalidOperationException error) when (error.Message.Contains("not an image CAPTCHA")) { }
            Require(unsupported.MetadataRequests == 0, "A different challenge type reached the image service.");
        }
        var expired = new ChallengeServer { MetadataStatus = 422 };
        using (var auth = new GameforgeAuthentication(settings, expired))
        {
            try { await auth.LoginAsync(credentials, "ClientLab-Test-UA"); throw new Exception("Metadata failure was hidden."); }
            catch (GameforgeAuthentication.GameforgeException error) when (error.Status == 422 && error.Operation == "CAPTCHA metadata") { }
        }
        Console.WriteLine("PASS: CAPTCHA headers, image versions, invalid/stale answers, 422 refresh, rejection retry, solved-session continuation and challenge type detection.");
    }

    private static void Require(bool value, string message) { if (!value) throw new Exception(message); }

    private sealed class ChallengeServer : HttpMessageHandler
    {
        public int Answers, Logins, MetadataRequests;
        public string Type = "gf-image-drop-captcha";
        public int MetadataStatus = 200;
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var uri = request.RequestUri!;
            Require(request.Headers.UserAgent.ToString() == "ClientLab-Test-UA", "Challenge identity user-agent changed.");
            if (uri.Host == "spark.gameforge.com")
            {
                if (uri.AbsolutePath.EndsWith("/sessions"))
                {
                    Logins++;
                    if (Logins > 1) return Json(201, "{\"token\":\"mock-token\"}");
                    var response = Json(409, "{\"errorTypes\":[\"CHALLENGE_REQUIRED\"]}");
                    response.Headers.Add("gf-challenge-id", "mock-challenge; ignored-attribute");
                    return response;
                }
                if (uri.AbsolutePath.EndsWith("/me")) return Json(200, "{}");
                if (uri.AbsolutePath.EndsWith("/accounts")) return Json(200, "{\"mock-account\":{\"id\":\"mock-account\",\"displayName\":\"Test\",\"guls\":{\"game\":\"nostale\"}}}");
            }
            Require(request.Headers.GetValues("Origin").Single() == "spark://www.gameforge.com", "Missing challenge origin.");
            Require(request.Headers.Accept.Count > 0, "Missing challenge Accept header.");
            if (uri.Host == "challenge.gameforge.com") return Json(200, new JsonObject { ["type"] = Type }.ToJsonString());
            if (uri.Host == "image-drop-challenge.gameforge.com")
            {
                if (request.Method == HttpMethod.Post)
                {
                    var payload = JsonNode.Parse(await request.Content!.ReadAsStringAsync(cancellationToken))!;
                    Require(payload["answer"]!.GetValue<int>() == 2, "Chosen index was changed during submission.");
                    Answers++;
                    return Answers switch { 1 => Json(422, "{\"status\":\"failed\"}"), 2 => Json(200, "{\"status\":\"failed\"}"), _ => Json(200, "{\"status\":\"solved\"}") };
                }
                if (uri.AbsolutePath.EndsWith("/en-GB"))
                {
                    MetadataRequests++;
                    return Json(MetadataStatus, new JsonObject { ["lastUpdated"] = 1000 + MetadataRequests }.ToJsonString());
                }
                Require(uri.Query == "?" + (1000 + MetadataRequests), "Images came from different challenge versions.");
                return new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent([1, 2, 3]) };
            }
            throw new Exception("Unexpected test request.");
        }
        private static HttpResponseMessage Json(int status, string json) => new((HttpStatusCode)status) { Content = new StringContent(json, Encoding.UTF8, "application/json") };
    }
}
