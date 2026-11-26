using Avalonia.Controls;
using Avalonia;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using NosCore.ClientLab.Launcher.ViewModels;
using NosCore.ClientLab.Launcher.Services;
using Xilium.CefGlue;
using Xilium.CefGlue.Avalonia;

namespace NosCore.ClientLab.Launcher;

public partial class MainWindow : Window
{
    private readonly MainViewModel model = new();
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private GameWindow? gameWindow;
    private Window? packetWindow;
    private bool browserInitialized;
    private AvaloniaCefBrowser? identityBrowser;
    private GameforgeBrowserIdentity? identityCollector;
    private bool closing;
    private static readonly DataFormat<string> CaptchaFormat = DataFormat.CreateStringApplicationFormat("ClientLab.CaptchaChoice");
    private MainViewModel.CaptchaChoice? captchaPointerChoice;
    private MainViewModel.CaptchaChoice? activeCaptchaDrag;
    private Point captchaPointerStart;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = model;
        CaptchaDropTarget.AddHandler(DragDrop.DragOverEvent, CaptchaDragOver);
        CaptchaDropTarget.AddHandler(DragDrop.DropEvent, CaptchaDrop);
        model.GameRequested += ShowGame;
        model.IdentityRequested = CreateIdentityAsync;
        model.RestartRequested += Close;
        timer.Tick += async (_, _) => await model.RefreshAsync();
        Opened += async (_, _) =>
        {
            // Chromium downloads in the background on first start, usually before the player needs it.
            _ = BrowserRuntime.EnsureAsync(Program.DataDirectory).ContinueWith(_ => { }, TaskScheduler.Default);
            await model.InitializeAsync(); timer.Start();
        };
        Closing += async (_, e) =>
        {
            if (closing) return;
            e.Cancel = true;
            timer.Stop();
            try
            {
                if (gameWindow is not null) await gameWindow.CloseGameAsync();
                  packetWindow?.Close();
                identityBrowser?.Dispose(); identityCollector?.Dispose();
                model.DisposeLauncherUpdates();
                if (model.Coordinator is not null) await model.Coordinator.DisposeAsync();
            }
            finally { closing = true; Close(); }
        };
    }

    /// <summary>Waits for Chromium (downloaded once, BrowserRuntime) and starts it, showing the download meanwhile.</summary>
    private async Task EnsureBrowserAsync()
    {
        if (browserInitialized) return;
        if (!BrowserRuntime.IsReady(AppContext.BaseDirectory))
        {
            const string downloading = "Downloading the game browser (first start only)…";
            model.Notice = downloading;
            var shown = -1;
            try
            {
                await BrowserRuntime.EnsureAsync(Program.DataDirectory, value =>
                {
                    var percent = (int)(value * 100);
                    if (percent == Interlocked.Exchange(ref shown, percent)) return;
                    Dispatcher.UIThread.Post(() => { if (model.Notice.StartsWith(downloading[..^1])) model.Notice = $"{downloading} {percent}%"; });
                });
            }
            catch (Exception error) { throw new InvalidOperationException("The game browser could not be downloaded: " + error.Message, error); }
            if (model.Notice.StartsWith(downloading[..^1])) model.Notice = "";
        }
        Program.InitializeBrowser();
        browserInitialized = true;
    }

    private async Task<IGameforgeIdentity> CreateIdentityAsync()
    {
        if (identityCollector is not null) return identityCollector;
        await EnsureBrowserAsync();
        var collector = model.Coordinator!.CreateBrowserIdentity();
        collector.PrepareCollection = async () =>
        {
            await Dispatcher.UIThread.InvokeAsync(() => IdentityBrowserHost.IsVisible = true);
            await Dispatcher.UIThread.InvokeAsync(() => { }, DispatcherPriority.Loaded);
            await Task.Delay(100); // Let CEF apply the real host bounds before the publisher collects them.
        };
        collector.CollectionFinished = () => Dispatcher.UIThread.Post(() => IdentityBrowserHost.IsVisible = false);
        try
        {
            IdentityBrowserHost.IsVisible = true;
            identityBrowser = new AvaloniaCefBrowser { Address = collector.Url }.OpeningPopupsExternally();
            IdentityBrowserHost.Child = identityBrowser;
            await collector.WaitUntilReadyAsync();
            identityCollector = collector;
            return collector;
        }
        catch
        {
            identityBrowser?.Dispose(); identityBrowser = null; IdentityBrowserHost.Child = null;
            IdentityBrowserHost.IsVisible = false;
            collector.Dispose(); throw;
        }
    }

    private void CaptchaPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (model.SignInBusy || sender is not Control { DataContext: MainViewModel.CaptchaChoice choice } control || !e.GetCurrentPoint(control).Properties.IsLeftButtonPressed) return;
        captchaPointerChoice = choice;
        captchaPointerStart = e.GetPosition(this);
        e.Pointer.Capture(control);
        e.Handled = true;
    }

    private async void CaptchaPointerMoved(object? sender, PointerEventArgs e)
    {
        if (captchaPointerChoice is not { } choice || model.SignInBusy || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        var offset = e.GetPosition(this) - captchaPointerStart;
        if (Math.Abs(offset.X) < 6 && Math.Abs(offset.Y) < 6) return;
        captchaPointerChoice = null;
        activeCaptchaDrag = choice;
        e.Pointer.Capture(null);
        var data = new DataTransfer();
        var item = new DataTransferItem(); item.Set(CaptchaFormat, choice.Revision.ToString("N") + ":" + choice.Number);
        data.Add(item);
        try { await DragDrop.DoDragDropAsync(e, data, DragDropEffects.Copy); }
        catch (Exception) { model.SignInError = "The drag could not start. Click the number beneath the item instead."; }
        finally { activeCaptchaDrag = null; }
    }

    private void CaptchaPointerReleased(object? sender, PointerReleasedEventArgs e)
    { captchaPointerChoice = null; e.Pointer.Capture(null); }

    private bool IsCurrentCaptchaDrag(DragEventArgs e) => !model.SignInBusy && model.StageCaptcha && activeCaptchaDrag is { } choice &&
        choice.Revision == model.CaptchaRevision && e.DataTransfer.TryGetValue(CaptchaFormat) == choice.Revision.ToString("N") + ":" + choice.Number;

    private void CaptchaDragOver(object? sender, DragEventArgs e)
    { e.DragEffects = IsCurrentCaptchaDrag(e) ? DragDropEffects.Copy : DragDropEffects.None; e.Handled = true; }

    private async void CaptchaDrop(object? sender, DragEventArgs e)
    {
        e.Handled = true;
        if (!IsCurrentCaptchaDrag(e) || activeCaptchaDrag is not { } choice) { e.DragEffects = DragDropEffects.None; return; }
        e.DragEffects = DragDropEffects.Copy;
        activeCaptchaDrag = null;
        await model.DropCaptchaAsync(choice.Number, choice.Revision);
    }

    private async void ShowGame(string url)
    {
        try
        {
            await EnsureBrowserAsync();
            if (gameWindow is null)
            {
                var coordinator = model.Coordinator;
                var options = coordinator is null ? new GameOptions() : await coordinator.ReadGameOptionsAsync();
                gameWindow = new GameWindow(url, async () =>
                {
                    gameWindow = null;
                    try { if (model.Coordinator is not null) await model.Coordinator.CloseGameAsync(); await model.RefreshAsync(); }
                    catch (Exception error) { model.Notice = error.Message; }
                }, coordinator?.LastGameResolution(), (width, height) => coordinator?.RememberGameResolution(width, height)) {
                    Icon = Icon, WindowState = options.FullScreen ? WindowState.FullScreen : WindowState.Normal };
                gameWindow.Show();
              if (model.PacketLogger && string.IsNullOrWhiteSpace(model.PacketPlugin))
              {
                  packetWindow?.Close();
                  var packetBrowser = new AvaloniaCefBrowser { Address = new Uri(new Uri(url), "/packet-log").AbsoluteUri }.OpeningPopupsExternally();
                  packetWindow = new Window { Title = "ClientLab · Packet logger", Width = 1000, Height = 650, Icon = Icon, Content = packetBrowser };
                  packetWindow.Closed += (_, _) => packetBrowser.Dispose();
                  packetWindow.Show();
              }
            }
            gameWindow.Activate();
        }
        catch (Exception error) { model.Notice = "The embedded game could not start: " + error.Message; }
    }

    private async void BrowseImport(object? sender, RoutedEventArgs e)
    {
        var result = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Choose existing NosTale installation", AllowMultiple = false });
        if (result.Count > 0) model.ImportDirectory = result[0].TryGetLocalPath() ?? "";
    }

    private async void BrowseIdentity(object? sender, RoutedEventArgs e)
    {
        var result = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose captured Gameforge identity", AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("Gameforge identity") { Patterns = ["*.json"] }]
        });
        if (result.Count > 0) model.IdentityFile = result[0].TryGetLocalPath() ?? "";
    }

    private async void BrowsePacketPlugin(object? sender, RoutedEventArgs e)
    {
        var result = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose a packet logger plugin", AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("Packet logger plugin") { Patterns = ["*.dll"] }]
        });
        if (result.Count > 0) model.PacketPlugin = result[0].TryGetLocalPath() ?? "";
    }

    private void ClearPacketPlugin(object? sender, RoutedEventArgs e) => model.PacketPlugin = "";
}
