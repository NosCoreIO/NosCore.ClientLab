using System.Diagnostics;
using NosCore.ClientLab.Launcher.Services;
using NosCore.ClientLab.Recompiler;

if (args is ["--child", var pidFile])
{
    await File.WriteAllTextAsync(pidFile, Environment.ProcessId.ToString());
    await Task.Delay(Timeout.Infinite);
    return;
}

static void Check(bool value, string message)
{
    if (!value) throw new Exception(message);
}

if (args is ["--game-settings"])
{
    await GameSettingsChecks.RunAsync();
    return;
}
if (args is ["--packet-logger"])
{
    PacketCaptureChecks.Run();
    WebPageChecks.Run();
    await PacketCaptureChecks.HostAsync();
    await PacketPluginChecks.RunAsync();
    return;
}

foreach (var path in new[] { "../escape", "/absolute", "C:\\absolute", "a//b", "a/./b", "a/b.", "a/b " })
{
    try { LauncherCoordinator.SafeRelativePath(path); throw new Exception("Accepted unsafe patch path: " + path); }
    catch (InvalidDataException) { }
}
Check(LauncherCoordinator.SafeRelativePath("NostaleData\\file.NOS") == "NostaleData/file.NOS", "Windows manifest path normalization failed.");

// Unaligned leaf methods using EDX/ECX were missed by the old text.Contains("a") heuristic.
foreach (var bytes in new[] { "9089d0c3", "9089c8c3", "90241fc3", "908b02c3" })
{
    var image = new ExecutableImage(Convert.FromHexString(bytes));
    image.RelocationTargets.Add(0x1001);
    Check(new FunctionDiscovery(image).RelocationSeeds([]).Contains(0x1001), "Missed a relocated Delphi argument-register leaf: " + bytes);
}
var junk = new ExecutableImage(Convert.FromHexString("9083c301c3")); // add ebx,1;ret: no argument register
junk.RelocationTargets.Add(0x1001);
Check(new FunctionDiscovery(junk).RelocationSeeds([]).Count == 0, "A mnemonic letter was mistaken for an argument register.");

var directory = Path.Combine(Path.GetTempPath(), "ClientLab-checks-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(directory);
List<int> children = [];
try
{
    await using var app = new LauncherCoordinator(directory, directory);
    using var cancel = new CancellationTokenSource();
    var pidFiles = Enumerable.Range(0, 3).Select(index => Path.Combine(directory, index + ".pid")).ToArray();
    var tasks = pidFiles.Select(file =>
    {
        var arguments = new List<string>();
        if (Path.GetFileNameWithoutExtension(Environment.ProcessPath) == "dotnet") arguments.Add(typeof(LauncherCoordinator).Assembly.Location.Replace("ClientLab.dll", "ClientLab.Checks.dll"));
        arguments.AddRange(["--child", file]);
        return app.RunAsync(Environment.ProcessPath!, arguments, cancel.Token);
    }).ToArray();
    var deadline = Stopwatch.StartNew();
    while (!pidFiles.All(File.Exists) && deadline.Elapsed < TimeSpan.FromSeconds(10)) await Task.Delay(50);
    Check(pidFiles.All(File.Exists), "Compiler process fixtures did not start.");
    foreach (var file in pidFiles) children.Add(int.Parse(await File.ReadAllTextAsync(file)));
    cancel.Cancel();
    try { await Task.WhenAll(tasks).WaitAsync(TimeSpan.FromSeconds(10)); throw new Exception("Cancellation was not reported."); }
    catch (OperationCanceledException) { }
    foreach (var pid in children)
    {
        try { using var child = Process.GetProcessById(pid); Check(child.HasExited, "Cancellation left a compiler running."); }
        catch (ArgumentException) { }
    }
}
finally
{
    foreach (var pid in children)
        try { using var child = Process.GetProcessById(pid); if (!child.HasExited) child.Kill(true); child.WaitForExit(5000); } catch (ArgumentException) { }
    // A process that just exited can hold its files for a moment on Windows.
    for (var attempt = 0; ; attempt++)
    {
        try { Directory.Delete(directory, recursive: true); break; }
        catch (IOException) when (attempt < 20) { await Task.Delay(100); }
    }
}
Console.WriteLine("PASS: manifest paths, relocated leaf discovery, and cancellation of parallel compiler processes.");
await CaptchaChecks.RunAsync();
await ProofOfWorkChecks.RunAsync();
await AuthFlowChecks.RunAsync();
await BrowserIdentityChecks.RunAsync();
await BuildWorkspaceChecks.RunAsync();
await GameSettingsChecks.RunAsync();
await RelayChecks.RunAsync();
DiscoveryChecks.Run();
await StartupChecks.RunAsync();
await AnalysisChecks.RunAsync();
NewsChecks.Run();
await CompilationCacheChecks.RunAsync();
await StructuredDispatchChecks.RunAsync();
VerifiedClientChecks.Run();
await GameFilesChecks.RunAsync();
await LauncherUpdateChecks.RunAsync();

if (OperatingSystem.IsWindows())
{
    // An isolated target in the real Windows vault; never reads or changes the user's launcher login.
    var vault = new CredentialStore(Path.Combine(Path.GetTempPath(), "ClientLab-vault-test-" + Guid.NewGuid().ToString("N")));
    try
    {
        Check(await vault.ReadAsync() is null, "A fresh credential target was not empty.");
        await vault.SaveAsync("test@example.invalid", "test-only-密碼-🔒");
        var credential = await vault.ReadAsync();
        Check(credential == new SavedCredentials("test@example.invalid", "test-only-密碼-🔒"), "Credential vault round-trip failed.");
        await vault.SaveAsync("other@example.invalid", "updated-test-password");
        Check((await vault.ReadAsync())?.Email == "other@example.invalid", "Credential vault update failed.");
        await vault.DeleteAsync();
        Check(await vault.ReadAsync() is null, "Credential vault delete failed.");
        Console.WriteLine("PASS: Windows credential store save, read, update and delete.");
    }
    finally { await vault.DeleteAsync(); }
}
