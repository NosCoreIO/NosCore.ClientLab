using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json.Nodes;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using NosCore.ClientLab.Launcher.Services;

namespace NosCore.ClientLab.Launcher.ViewModels;

public partial class MainViewModel : ObservableObject
{
    public LauncherCoordinator? Coordinator { get; private set; }
    private JsonObject state = new();
    private bool polling;
    private bool loadedSettings;
    public event Action<string>? GameRequested;

    [ObservableProperty] private string currentPage = "library";
    [ObservableProperty] private string message = "Getting things ready…";
    [ObservableProperty] private string notice = "";
    [ObservableProperty] private bool busy = true;
    [ObservableProperty] private bool initialized;
    [ObservableProperty] private bool running;
    [ObservableProperty] private bool hasBuild;
    [ObservableProperty] private bool toolsReady;
    [ObservableProperty] private double progress;
    [ObservableProperty] private string stage = "Starting";
    [ObservableProperty] private string activity = "ClientLab is starting.";
    [ObservableProperty] private string version = "Not installed";
    [ObservableProperty] private string downloadSize = "Check for updates to see the download size.";
    [ObservableProperty] private string importDirectory = "";
    [ObservableProperty] private string identityFile = "";
    [ObservableProperty] private string installationId = "";
    [ObservableProperty] private string pythonPath = "";
    [ObservableProperty] private string ghidraPath = "";
    [ObservableProperty] private string javaPath = "";
    [ObservableProperty] private string emccPath = "";
    [ObservableProperty] private bool useGameforge = true;

    public string Device => OperatingSystem.IsWindows() ? "Windows" : OperatingSystem.IsMacOS() ? "macOS" : "Linux";
    public string DataDirectory => Program.DataDirectory;
    public bool IsLibrary => CurrentPage == "library";
    public bool IsActivity => CurrentPage == "activity";
    public bool IsSettings => CurrentPage == "settings";
    public bool HasNotice => !string.IsNullOrWhiteSpace(Notice);
    public bool IsIndeterminate => Busy && Progress <= 0;
    public bool CanAct => Initialized && !Busy;
    public bool CanCheck => CanAct && !Running;
    public bool CanPlayCurrent => CanAct && HasBuild;
    private bool UpdateAvailable => state["remote"]?["id"]?.GetValue<string>() is { } remote && remote != state["active"]?["id"]?.GetValue<string>();
    private bool HasGameFolder => !string.IsNullOrWhiteSpace(state["settings"]?["import_dir"]?.GetValue<string>());
    public string ActionLabel => Running ? "Resume game" : HasBuild && !UpdateAvailable ? "Play NosTale" : !ToolsReady ? "Set up build tools" : HasGameFolder ? "Build & play" : state["remote"] is null ? "Check for updates" : "Update & prepare";
    public string StatusLabel => Busy ? Stage : Running ? "Game is running" : HasBuild ? "Ready to play" : "Let's get you set up";

    partial void OnCurrentPageChanged(string value)
    {
        OnPropertyChanged(nameof(IsLibrary)); OnPropertyChanged(nameof(IsActivity)); OnPropertyChanged(nameof(IsSettings));
    }
    partial void OnNoticeChanged(string value) => OnPropertyChanged(nameof(HasNotice));
    partial void OnBusyChanged(bool value) => NotifyActions();
    partial void OnInitializedChanged(bool value) => NotifyActions();
    partial void OnRunningChanged(bool value) => NotifyActions();
    partial void OnHasBuildChanged(bool value) => NotifyActions();
    partial void OnToolsReadyChanged(bool value) => NotifyActions();
    partial void OnProgressChanged(double value) => OnPropertyChanged(nameof(IsIndeterminate));

    private void NotifyActions()
    {
        foreach (var name in new[] { nameof(CanAct), nameof(CanCheck), nameof(CanPlayCurrent), nameof(ActionLabel), nameof(StatusLabel), nameof(IsIndeterminate) })
            OnPropertyChanged(name);
    }

    public async Task InitializeAsync()
    {
        try
        {
            Message = "Preparing your workspace…";
            var root = await Task.Run(SourceAssets.Resolve);
            Coordinator = new LauncherCoordinator(root, Program.DataDirectory);
            Initialized = true;
            await RefreshAsync();
        }
        catch (Exception error) { Notice = error.Message; Busy = false; Message = "ClientLab could not start."; }
    }

