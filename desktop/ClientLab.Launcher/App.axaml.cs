using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace NosCore.ClientLab.Launcher;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);
    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Diagnostics: --game-url opens only the game window on a local game server, as Play would.
            if (Program.Option("--game-url") is { } url && Uri.TryCreate(url, UriKind.Absolute, out var game) && game.Host == "127.0.0.1")
            {
                Program.InitializeBrowser();
                desktop.MainWindow = new GameWindow(url, () => Task.CompletedTask);
            }
            else desktop.MainWindow = new MainWindow();
        }
        base.OnFrameworkInitializationCompleted();
    }
}
