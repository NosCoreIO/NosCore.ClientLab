using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public enum SignInStage { NeedCredentials, NeedOtp, NeedCaptcha, ChooseAccount, Ready }

/// <summary>
/// Drives the native sign-in form while the embedded browser runs Gameforge's official identity collector.
/// The same provider is retained for credentials, MFA/CAPTCHA continuation, and account-bound launch.
/// </summary>
public sealed class GameforgeSignIn : IDisposable
{
    private readonly HttpClient http = new() { Timeout = TimeSpan.FromSeconds(40) };
    private readonly GameforgeAuthentication auth;
    private readonly IGameforgeIdentity identity;
    private readonly string userAgent;
    private string? flowId;
    private string? sessionId;
    private string? captchaVersion;

    public SignInStage Stage { get; private set; } = SignInStage.NeedCredentials;
    public JsonArray Accounts { get; private set; } = [];
    public (string Prompt, string Icons, string Target)? Captcha { get; private set; }
    public string? LaunchCode { get; private set; }
    public string? InstallationId { get; private set; }
    public string? ChallengeMessage { get; private set; }
    public string? CaptchaVersion => captchaVersion;

    public static bool IsConfigured(JsonObject settings) =>
        !string.IsNullOrWhiteSpace(settings["identity_file"]?.GetValue<string>());

    public GameforgeSignIn(JsonObject settings, IGameforgeIdentity? provider = null, HttpMessageHandler? handler = null)
    {
        var file = settings["identity_file"]?.GetValue<string>() ?? "";
        if (provider is null && file.Length == 0) throw new InvalidOperationException("A registered Gameforge installation ID is required to sign in.");
        identity = provider ?? new GameforgeIdentity(file, settings["installation_id"]?.GetValue<string>());
        userAgent = identity.UserAgent;
        auth = new GameforgeAuthentication(settings, handler, identity);
    }

    public Task SignInAsync(string email, string password) => StepAsync(async () =>
        await ApplyAsync(await auth.LoginAsync(new JsonObject { ["email"] = email.Trim(), ["password"] = password, ["blackbox"] = await BlackboxAsync() }, userAgent)));

    public Task SubmitOtpAsync(string code) => StepAsync(async () =>
        await ApplyAsync(await auth.ChallengeAsync(new JsonObject { ["flowId"] = flowId, ["otpCode"] = code.Trim(), ["blackbox"] = await BlackboxAsync() })));

    // The on-screen choices are numbered from 1; Gameforge expects a zero-based index, matching the web page.
    public Task SubmitCaptchaAsync(int oneBasedChoice)
    {
        if (oneBasedChoice is < 1 or > 4) throw new InvalidOperationException("Choose one of the four CAPTCHA images.");
        return StepAsync(async () =>
            await ApplyAsync(await auth.ChallengeAsync(new JsonObject { ["flowId"] = flowId, ["answer"] = oneBasedChoice - 1, ["captchaVersion"] = captchaVersion, ["blackbox"] = await BlackboxAsync() })));
    }

    public Task ChooseAccountAsync(string accountId) => StepAsync(async () =>
    {
        var result = await auth.LaunchAsync(new JsonObject { ["sessionId"] = sessionId, ["accountId"] = accountId }, userAgent, retainSession: true);
        LaunchCode = result["code"]?.GetValue<string>() ?? throw new InvalidOperationException("Gameforge did not return a launch code for this identity.");
        InstallationId = result["installationId"]?.GetValue<string>();
        Stage = SignInStage.Ready;
    });

    private async Task StepAsync(Func<Task> step)
    {
        try { await step(); }
        catch (GameforgeAuthentication.GameforgeException error)
        {
            var message = error.Operation switch
            {
                "auth/iovation" => "Gameforge rejected device verification",
                "auth/thin/codes" => "Gameforge refused to issue a game launch code",
                _ => "Gameforge " + error.Operation + " failed"
            };
            throw new InvalidOperationException($"{message} (HTTP {error.Status}).", error);
        }
    }

    private async Task<string> BlackboxAsync() => await identity.CreateAsync(http);

    private async Task ApplyAsync(JsonObject result)
    {
        for (var attempt = 0; result["challenge"]?.GetValue<string>() == "pow"; attempt++)
        {
            if (attempt >= 2) throw new InvalidOperationException("Gameforge is still requesting device verification. Please wait before trying again.");
            if (identity is not IGameforgeVerification verification)
                throw new InvalidOperationException("Proof-of-work verification requires the launcher's embedded browser.");
            flowId = result["flowId"]!.GetValue<string>();
            await verification.CompleteProofOfWorkAsync(result["challengeId"]!.GetValue<string>());
            // Gameforge validates the proof when credentials are retried; no local success flag grants a session.
            result = await auth.ChallengeAsync(new JsonObject { ["flowId"] = flowId, ["verificationComplete"] = true, ["blackbox"] = await BlackboxAsync() });
        }
        ChallengeMessage = result["message"]?.GetValue<string>();
        switch (result["challenge"]?.GetValue<string>())
        {
            case "otp":
                flowId = result["flowId"]!.GetValue<string>();
                Stage = SignInStage.NeedOtp;
                return;
            case "captcha":
                flowId = result["flowId"]!.GetValue<string>();
                var images = result["images"]!.AsObject();
                captchaVersion = images["version"]?.GetValue<string>();
                Captcha = (images["prompt"]!.GetValue<string>(), images["icons"]!.GetValue<string>(), images["target"]!.GetValue<string>());
                Stage = SignInStage.NeedCaptcha;
                return;
        }
        sessionId = result["sessionId"]?.GetValue<string>() ?? throw new InvalidOperationException("Unexpected sign-in response from Gameforge.");
        Accounts = result["accounts"]!.AsArray().DeepClone().AsArray();
        Stage = SignInStage.ChooseAccount;
    }

    public void Dispose() { http.Dispose(); auth.Dispose(); }
}