    public Task RefreshAsync()
    {
        if (!Initialized || polling) return Task.CompletedTask;
        polling = true;
        try
        {
            state = Coordinator!.Snapshot();
            Busy = state["busy"]?.GetValue<bool>() ?? false;
            Running = state["running"]?.GetValue<bool>() ?? false;
            HasBuild = state["active"] is not null;
            ToolsReady = state["tools"]?.AsObject().All(p => p.Value?.GetValue<bool>() == true) == true;
            Message = state["message"]?.GetValue<string>() ?? "Ready";
            Stage = state["stage"]?.GetValue<string>() switch
            {
                "checking" => "Checking for updates", "downloading" => "Downloading game files", "analysing" => "Preparing client",
                "translating" => "Translating client", "compiling" => "Compiling runtime", "tools" => "Setting up tools", _ => "Ready"
            };
            Progress = (state["progress"]?.GetValue<double>() ?? 0) * 100;
            Activity = string.Join(Environment.NewLine, state["logs"]?.AsArray().Select(n => n?.GetValue<string>()) ?? []);
            if (state["error"]?.GetValue<string>() is { } error) Notice = error;
            Version = state["active"]?["build"]?.GetValue<string>() ?? "Not installed";
            if (Version.Length > 28) Version = Version[..28];
            if (state["plan"] is JsonObject plan)
                DownloadSize = $"{FormatBytes(plan["download_bytes"]!.GetValue<long>())} to download · {plan["files"]} files";
            var settings = state["settings"]!.AsObject();
            if (!loadedSettings)
            {
                ImportDirectory = settings["import_dir"]!.GetValue<string>();
                IdentityFile = settings["identity_file"]!.GetValue<string>();
                InstallationId = settings["installation_id"]!.GetValue<string>();
                UseGameforge = settings["provider"]!.GetValue<string>() == "gameforge";
                PythonPath = settings["python"]!.GetValue<string>(); GhidraPath = settings["ghidra"]!.GetValue<string>();
                JavaPath = settings["java"]!.GetValue<string>(); EmccPath = settings["emcc"]!.GetValue<string>();
                loadedSettings = true;
            }
            NotifyActions();
        }
        catch (Exception error) { Notice = error.Message; }
        finally { polling = false; }
        return Task.CompletedTask;
    }

    public static string FormatBytes(long bytes) => bytes >= 1024L * 1024 * 1024 ? $"{bytes / (1024d * 1024 * 1024):0.0} GB" : $"{bytes / (1024d * 1024):0.0} MB";

    [RelayCommand] private void Navigate(string page) => CurrentPage = page;

    private async Task ExecuteAsync(Func<Task> action)
    {
        Notice = "";
        try { await action(); await RefreshAsync(); }
        catch (Exception error) { Notice = error.Message; }
    }

    [RelayCommand]
    private Task PrimaryAsync() => ExecuteAsync(async () =>
    {
        if (!CanAct) return;
        if (Running || HasBuild && !UpdateAvailable) { await PlayAsync(); return; }
        Coordinator!.Start(!ToolsReady ? "tools" : HasGameFolder || state["remote"] is not null ? "prepare" : "check");
    });

    [RelayCommand]
    private Task CheckUpdatesAsync() => ExecuteAsync(() => { Coordinator!.Start("check"); return Task.CompletedTask; });

    [RelayCommand]
    private Task PlayCurrentAsync() => ExecuteAsync(PlayAsync);

    private async Task PlayAsync()
    {
        var url = await Coordinator!.LaunchAsync();
        var uri = new Uri(url);
        if (uri.Host != "127.0.0.1" || uri.Scheme != "http") throw new InvalidOperationException("Unexpected game server address.");
        GameRequested?.Invoke(url);
    }

    [RelayCommand]
    private Task CancelAsync() => ExecuteAsync(() => { Coordinator!.Cancel(); return Task.CompletedTask; });

    [RelayCommand]
    private Task SaveSettingsAsync() => ExecuteAsync(() =>
    {
        Coordinator!.SaveSettings(new JsonObject
        {
            ["import_dir"] = ImportDirectory, ["identity_file"] = IdentityFile,
            ["installation_id"] = InstallationId, ["provider"] = UseGameforge ? "gameforge" : "noscore",
            ["python"] = PythonPath, ["ghidra"] = GhidraPath, ["java"] = JavaPath, ["emcc"] = EmccPath,
        });
        Notice = "Settings saved.";
        return Task.CompletedTask;
    });

    // --- Native Gameforge sign-in ---
    private GameforgeSignIn? signIn;
    public ObservableCollection<AccountItem> SignInAccounts { get; } = [];
    public record AccountItem(string Id, string Name);

    [ObservableProperty] private bool signInVisible;
    [ObservableProperty] private bool signInBusy;
    [ObservableProperty] private string signInEmail = "";
    [ObservableProperty] private string signInPassword = "";
    [ObservableProperty] private string signInOtp = "";
    [ObservableProperty] private string signInCaptchaAnswer = "";
    [ObservableProperty] private string signInError = "";
    [ObservableProperty] private string signedInAs = "";
    [ObservableProperty] private Bitmap? captchaPrompt;
    [ObservableProperty] private Bitmap? captchaIcons;
    [ObservableProperty] private Bitmap? captchaTarget;

