using Avalonia;
using Xilium.CefGlue;
using Xilium.CefGlue.Common;

namespace NosCore.ClientLab.Launcher;

internal static class Program
{
    public static string[] Arguments { get; private set; } = [];
    public static string DataDirectory { get; private set; } = "";
    private static bool browserInitialized;

    [STAThread]
    public static void Main(string[] args)
    {
        if (args.FirstOrDefault() == "--recompile")
        {
            Environment.Exit(NosCore.ClientLab.Recompiler.Program.Main(args[1..]));
            return;
        }
        Arguments = args;
        DataDirectory = Option("--data-dir") ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NosCore.ClientLab");
        Directory.CreateDirectory(DataDirectory);
        if (args.Contains("--browser"))
        {
            RunBrowserAsync(args).GetAwaiter().GetResult();
            return;
        }
        try { BuildAvaloniaApp().StartWithClassicDesktopLifetime(args); }
        finally { if (browserInitialized) CefRuntime.Shutdown(); }
    }

    public static string? Option(string name)
    {
        var index = Array.IndexOf(Arguments, name);
        return index >= 0 && index + 1 < Arguments.Length ? Arguments[index + 1] : null;
    }

    private static async Task RunBrowserAsync(string[] args)
    {
        var port = int.TryParse(Option("--port"), out var configuredPort) ? configuredPort : 0;
        if (port is < 0 or > 65535) throw new ArgumentException("Choose a port between 1 and 65535, or 0 for an available port.");
        await using var server = new Services.BrowserLauncher(Services.SourceAssets.Resolve(), DataDirectory, port, !args.Contains("--no-prepare"));
        Console.WriteLine("ClientLab browser launcher: " + server.BrowserUrl);
        if (!args.Contains("--no-open"))
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(server.BrowserUrl) { UseShellExecute = true });
        using var stop = new CancellationTokenSource();
        ConsoleCancelEventHandler handler = (_, e) => { e.Cancel = true; stop.Cancel(); };
        Console.CancelKeyPress += handler;
        try { await Task.Delay(Timeout.Infinite, stop.Token); } catch (OperationCanceledException) { }
        finally { Console.CancelKeyPress -= handler; }
    }

    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect()
        .LogToTrace();

    public static void InitializeBrowser()
    {
        if (browserInitialized) return;
        var settings = new CefSettings
        {
            RootCachePath = Path.Combine(DataDirectory, "browser"),
            WindowlessRenderingEnabled = false
        };
        // Diagnostics only: CLIENTLAB_DEVTOOLS_PORT exposes the embedded pages to DevTools/CDP on loopback.
        if (int.TryParse(Environment.GetEnvironmentVariable("CLIENTLAB_DEVTOOLS_PORT"), out var devtools) && devtools is > 1024 and < 65536)
            settings.RemoteDebuggingPort = devtools;
        CefRuntimeLoader.Initialize(settings, flags:
        [
            // On dual-GPU machines Chromium renders on the integrated GPU unless asked; prefer the discrete one. Machines
            // without a usable GPU still get WebGL from Chromium's fallback or, failing that, the game's software rasteriser.
            new KeyValuePair<string, string>("force_high_performance_gpu", "1"),
            // The game advances one frame per page refresh. Chromium may judge a browser embedded in another window as
            // occluded or in the background and throttle its refreshes to a few per second; the game must never be.
            new KeyValuePair<string, string>("disable-features", "CalculateNativeWinOcclusion"),
            new KeyValuePair<string, string>("disable-backgrounding-occluded-windows", "1"),
            new KeyValuePair<string, string>("disable-renderer-backgrounding", "1"),
            new KeyValuePair<string, string>("disable-background-timer-throttling", "1"),
        ]);
        browserInitialized = true;
    }
}
