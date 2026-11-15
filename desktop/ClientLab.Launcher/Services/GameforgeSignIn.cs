using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public enum SignInStage { NeedCredentials, NeedOtp, NeedCaptcha, ChooseAccount, Ready }

/// <summary>
/// Drives the Gameforge sign-in natively from the launcher (email, MFA, captcha, account, launch code) by reusing
/// <see cref="GameforgeAuthentication"/>. A captured identity file generates the device blackbox, so the whole flow
/// completes without the browser; the launch code is then handed to the game through the page URL.
/// </summary>
public sealed class GameforgeSignIn : IDisposable
{
    private readonly HttpClient http = new() { Timeout = TimeSpan.FromSeconds(40) };
    private readonly GameforgeAuthentication auth;
    private readonly GameforgeIdentity identity;
    private readonly string userAgent;
    private string? flowId;
    private string? sessionId;

    public SignInStage Stage { get; private set; } = SignInStage.NeedCredentials;
    public JsonArray Accounts { get; private set; } = [];
    public (string Prompt, string Icons, string Target)? Captcha { get; private set; }
    public string? LaunchCode { get; private set; }
    public string? InstallationId { get; private set; }

    public static bool IsConfigured(JsonObject settings) =>
        !string.IsNullOrWhiteSpace(settings["identity_file"]?.GetValue<string>());

    public GameforgeSignIn(JsonObject settings)
    {
        var file = settings["identity_file"]?.GetValue<string>() ?? "";
        if (file.Length == 0) throw new InvalidOperationException("Add a captured Gameforge identity file in Settings to sign in from the launcher.");
        identity = new GameforgeIdentity(file, settings["installation_id"]?.GetValue<string>());
        userAgent = identity.UserAgent;
        auth = new GameforgeAuthentication(settings);
    }

    public Task SignInAsync(string email, string password) => StepAsync(async () =>
        Apply(await auth.LoginAsync(new JsonObject { ["email"] = email.Trim(), ["password"] = password, ["blackbox"] = await BlackboxAsync() }, userAgent)));

    public Task SubmitOtpAsync(string code) => StepAsync(async () =>
        Apply(await auth.ChallengeAsync(new JsonObject { ["flowId"] = flowId, ["otpCode"] = code.Trim(), ["blackbox"] = await BlackboxAsync() })));

    // The on-screen choices are numbered from 1; Gameforge expects a zero-based index, matching the web page.
    public Task SubmitCaptchaAsync(int oneBasedChoice) => StepAsync(async () =>
        Apply(await auth.ChallengeAsync(new JsonObject { ["flowId"] = flowId, ["answer"] = oneBasedChoice - 1, ["blackbox"] = await BlackboxAsync() })));

    public Task ChooseAccountAsync(string accountId) => StepAsync(async () =>
    {
        var result = await auth.LaunchAsync(new JsonObject { ["sessionId"] = sessionId, ["accountId"] = accountId }, userAgent);
        LaunchCode = result["code"]?.GetValue<string>() ?? throw new InvalidOperationException("Gameforge did not return a launch code for this identity.");
        InstallationId = result["installationId"]?.GetValue<string>();
        Stage = SignInStage.Ready;
    });

    private async Task StepAsync(Func<Task> step)
    {
        try { await step(); }
        catch (GameforgeAuthentication.GameforgeException) { throw new InvalidOperationException("Gameforge rejected the sign-in. Check your details and try again."); }
    }

    private async Task<string> BlackboxAsync() => await identity.CreateAsync(http);

    private void Apply(JsonObject result)
    {
        switch (result["challenge"]?.GetValue<string>())
        {
            case "otp":
                flowId = result["flowId"]!.GetValue<string>();
                Stage = SignInStage.NeedOtp;
                return;
            case "captcha":
                flowId = result["flowId"]!.GetValue<string>();
                var images = result["images"]!.AsObject();
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
