using System.Text;
using NosCore.ClientLab.Launcher.Services;
using NosCore.ClientLab.Launcher.ViewModels;

internal static class GameSettingsChecks
{
    public static async Task RunAsync()
    {
        var prefix = Path.Combine(Path.GetTempPath(), "ClientLab-settings-check-");
        var root = prefix + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(root);
        try
        {
            var preparing = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var resume = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            string? preparationCompiler = null;
            LauncherCoordinator? preparingApp = null;
            using var handler = new SettingsRequestHandler(async () =>
            {
                preparing.TrySetResult();
                await resume.Task;
                preparationCompiler = (string?)typeof(LauncherCoordinator).GetMethod("Setting", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(preparingApp, ["emcc"]);
                return new HttpResponseMessage(System.Net.HttpStatusCode.ServiceUnavailable);
            });
            await using (preparingApp = new LauncherCoordinator(root, Path.Combine(root, "busy-settings"), handler))
            {
                preparingApp.SaveSettings(new System.Text.Json.Nodes.JsonObject { ["emcc"] = "old-compiler" });
                preparingApp.Start("check");
                await preparing.Task.WaitAsync(TimeSpan.FromSeconds(10));
                var model = new MainViewModel { Initialized = true, Busy = true, Running = true };
                if (!model.CanSaveSettings) throw new Exception("Save settings is disabled during preparation/gameplay.");
                preparingApp.SaveSettings(new System.Text.Json.Nodes.JsonObject { ["packet_logger"] = "true", ["emcc"] = "new-compiler" });
                resume.TrySetResult();
                await preparingApp.CancelAndWaitAsync();
                if (preparationCompiler != "old-compiler") throw new Exception("Saving settings changed an in-progress preparation's configuration.");
                if (preparingApp.Snapshot()["settings"]!["emcc"]!.GetValue<string>() != "new-compiler") throw new Exception("Settings changed during preparation were not retained.");
                var saved = System.Text.Json.Nodes.JsonNode.Parse(await File.ReadAllTextAsync(Path.Combine(root, "busy-settings", "settings.json")))!;
                if (saved["packet_logger"]!.GetValue<string>() != "true") throw new Exception("Packet logger preference was not saved during preparation.");
            }
            var toggleData = Path.Combine(root, "toggle-data");
            await using (var coordinator = new LauncherCoordinator(root, toggleData))
            {
                var model = new MainViewModel();
                typeof(MainViewModel).GetProperty(nameof(MainViewModel.Coordinator))!.SetValue(model, coordinator);
                model.Initialized = true;
                model.GameMusic = false;
                model.GameSoundEffects = false;
                model.GameMusic = true;
                model.GameMusic = false;
                await model.FlushGamePreferencesAsync();
                var savedOptions = await new GameSettingsStore(toggleData).ReadOptionsAsync();
                if (savedOptions.Music || savedOptions.SoundEffects)
                    throw new Exception("Toggling launcher preferences did not save immediately, or rapid edits were reordered.");
                if (!model.GamePreferencesStatus.StartsWith("Saved.")) throw new Exception("Preference save was not confirmed.");
            }
            var originalDirectory = Path.Combine(root, "official-client");
            Directory.CreateDirectory(originalDirectory);
            var original = Encoding.UTF8.GetBytes("; keep comments and unknown keys\r\n[EW_Sound_Config]\r\nBGMSoundOff=0\r\nFxSoundOff=1\r\n3DSoundOff=0\r\n[EW_Multi_Navigator]\r\nFullScreen=1\r\nScreenSizeMode=2\r\nCustomValue=original\r\n");
            var originalPath = Path.Combine(originalDirectory, "Config.ini");
            await File.WriteAllBytesAsync(originalPath, original);
            var dataDirectory = Path.Combine(root, "app-data");
            var store = new GameSettingsStore(dataDirectory);
            if (await store.ReadAsync() is not null) throw new Exception("Fresh game settings were not empty.");
            if (!(await store.ReadAsync([originalDirectory]))!.SequenceEqual(original)) throw new Exception("Official settings bytes changed during import.");
            var changed = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(original).Replace("BGMSoundOff=0", "BGMSoundOff=1").Replace("FullScreen=1", "FullScreen=0"));
            await store.WriteAsync(changed);
            var nextClientVersion = Path.Combine(root, "updated-client"); Directory.CreateDirectory(nextClientVersion);
            await File.WriteAllBytesAsync(Path.Combine(nextClientVersion, "Config.ini"), original);
            var reopened = new GameSettingsStore(dataDirectory);
            if (!(await reopened.ReadAsync([nextClientVersion]))!.SequenceEqual(changed)) throw new Exception("A patch or a new launcher instance reset saved music/display options.");
            if (!(await File.ReadAllBytesAsync(originalPath)).SequenceEqual(original)) throw new Exception("Saving ClientLab preferences modified the user's official installation.");
            try { await reopened.WriteAsync(new byte[GameSettingsStore.MaximumBytes + 1]); throw new Exception("Oversized settings were accepted."); }
            catch (InvalidDataException) { }
            if (!(await reopened.ReadAsync())!.SequenceEqual(changed)) throw new Exception("A rejected write damaged the previous preferences.");
            if (await reopened.ReadOptionsAsync() != new GameOptions(false, false, true, false))
                throw new Exception("The launcher did not read the official SoundOff flags correctly.");
            var selected = new GameOptions(true, true, false, true);
            await reopened.UpdateOptionsAsync(selected);
            if (await new GameSettingsStore(dataDirectory).ReadOptionsAsync() != selected)
                throw new Exception("Launcher game preferences did not survive reopening.");
            var edited = Encoding.UTF8.GetString((await reopened.ReadAsync())!);
            if (!edited.Contains("BGMSoundOff=0\r\n") || !edited.Contains("FxSoundOff=0\r\n") || !edited.Contains("3DSoundOff=1\r\n") ||
                !edited.Contains("FullScreen=1\r\n") || !edited.Contains("CustomValue=original\r\n") || !edited.StartsWith("; keep comments and unknown keys\r\n"))
                throw new Exception("Editing launcher preferences changed unrelated settings or wrote inverted audio flags.");
            var unicode = "; café\r\n[EW_Sound_Config]\n BGMSoundOff = False  ; music\r\n[Other]\r\nUnknown=keep";
            await reopened.WriteAsync(Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes(unicode)).ToArray());
            await reopened.UpdateOptionsAsync(new GameOptions(false, true, true, false));
            var unicodeBytes = (await reopened.ReadAsync())!;
            var unicodeText = Encoding.Unicode.GetString(unicodeBytes.AsSpan(2));
            if (!unicodeBytes.AsSpan().StartsWith(Encoding.Unicode.GetPreamble()) || !unicodeText.Contains("; café\r\n") ||
                !unicodeText.Contains(" BGMSoundOff = 1  ; music\r\n") || !unicodeText.Contains("Unknown=keep"))
                throw new Exception("Launcher settings editing damaged Unicode, comments, whitespace, or unrelated sections.");
            var writers = Enumerable.Range(1, 12).Select(index => new GameSettingsStore(dataDirectory).WriteAsync(Enumerable.Repeat((byte)index, 32768).ToArray()));
            await Task.WhenAll(writers);
            var complete = (await reopened.ReadAsync())!;
            if (complete.Length != 32768 || complete.Any(value => value != complete[0])) throw new Exception("Concurrent launcher instances produced a partial settings file.");
            if (Directory.EnumerateFiles(Path.GetDirectoryName(store.FilePath)!, "*.tmp").Any()) throw new Exception("A settings write left temporary files behind.");
            await reopened.WriteAsync(Array.Empty<byte>());
            if ((await new GameSettingsStore(dataDirectory).ReadAsync())?.Length != 0) throw new Exception("An intentionally empty preferences file was replaced by imported defaults.");
            Console.WriteLine("PASS: exact official INI import, music/display persistence across launches and patches, original-install preservation, atomic concurrent writes and bounded settings files.");
            Console.WriteLine("PASS: launcher options round-trip through the official INI flags while preserving other keys, Unicode, comments and line endings.");
            Console.WriteLine("PASS: Save stays enabled during preparation/gameplay; settings persist without changing the active preparation's snapshot.");
        }
        finally
        {
            var full = Path.GetFullPath(root);
            if (!full.StartsWith(Path.GetFullPath(prefix), StringComparison.OrdinalIgnoreCase)) throw new Exception("Unsafe test cleanup path.");
            Directory.Delete(full, recursive: true);
        }
    }

    private sealed class SettingsRequestHandler(Func<Task<HttpResponseMessage>> request) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage message, CancellationToken token) => request();
    }
}