    public bool SignedIn => !string.IsNullOrEmpty(SignedInAs);
    public bool CanSignIn => UseGameforge && !SignedIn;
    public bool StageCredentials => signIn?.Stage == SignInStage.NeedCredentials;
    public bool StageOtp => signIn?.Stage == SignInStage.NeedOtp;
    public bool StageCaptcha => signIn?.Stage == SignInStage.NeedCaptcha;
    public bool StageAccounts => signIn?.Stage == SignInStage.ChooseAccount;
    public bool SignInSubmitVisible => StageCredentials || StageOtp || StageCaptcha;
    public string SignInSubmitLabel => signIn?.Stage switch { SignInStage.NeedOtp => "Verify code", SignInStage.NeedCaptcha => "Submit answer", _ => "Sign in" };
    public bool HasSignInError => !string.IsNullOrEmpty(SignInError);

    partial void OnSignedInAsChanged(string value) { OnPropertyChanged(nameof(SignedIn)); OnPropertyChanged(nameof(CanSignIn)); }
    partial void OnSignInErrorChanged(string value) => OnPropertyChanged(nameof(HasSignInError));
    partial void OnUseGameforgeChanged(bool value) => OnPropertyChanged(nameof(CanSignIn));

    [RelayCommand]
    private void OpenSignIn()
    {
        SignInError = "";
        try
        {
            signIn?.Dispose();
            signIn = Coordinator!.CreateSignIn();
            SignInEmail = ""; SignInPassword = ""; SignInOtp = ""; SignInCaptchaAnswer = ""; SignInAccounts.Clear();
            SignInVisible = true;
            RefreshSignIn();
        }
        catch (Exception error) { Notice = error.Message; }
    }

    [RelayCommand]
    private void CancelSignIn() { signIn?.Dispose(); signIn = null; SignInVisible = false; SignInError = ""; }

    [RelayCommand]
    private async Task SignInSubmitAsync()
    {
        if (signIn is null || SignInBusy) return;
        SignInBusy = true; SignInError = "";
        try
        {
            switch (signIn.Stage)
            {
                case SignInStage.NeedCredentials: await signIn.SignInAsync(SignInEmail, SignInPassword); break;
                case SignInStage.NeedOtp: await signIn.SubmitOtpAsync(SignInOtp); break;
                case SignInStage.NeedCaptcha:
                    if (!int.TryParse(SignInCaptchaAnswer, out var answer)) throw new InvalidOperationException("Enter the number of the matching image.");
                    await signIn.SubmitCaptchaAsync(answer); break;
            }
            SignInPassword = "";
            RefreshSignIn();
        }
        catch (Exception error) { SignInError = error.Message; }
        finally { SignInBusy = false; }
    }

    [RelayCommand]
    private async Task ChooseAccountAsync(string id)
    {
        if (signIn is null || SignInBusy) return;
        SignInBusy = true; SignInError = "";
        try
        {
            await signIn.ChooseAccountAsync(id);
            if (signIn.Stage == SignInStage.Ready && signIn.LaunchCode is { } code)
            {
                Coordinator!.UseLaunchCode(code, signIn.InstallationId);
                SignedInAs = SignInAccounts.FirstOrDefault(account => account.Id == id)?.Name ?? id;
                SignInVisible = false;
                Notice = "Signed in to Gameforge. Press Play to launch.";
            }
            RefreshSignIn();
        }
        catch (Exception error) { SignInError = error.Message; }
        finally { SignInBusy = false; }
    }

    private void RefreshSignIn()
    {
        SignInAccounts.Clear();
        if (signIn is not null)
        {
            foreach (var item in signIn.Accounts)
                SignInAccounts.Add(new AccountItem(item!["id"]!.GetValue<string>(), item["name"]?.GetValue<string>() ?? item["id"]!.GetValue<string>()));
            if (signIn.Captcha is { } captcha)
            {
                CaptchaPrompt = ToBitmap(captcha.Prompt); CaptchaIcons = ToBitmap(captcha.Icons); CaptchaTarget = ToBitmap(captcha.Target);
            }
        }
        foreach (var name in new[] { nameof(StageCredentials), nameof(StageOtp), nameof(StageCaptcha), nameof(StageAccounts), nameof(SignInSubmitLabel), nameof(SignInSubmitVisible) })
            OnPropertyChanged(name);
    }

    private static Bitmap? ToBitmap(string dataUri)
    {
        var comma = dataUri.IndexOf(',');
        if (comma < 0) return null;
        try { return new Bitmap(new MemoryStream(Convert.FromBase64String(dataUri[(comma + 1)..]))); }
        catch { return null; }
    }
}
