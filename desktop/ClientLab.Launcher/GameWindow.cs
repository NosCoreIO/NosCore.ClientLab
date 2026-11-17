using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Platform;
using Avalonia.Threading;
using Xilium.CefGlue.Avalonia;

namespace NosCore.ClientLab.Launcher;

public sealed class GameWindow : Window
{
    private const string StatsScript = "return window.gameExited ? 'exited' : window.gameStats ? JSON.stringify(window.gameStats) : ''";
    private static readonly Size DefaultChrome = new(16, 39);
    private readonly AvaloniaCefBrowser browser;
    private readonly Func<Task> closeRuntime;
    private readonly Action<int, int>? rememberResolution;
    private readonly DispatcherTimer statsTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private (int Width, int Height) applied;
    private Task? closeTask;
    private bool allowClose, polling, exitRequested;

    public GameWindow(string url, Func<Task> closeRuntime, (int Width, int Height)? resolution = null, Action<int, int>? rememberResolution = null)
    {
        this.closeRuntime = closeRuntime;
        this.rememberResolution = rememberResolution;
        Title = "NosTale";
        MinWidth = 640; MinHeight = 480;
        Background = Brushes.Black;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        var (width, height) = resolution ?? (1024, 768);
        var size = Fit(width, height, Screens.Primary, DefaultChrome);
        Width = size.Width; Height = size.Height;
        applied = resolution ?? default;
        browser = new AvaloniaCefBrowser { Address = url + (url.Contains('?') ? "&" : "?") + "desktop=1" };
        Content = browser;
        statsTimer.Tick += async (_, _) => await PollStatsAsync();
        Opened += (_, _) => statsTimer.Start();
        // Like the native client's close button, this asks the game to quit: it confirms with the player and saves its
        // in-game options before exiting, and the window follows it. Closing again while it asks closes at once.
        Closing += async (_, e) =>
        {
            if (allowClose) return;
            e.Cancel = true;
            if (!exitRequested && await RequestGameExitAsync()) { exitRequested = true; return; }
            await CloseGameAsync();
        };
    }

    public Task CloseGameAsync() => closeTask ??= CloseCoreAsync();

    private async Task CloseCoreAsync()
    {
        statsTimer.Stop();
        browser.Dispose(); Content = null;
        try { await closeRuntime(); }
        finally { allowClose = true; Close(); }
    }

    private async Task<bool> RequestGameExitAsync()
    {
        try { return await browser.EvaluateJavaScript<bool>("return window.closeGame ? window.closeGame() : false"); }
        catch (Exception) { return false; /* the page never loaded or is already gone */ }
    }

    // The page publishes the frames it showed in the last second and the size the game rendered at.
    private async Task PollStatsAsync()
    {
        if (polling || allowClose) return;
        polling = true;
        try
        {
            var json = await browser.EvaluateJavaScript<string>(StatsScript);
            if (json == "exited") { _ = CloseGameAsync(); return; }
            if (string.IsNullOrEmpty(json)) return;
            using var stats = JsonDocument.Parse(json);
            var fps = stats.RootElement.GetProperty("fps").GetInt32();
            var width = stats.RootElement.GetProperty("w").GetInt32();
            var height = stats.RootElement.GetProperty("h").GetInt32();
            Title = $"NosTale  ·  {fps} fps";
            if (width >= 320 && height >= 240 && (width, height) != applied)
            {
                applied = (width, height);
                ApplyResolution(width, height);
                rememberResolution?.Invoke(width, height);
            }
        }
        catch (Exception) { /* the page is still loading or navigating */ }
        finally { polling = false; }
    }

    // One game pixel per physical screen pixel when it fits, so small text stays sharp; otherwise the largest size
    // that fits the screen. A window the player maximised or resized keeps its size.
    private void ApplyResolution(int width, int height)
    {
        if (WindowState != WindowState.Normal) return;
        var screen = Screens.ScreenFromWindow(this) ?? Screens.Primary;
        var chrome = FrameSize is { } frame ? new Size(frame.Width - ClientSize.Width, frame.Height - ClientSize.Height) : DefaultChrome;
        var size = Fit(width, height, screen, chrome);
        Width = size.Width; Height = size.Height;
        if (screen is null) return;
        var area = screen.WorkingArea;
        var frameWidth = (int)Math.Ceiling((size.Width + chrome.Width) * screen.Scaling);
        var frameHeight = (int)Math.Ceiling((size.Height + chrome.Height) * screen.Scaling);
        Position = new PixelPoint(area.X + Math.Max(0, (area.Width - frameWidth) / 2), area.Y + Math.Max(0, (area.Height - frameHeight) / 2));
    }

    private static Size Fit(int width, int height, Screen? screen, Size chrome)
    {
        var scaling = screen?.Scaling ?? 1;
        double w = width / scaling, h = height / scaling;
        if (screen is not null)
        {
            var maxWidth = screen.WorkingArea.Width / scaling - chrome.Width;
            var maxHeight = screen.WorkingArea.Height / scaling - chrome.Height;
            if (w > maxWidth || h > maxHeight)
            {
                var shrink = Math.Min(maxWidth / w, maxHeight / h);
                w *= shrink; h *= shrink;
            }
        }
        return new Size(Math.Floor(w), Math.Floor(h));
    }
}
