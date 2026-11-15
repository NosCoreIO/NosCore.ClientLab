using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using NosCore.ClientLab.Launcher.ViewModels;
using Xilium.CefGlue;
using Xilium.CefGlue.Avalonia;

namespace NosCore.ClientLab.Launcher;

public partial class MainWindow : Window
{
    private readonly MainViewModel model = new();
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private AvaloniaCefBrowser? browser;
    private bool browserInitialized;
    private bool closing;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = model;
        model.GameRequested += ShowGame;
        timer.Tick += async (_, _) => await model.RefreshAsync();
        Opened += async (_, _) => { await model.InitializeAsync(); timer.Start(); };
        Closing += async (_, e) =>
        {
            if (closing) return;
            e.Cancel = true;
            timer.Stop();
            try
            {
                browser?.Dispose();
                if (model.Coordinator is not null) await model.Coordinator.DisposeAsync();
                if (browserInitialized) CefRuntime.Shutdown();
            }
            finally { closing = true; Close(); }
        };
    }

    private void ShowGame(string url)
    {
        try
        {
            if (!browserInitialized) { Program.InitializeBrowser(); browserInitialized = true; }
            if (browser is null)
            {
                browser = new AvaloniaCefBrowser { Address = url };
                GameHost.Child = browser;
            }
            else if (browser.Address != url) browser.Address = url;
            LauncherPanel.IsVisible = false;
            GamePanel.IsVisible = true;
        }
        catch (Exception error) { model.Notice = "The embedded game could not start: " + error.Message; }
    }

    private void BackToLibrary(object? sender, RoutedEventArgs e)
    {
        GamePanel.IsVisible = false;
        LauncherPanel.IsVisible = true;
    }

    private async void CloseGame(object? sender, RoutedEventArgs e)
    {
        browser?.Dispose(); browser = null; GameHost.Child = null;
        BackToLibrary(sender, e);
        try { if (model.Coordinator is not null) await model.Coordinator.CloseGameAsync(); await model.RefreshAsync(); }
        catch (Exception error) { model.Notice = error.Message; }
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
}
