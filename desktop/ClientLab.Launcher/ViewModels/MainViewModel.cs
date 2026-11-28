using System.Collections.ObjectModel;
using System.IO;
using System.Diagnostics;
using System.Text.Json.Nodes;
using Avalonia.Media.Imaging;
using Avalonia;
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
    private CredentialStore? credentialStore;
    private CredentialStore? sessionStore;
    private readonly SemaphoreSlim sessionWrites = new(1);
    public event Action<string>? GameRequested;
    public Func<Task<IGameforgeIdentity>>? IdentityRequested { get; set; }

    [ObservableProperty] private string currentPage = "library";
    [ObservableProperty] private Bitmap? artwork;
    public bool HasArtwork => Artwork is not null;
    public ObservableCollection<NewsCard> News { get; } = [];
    public ObservableCollection<NewsPage> NewsPages { get; } = [];
    public record NewsPage(IReadOnlyList<NewsCard> Items);
    [ObservableProperty] private int newsPageIndex;
    public bool CanPageNews => NewsPages.Count > 1;
    public bool CanPreviousNews => NewsPageIndex > 0;
    public bool CanNextNews => NewsPageIndex + 1 < NewsPages.Count;
    public string NewsPageLabel => $"{NewsPageIndex + 1} / {NewsPages.Count}";
    partial void OnNewsPageIndexChanged(int value)
    {
        OnPropertyChanged(nameof(NewsPageLabel));
        OnPropertyChanged(nameof(CanPreviousNews)); OnPropertyChanged(nameof(CanNextNews));
        if (value >= 0 && value < NewsPages.Count)
            foreach (var card in NewsPages[value].Items) _ = LoadNewsImageAsync(card);
    }
    [RelayCommand] private void PreviousNews() { if (CanPreviousNews) NewsPageIndex--; }
    [RelayCommand] private void NextNews() { if (CanNextNews) NewsPageIndex++; }
    public partial class NewsCard(NewsEntry entry) : ObservableObject
    {
        public NewsEntry Entry => entry;
        public string Title => entry.Title;
        public string Url => entry.Url;
        public string Category => entry.Category;
        public string Date => entry.Date;
        [ObservableProperty] private Bitmap? image;
        public bool LoadingImage { get; set; }
    }
    public bool HasNews => News.Count > 0;
    partial void OnArtworkChanged(Bitmap? value) => OnPropertyChanged(nameof(HasArtwork));
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
    [ObservableProperty] private bool gameMusic = true;
    [ObservableProperty] private bool gameSoundEffects = true;
    [ObservableProperty] private bool gameSpatialSound = true;
    [ObservableProperty] private bool gameFullScreen;
    [ObservableProperty] private bool packetLogger;
    [ObservableProperty] private string packetPlugin = "";
    [ObservableProperty] private string gamePreferencesStatus = "Changes are saved automatically for the next game launch.";
    private bool loadingGameOptions;
    private Task gameOptionsSave = Task.CompletedTask;
    private Exception? gameOptionsSaveError;
    partial void OnGameMusicChanged(bool value) => SaveGamePreferenceChange();
    partial void OnGameSoundEffectsChanged(bool value) => SaveGamePreferenceChange();
    partial void OnGameSpatialSoundChanged(bool value) => SaveGamePreferenceChange();
    partial void OnGameFullScreenChanged(bool value) => SaveGamePreferenceChange();

    private void SaveGamePreferenceChange()
    {
        if (loadingGameOptions || !Initialized || Coordinator is null) return;
        var options = new GameOptions(GameMusic, GameSoundEffects, GameSpatialSound, GameFullScreen);
        GamePreferencesStatus = "Saving game preferences…";
        gameOptionsSave = SaveAfterAsync(gameOptionsSave, options);
    }

    private async Task SaveAfterAsync(Task previous, GameOptions options)
    {
        try
        {
            await previous;
            await Coordinator!.SaveGameOptionsAsync(options);
            gameOptionsSaveError = null;
            GamePreferencesStatus = "Saved. Applies on the next game launch.";
        }
        catch (Exception error)
        {
            gameOptionsSaveError = error;
            GamePreferencesStatus = "Game preferences were not saved: " + error.Message;
            Notice = GamePreferencesStatus;
        }
    }

    public async Task FlushGamePreferencesAsync()
    {
        await gameOptionsSave;
        if (gameOptionsSaveError is not null) throw new IOException("Game preferences could not be saved. Retry the setting before launching.", gameOptionsSaveError);
    }
    [ObservableProperty] private string privateAuthUrl = "https://127.0.0.1:7001";
    [ObservableProperty] private string privateServerHost = "";
    [ObservableProperty] private string privatePortRemap = "";
    private string? privatePassword;
    public string AccountLabel => UseGameforge ? "Email address" : "Account name";

    public string Device => OperatingSystem.IsWindows() ? "Windows" : OperatingSystem.IsMacOS() ? "macOS" : "Linux";
    public string DataDirectory => Program.DataDirectory;
    public bool IsLibrary => CurrentPage == "library";
    public bool IsActivity => CurrentPage == "activity";
    public bool IsSettings => CurrentPage == "settings";
    public bool HasNotice => !string.IsNullOrWhiteSpace(Notice);
    public bool IsIndeterminate => Busy && Progress <= 0;
    public bool CanAct => Initialized && !Busy;
    public bool CanPrimary => Initialized && !SignInBusy && !SignInVisible && (ShowCredentials || !Busy);
    public bool ShowCredentials => !SignedIn;
    public string WelcomeMessage => SignedIn ? "You're all set. Your next adventure is just a click away." : UseGameforge ? "Sign in with Gameforge and pick up where you left off." : "Your NosCore adventure, all in one place. Ready when you are.";
    public bool CanCheck => CanAct && !Running;
    public bool CanSaveSettings => Initialized;
    public bool CanSelectMode => Initialized && !Running && !SignInBusy;
    public bool CanPlayCurrent => CanAct && HasBuild;
    private bool UpdateAvailable => state["remote"]?["id"]?.GetValue<string>() is { } remote && remote != state["active"]?["id"]?.GetValue<string>();
    private bool HasGameFolder => !string.IsNullOrWhiteSpace(state["settings"]?["import_dir"]?.GetValue<string>());
    public string ActionLabel => SignInBusy ? "Signing in…" : ShowCredentials ? "Sign in  →" : Busy ? "Updating game…" : Running ? "Resume game  →" : HasBuild ? "Play NosTale  →" : "Retry game update  →";
    public string StatusLabel => Busy ? Stage : Running ? "Game is running" : HasBuild ? "Ready to play" : "Ready for your adventure";

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
        foreach (var name in new[] { nameof(CanAct), nameof(CanPrimary), nameof(CanCheck), nameof(CanSaveSettings), nameof(CanSelectMode), nameof(CanPlayCurrent), nameof(ActionLabel), nameof(StatusLabel), nameof(IsIndeterminate), nameof(ShowCredentials), nameof(WelcomeMessage) })
            OnPropertyChanged(name);
    }

    public async Task InitializeAsync()
    {
        try
        {
            _ = LoadArtworkAsync();
            _ = LoadNewsAsync();
            Message = "Preparing your workspace…";
            var root = await Task.Run(SourceAssets.Resolve);
            Coordinator = new LauncherCoordinator(root, Program.DataDirectory);
            try { await LoadGameOptionsAsync(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { Notice = "Game preferences could not be loaded: " + error.Message; }
            UseGameforge = Coordinator.Snapshot()["settings"]!["provider"]!.GetValue<string>() == "gameforge";
            credentialStore = new CredentialStore(UseGameforge ? Program.DataDirectory : Path.Combine(Program.DataDirectory, "private-server"));
            sessionStore = UseGameforge ? new CredentialStore(Program.DataDirectory, "session") : null;
            try
            {
                var saved = await Task.Run(() => credentialStore.ReadAsync());
                if (saved is not null) { SignInEmail = saved.Email; SignInPassword = saved.Password; }
            }
            catch (Exception) { Notice = "Your saved account could not be loaded. You can still sign in."; }
            Initialized = true;
            Coordinator.Start("startup");
            StartLauncherUpdates();
            // A slow Gameforge must not hold back the update check.
            _ = ResumeSessionAsync();
            await RefreshAsync();
        }
        catch (Exception error) { Notice = error.Message; Busy = false; Message = "ClientLab could not start."; }
    }

    private LauncherUpdates? launcherUpdates;
    public event Action? RestartRequested;
    [ObservableProperty] private string launcherUpdateVersion = "";
    public bool LauncherUpdateReady => LauncherUpdateVersion.Length > 0;
    public string LauncherUpdateLabel => $"ClientLab {LauncherUpdateVersion} is ready";
    partial void OnLauncherUpdateVersionChanged(string value)
    {
        OnPropertyChanged(nameof(LauncherUpdateReady)); OnPropertyChanged(nameof(LauncherUpdateLabel));
    }

    private void StartLauncherUpdates()
    {
        launcherUpdates = new LauncherUpdates(text => Coordinator?.Log(text));
        launcherUpdates.StagedChanged += update => Dispatcher.UIThread.Post(() => LauncherUpdateVersion = update.Version);
        launcherUpdates.StartBackground();
    }

    /// <summary>Closes ClientLab (and the game, with its usual exit prompt) so the bootstrap can swap in the staged
    /// launcher and start it again. Without a restart the update still applies on the next start.</summary>
    [RelayCommand]
    private void RestartToUpdate()
    {
        if (!LauncherUpdateReady) return;
        Program.RestartRequested = true;
        RestartRequested?.Invoke();
    }

    public void DisposeLauncherUpdates() => launcherUpdates?.Dispose();

    private async Task LoadArtworkAsync()
    {
        try
        {
            if (await LauncherArtwork.LoadAsync(Program.DataDirectory) is { } bytes)
                Artwork = new Bitmap(new MemoryStream(bytes));
        }
        catch (Exception) { } // The local illustration remains available offline.
    }

    private async Task LoadNewsAsync()
    {
        foreach (var item in await OfficialNews.LoadAsync(Program.DataDirectory))
        {
            News.Add(new NewsCard(item));
        }
        foreach (var page in News.Chunk(4)) NewsPages.Add(new NewsPage(page));
        OnPropertyChanged(nameof(CanPageNews)); OnPropertyChanged(nameof(NewsPageLabel));
        OnNewsPageIndexChanged(NewsPageIndex);
        OnPropertyChanged(nameof(HasNews));
    }

    private async Task LoadNewsImageAsync(NewsCard card)
    {
        if (card.Image is not null || card.LoadingImage) return;
        card.LoadingImage = true;
        try
        {
            if (await OfficialNews.LoadImageAsync(card.Entry, Program.DataDirectory) is { } bytes)
                card.Image = new Bitmap(new MemoryStream(bytes));
        }
        catch (Exception) { }
        finally { card.LoadingImage = false; }
    }

    [RelayCommand]
    private void OpenNews(string? url)
    {
        if (url is null || !OfficialNews.IsOfficialLink(url)) return;
        if (!ExternalLinks.Open(url)) Notice = "Your browser could not open the news page.";
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
                "checking" => "Checking game files", "downloading" => "Downloading game files", "installing" => "Preparing local files", "analysing" => "Preparing client",
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
                PrivateAuthUrl = settings["private_auth_url"]!.GetValue<string>();
                PrivateServerHost = settings["private_server_host"]!.GetValue<string>();
                PrivatePortRemap = settings["private_port_remap"]!.GetValue<string>();
                PacketLogger = settings["packet_logger"]?.GetValue<string>() == "true";
                PacketPlugin = settings["packet_plugin"]?.GetValue<string>() ?? "";
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

    [RelayCommand] private async Task NavigateAsync(string page)
    {
        CurrentPage = page;
        if (page == "settings") await ExecuteAsync(LoadGameOptionsAsync);
    }

    private async Task LoadGameOptionsAsync()
    {
        if (Coordinator is null) return;
        await FlushGamePreferencesAsync();
        var options = await Coordinator.ReadGameOptionsAsync();
        loadingGameOptions = true;
        try
        {
            GameMusic = options.Music; GameSoundEffects = options.SoundEffects;
            GameSpatialSound = options.SpatialSound; GameFullScreen = options.FullScreen;
        }
        finally { loadingGameOptions = false; }
    }

    private async Task ExecuteAsync(Func<Task> action)
    {
        Notice = "";
        try { await action(); await RefreshAsync(); }
        catch (Exception error) { Notice = error.Message; }
    }

    [RelayCommand]
    private Task PrimaryAsync() => ExecuteAsync(async () =>
    {
        if (!CanPrimary) return;
        if (Running) { await PlayAsync(); return; }
        if (ShowCredentials)
        {
            if (string.IsNullOrWhiteSpace(SignInEmail) || string.IsNullOrEmpty(SignInPassword))
                throw new InvalidOperationException("Enter your account and password to continue.");
            await SaveFormCredentialsAsync();
            if (!UseGameforge)
            {
                SignInBusy = true;
                try
                {
                    await PrivateServerAuthentication.SignInAsync(PrivateAuthUrl, SignInEmail, SignInPassword);
                    privatePassword = SignInPassword; SignInPassword = ""; SignedInAs = SignInEmail;
                }
                finally { SignInBusy = false; }
                return;
            }
            await CreateSignInAsync();
            await SignInSubmitAsync();
            return;
        }
        if (!HasBuild || UpdateAvailable)
        {
            await Coordinator!.StartAndWaitAsync("startup");
            await RefreshAsync();
        }
        await PlayAsync();
    });

    [RelayCommand]
    private Task CheckUpdatesAsync() => ExecuteAsync(() => { Coordinator!.Start("startup"); return Task.CompletedTask; });

    [RelayCommand]
    private Task PlayCurrentAsync() => ExecuteAsync(PlayAsync);

    private async Task PlayAsync()
    {
        await FlushGamePreferencesAsync();
        if (!Running && !UseGameforge && privatePassword is not null)
            Coordinator!.UseLaunchCode(await PrivateServerAuthentication.SignInAsync(PrivateAuthUrl, SignedInAs, privatePassword), null);
        if (!Running && signIn is not null && selectedAccountId is { } accountId)
        {
            // Request the one-use code after compilation, immediately before launching.
            SignInBusy = true;
            try
            {
                var expired = false;
                try { await signIn.ChooseAccountAsync(accountId); }
                catch (GameforgeAuthentication.SessionExpiredException) { expired = true; }
                if (expired)
                {
                    await ForgetSessionAsync();
                    if (!await RenewSignInAsync(accountId)) throw new GameforgeAuthentication.SessionExpiredException();
                    await signIn!.ChooseAccountAsync(accountId);
                }
                Coordinator!.UseLaunchCode(signIn!.LaunchCode!, signIn.InstallationId);
            }
            finally { SignInBusy = false; }
        }
        var url = await Coordinator!.LaunchAsync();
        var uri = new Uri(url);
        if (uri.Host != "127.0.0.1" || uri.Scheme != "http") throw new InvalidOperationException("Unexpected game server address.");
        GameRequested?.Invoke(url);
    }

    [RelayCommand]
    private Task CancelAsync() => ExecuteAsync(() => { Coordinator!.Cancel(); return Task.CompletedTask; });

    [RelayCommand]
    private Task SaveSettingsAsync() => ExecuteAsync(async () =>
    {
        Coordinator!.SaveSettings(new JsonObject
        {
            ["import_dir"] = ImportDirectory, ["identity_file"] = IdentityFile,
            ["installation_id"] = InstallationId, ["provider"] = UseGameforge ? "gameforge" : "noscore",
            ["private_auth_url"] = PrivateAuthUrl, ["private_server_host"] = PrivateServerHost, ["private_port_remap"] = PrivatePortRemap,
            ["packet_logger"] = PacketLogger ? "true" : "false", ["packet_plugin"] = PacketPlugin,
            ["python"] = PythonPath, ["ghidra"] = GhidraPath, ["java"] = JavaPath, ["emcc"] = EmccPath,
        });
        await FlushGamePreferencesAsync();
        Notice = "Settings saved. Launch options apply to the next game session; file and tool changes apply to the next preparation.";
    });

    // --- Native Gameforge sign-in ---
    private GameforgeSignIn? signIn;
    private string? selectedAccountId;
    public ObservableCollection<AccountItem> SignInAccounts { get; } = [];
    public record AccountItem(string Id, string Name);

    [ObservableProperty] private bool signInVisible;
    [ObservableProperty] private bool signInBusy;
    [ObservableProperty] private string signInEmail = "";
    [ObservableProperty] private string signInPassword = "";
    [ObservableProperty] private bool rememberCredentials = true;
    [ObservableProperty] private string signInOtp = "";
    [ObservableProperty] private string signInCaptchaAnswer = "";
    [ObservableProperty] private string signInError = "";
    [ObservableProperty] private string signedInAs = "";
    [ObservableProperty] private Bitmap? captchaPrompt;
    [ObservableProperty] private Bitmap? captchaIcons;
    [ObservableProperty] private Bitmap? captchaTarget;
    public ObservableCollection<CaptchaChoice> CaptchaChoices { get; } = [];
    public record CaptchaChoice(int Number, CroppedBitmap Image, Guid Revision);
    public Guid CaptchaRevision { get; private set; }

    [RelayCommand]
    private async Task ChooseCaptchaAsync(CaptchaChoice choice)
    {
        if (SignInBusy || !StageCaptcha || choice.Revision != CaptchaRevision) return;
        SignInCaptchaAnswer = choice.Number.ToString();
        await SignInSubmitAsync();
    }

    public Task DropCaptchaAsync(int number, Guid revision)
    {
        var choice = CaptchaChoices.FirstOrDefault(item => item.Number == number && item.Revision == revision);
        return choice is null ? Task.CompletedTask : ChooseCaptchaAsync(choice);
    }

    public bool SignedIn => !string.IsNullOrEmpty(SignedInAs);
    public bool CanSignIn => UseGameforge && !SignedIn;
    public bool StageCredentials => signIn?.Stage == SignInStage.NeedCredentials;
    public bool StageOtp => signIn?.Stage == SignInStage.NeedOtp;
    public bool StageCaptcha => signIn?.Stage == SignInStage.NeedCaptcha;
    public bool StageAccounts => signIn?.Stage == SignInStage.ChooseAccount;
    public bool SignInSubmitVisible => StageCredentials || StageOtp;
    public string SignInSubmitLabel => signIn?.Stage switch { SignInStage.NeedOtp => "Verify code", SignInStage.NeedCaptcha => "Submit answer", _ => "Sign in" };
    public bool HasSignInError => !string.IsNullOrEmpty(SignInError);

    partial void OnSignedInAsChanged(string value) { OnPropertyChanged(nameof(SignedIn)); OnPropertyChanged(nameof(CanSignIn)); NotifyActions(); }
    partial void OnSignInBusyChanged(bool value) => NotifyActions();
    partial void OnSignInVisibleChanged(bool value) => NotifyActions();
    partial void OnRememberCredentialsChanged(bool value) { if (!value) _ = SaveFormCredentialsAsync(); }
    partial void OnSignInErrorChanged(string value) => OnPropertyChanged(nameof(HasSignInError));
    partial void OnUseGameforgeChanged(bool value) { OnPropertyChanged(nameof(CanSignIn)); OnPropertyChanged(nameof(AccountLabel)); NotifyActions(); }

    [RelayCommand]
    private Task SelectModeAsync(string mode) => ExecuteAsync(async () =>
    {
        if (!CanSelectMode || mode == (UseGameforge ? "gameforge" : "noscore")) return;
        await Coordinator!.CancelAndWaitAsync();
        Coordinator!.SaveSettings(new JsonObject { ["provider"] = mode });
        signIn?.Dispose(); signIn = null; selectedAccountId = null; privatePassword = null;
        Coordinator.ClearLaunchCode(); SignedInAs = ""; SignInVisible = false; SignInError = "";
        UseGameforge = mode == "gameforge";
        SignInEmail = ""; SignInPassword = "";
        credentialStore = new CredentialStore(UseGameforge ? Program.DataDirectory : Path.Combine(Program.DataDirectory, "private-server"));
        sessionStore = UseGameforge ? new CredentialStore(Program.DataDirectory, "session") : null;
        var saved = await credentialStore.ReadAsync();
        if (saved is not null) { SignInEmail = saved.Email; SignInPassword = saved.Password; }
        Coordinator.Start("startup");
        _ = ResumeSessionAsync();
    });

    private async Task SaveFormCredentialsAsync()
    {
        if (credentialStore is null) return;
        try
        {
            if (RememberCredentials) await Task.Run(() => credentialStore.SaveAsync(SignInEmail.Trim(), SignInPassword));
            else await Task.Run(() => credentialStore.DeleteAsync());
        }
        catch (Exception) { Notice = "Your account could not be saved to the system credential store."; }
        if (!RememberCredentials) await ForgetSessionAsync();
    }

    /// <summary>Signs in with the Gameforge session kept from the last start, as the official client keeps its sign-in
    /// until Gameforge stops accepting it.</summary>
    private async Task ResumeSessionAsync()
    {
        if (sessionStore is null) return;
        SavedSession? saved;
        try { saved = await Task.Run(() => sessionStore.ReadAsync<SavedSession>()); }
        catch (Exception) { return; }
        if (saved is null) return;
        try
        {
            await CreateSignInAsync();
            SignInBusy = true;
            await signIn!.ResumeAsync(saved.Token);
            RefreshSignIn();
            var account = SignInAccounts.FirstOrDefault(item => item.Id == saved.AccountId) ?? (SignInAccounts.Count == 1 ? SignInAccounts[0] : null);
            if (account is not null) SelectAccount(account.Id);
            else SignInVisible = true;
        }
        catch (Exception error)
        {
            signIn?.Dispose(); signIn = null;
            RefreshSignIn();
            if (error is GameforgeAuthentication.SessionExpiredException) await ForgetSessionAsync();
            else Notice = "Your Gameforge sign-in could not be restored: " + error.Message;
        }
        finally { SignInBusy = false; }
    }

    private async Task KeepSessionAsync()
    {
        if (sessionStore is not { } store) return;
        await sessionWrites.WaitAsync();
        try
        {
            // Checked after the wait so a sign-out or Remember me change queued first is not undone.
            if (!RememberCredentials || signIn?.SessionToken is not { } token || selectedAccountId is not { } account) return;
            await Task.Run(() => store.SaveAsync("session", new SavedSession(token, account)));
        }
        catch (Exception) { Notice = "Your Gameforge sign-in could not be kept for the next start."; }
        finally { sessionWrites.Release(); }
    }

    private async Task ForgetSessionAsync()
    {
        if (sessionStore is not { } store) return;
        await sessionWrites.WaitAsync();
        try { await Task.Run(() => store.DeleteAsync()); }
        catch (Exception) { Notice = "Your Gameforge sign-in could not be removed from the system credential store."; }
        finally { sessionWrites.Release(); }
    }

    [RelayCommand]
    private async Task SignOutAsync()
    {
        signIn?.Dispose(); signIn = null; selectedAccountId = null;
        privatePassword = null;
        Coordinator?.ClearLaunchCode(); SignedInAs = ""; SignInError = "";
        SignInEmail = ""; SignInPassword = ""; SignInOtp = "";
        try { if (credentialStore is not null) await Task.Run(() => credentialStore.DeleteAsync()); }
        catch (Exception) { Notice = "Your saved account could not be removed from the system credential store."; }
        await ForgetSessionAsync();
        RefreshSignIn();
    }

    [RelayCommand]
    private async Task OpenSignInAsync()
    {
        SignInError = "";
        try
        {
            await CreateSignInAsync();
            SignInEmail = ""; SignInPassword = ""; SignInOtp = ""; SignInCaptchaAnswer = ""; SignInAccounts.Clear();
            SignInVisible = true;
            RefreshSignIn();
        }
        catch (Exception error) { Notice = error.Message; }
    }

    /// <summary>Signs in again with the saved account after Gameforge's session lapsed. False when the user has to step in,
    /// in which case the sign-in form shows what is needed.</summary>
    private async Task<bool> RenewSignInAsync(string accountId)
    {
        var saved = credentialStore is null ? null : await Task.Run(() => credentialStore.ReadAsync());
        SignedInAs = ""; selectedAccountId = null;
        if (saved is null)
        {
            signIn?.Dispose(); signIn = null;
            RefreshSignIn();
            return false;
        }
        SignInEmail = saved.Email;
        signIn?.Dispose(); signIn = null;
        var identity = IdentityRequested is null ? null : await IdentityRequested();
        signIn = Coordinator!.CreateSignIn(identity);
        await signIn.SignInAsync(saved.Email, saved.Password);
        RefreshSignIn();
        if (signIn.Stage == SignInStage.ChooseAccount && SignInAccounts.FirstOrDefault(account => account.Id == accountId) is { } account)
        {
            selectedAccountId = account.Id; SignedInAs = account.Name;
            await KeepSessionAsync();
            return true;
        }
        SignInError = signIn.ChallengeMessage ?? "";
        SignInVisible = true;
        return false;
    }

    private async Task CreateSignInAsync()
    {
        SignInBusy = true;
        try
        {
            signIn?.Dispose(); signIn = null;
            var identity = IdentityRequested is null ? null : await IdentityRequested();
            signIn = Coordinator!.CreateSignIn(identity);
        }
        finally { SignInBusy = false; }
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
                case SignInStage.NeedCredentials:
                    await signIn.SignInAsync(SignInEmail, SignInPassword);
                    break;
                case SignInStage.NeedOtp: await signIn.SubmitOtpAsync(SignInOtp); break;
                case SignInStage.NeedCaptcha:
                    if (!int.TryParse(SignInCaptchaAnswer, out var answer)) throw new InvalidOperationException("Enter the number of the matching image.");
                    await signIn.SubmitCaptchaAsync(answer); break;
            }
            SignInPassword = "";
            RefreshSignIn();
            SignInError = signIn.ChallengeMessage ?? "";
            if (signIn.Stage == SignInStage.ChooseAccount && SignInAccounts.Count == 1)
                SelectAccount(SignInAccounts[0].Id);
            else SignInVisible = signIn.Stage != SignInStage.NeedCredentials;
        }
        catch (Exception error) { SignInError = error.Message; }
        finally { SignInBusy = false; }
    }

    [RelayCommand]
    private Task ChooseAccountAsync(string id)
    {
        if (signIn is not null && !SignInBusy) SelectAccount(id);
        return Task.CompletedTask;
    }

    private void SelectAccount(string id)
    {
        selectedAccountId = id;
        SignedInAs = SignInAccounts.First(account => account.Id == id).Name;
        SignInVisible = false; SignInError = "";
        RefreshSignIn();
        _ = KeepSessionAsync();
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
                // Keep one revision for the whole image set. A drag begun on an old challenge cannot submit
                // an answer against refreshed images, and re-rendering account state does not invalidate it.
                if (CaptchaChoices.Count == 0 || displayedCaptcha != captcha || displayedCaptchaVersion != signIn.CaptchaVersion)
                {
                    foreach (var choice in CaptchaChoices) choice.Image.Dispose();
                    CaptchaChoices.Clear();
                    CaptchaPrompt?.Dispose(); CaptchaIcons?.Dispose(); CaptchaTarget?.Dispose();
                    CaptchaPrompt = ToBitmap(captcha.Prompt); CaptchaIcons = ToBitmap(captcha.Icons); CaptchaTarget = ToBitmap(captcha.Target);
                    displayedCaptcha = captcha; displayedCaptchaVersion = signIn.CaptchaVersion;
                    CaptchaRevision = Guid.NewGuid(); SignInCaptchaAnswer = "";
                    if (CaptchaIcons is { } strip)
                        for (var index = 0; index < 4; index++)
                        {
                            var left = index * strip.PixelSize.Width / 4;
                            var right = (index + 1) * strip.PixelSize.Width / 4;
                            CaptchaChoices.Add(new(index + 1, new CroppedBitmap(strip, new PixelRect(left, 0, right - left, strip.PixelSize.Height)), CaptchaRevision));
                        }
                }
            }
        }
        foreach (var name in new[] { nameof(StageCredentials), nameof(StageOtp), nameof(StageCaptcha), nameof(StageAccounts), nameof(SignInSubmitLabel), nameof(SignInSubmitVisible) })
            OnPropertyChanged(name);
    }

    private (string Prompt, string Icons, string Target)? displayedCaptcha;
    private string? displayedCaptchaVersion;

    private static Bitmap? ToBitmap(string dataUri)
    {
        var comma = dataUri.IndexOf(',');
        if (comma < 0) return null;
        try { return new Bitmap(new MemoryStream(Convert.FromBase64String(dataUri[(comma + 1)..]))); }
        catch { return null; }
    }
}
