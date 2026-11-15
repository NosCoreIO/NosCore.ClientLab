using Avalonia;
using Xilium.CefGlue;
using Xilium.CefGlue.Common;

namespace NosCore.ClientLab.Launcher;

internal static class Program
{
    public static string[] Arguments { get; private set; } = [];
    public static string DataDirectory { get; private set; } = "";

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
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static string? Option(string name)
    {
        var index = Array.IndexOf(Arguments, name);
        return index >= 0 && index + 1 < Arguments.Length ? Arguments[index + 1] : null;
    }

    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect()
        .LogToTrace();

    public static void InitializeBrowser()
    {
        CefRuntimeLoader.Initialize(new CefSettings
        {
            RootCachePath = Path.Combine(DataDirectory, "browser"),
            WindowlessRenderingEnabled = false
        });
    }
}
