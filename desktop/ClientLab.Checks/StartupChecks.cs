using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class StartupChecks
{
    public static async Task RunAsync()
    {
        var directory = Path.Combine(Path.GetTempPath(), "ClientLab-startup-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var compiler = Path.Combine(directory, "compiler"); File.WriteAllText(compiler, "test marker");
            File.WriteAllText(Path.Combine(directory, "settings.json"), new JsonObject { ["provider"] = "gameforge", ["emcc"] = compiler }.ToJsonString());
            string id;
            await using (var check = new LauncherCoordinator(directory, directory, new PatchServer()))
            {
                await check.StartAndWaitAsync("check");
                id = check.Snapshot()["remote"]!["id"]!.ToString();
            }
            var executable = Path.Combine(directory, "previous.exe"); File.WriteAllText(executable, "initial invalid PE fixture");
            var runtime = Path.Combine(directory, "previous-runtime"); Directory.CreateDirectory(runtime);
            File.WriteAllText(Path.Combine(runtime, "nostale.js"), "previous runtime"); File.WriteAllText(Path.Combine(runtime, "nostale.wasm"), "previous wasm fixture");
            var active = new JsonObject { ["id"] = id, ["build"] = "previous", ["client"] = directory, ["runtime"] = runtime, ["executable"] = executable,
                ["executable_sha256"] = Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(executable))),
                ["runtime_revision"] = NosCore.ClientLab.Recompiler.Program.RuntimeRevision,
                ["source_version"] = "a previous launcher installation", ["toolchain_version"] = "a previous UI assembly" };
            File.WriteAllText(Path.Combine(directory, "active.json"), active.ToJsonString());
            File.Delete(compiler); // A ready runtime must not reinstall or invoke compilers.
            var unchanged = new PatchServer { ReverseOrder = true, DifferentCdn = true };
            await using (var app = new LauncherCoordinator(directory, directory, unchanged))
            {
                await app.StartAndWaitAsync("startup");
                await app.StartAndWaitAsync("startup");
                if (unchanged.Manifests != 2 || unchanged.Downloads != 0 || app.Snapshot()["stage"]!.ToString() != "ready")
                    throw new Exception("Startup did not check the patch manifest and reuse the unchanged complete build.");
            }
            string assetPatchId;
            var assetPatch = new PatchServer { AssetChanged = true };
            await using (var app = new LauncherCoordinator(directory, directory, assetPatch))
            {
                await app.StartAndWaitAsync("startup");
                var snapshot = app.Snapshot();
                assetPatchId = snapshot["active"]!["id"]!.ToString();
                if (assetPatch.Downloads != 2 || assetPatchId == id || snapshot["active"]!["runtime"]!.ToString() != runtime || File.Exists(compiler))
                    throw new Exception("An asset-only patch rebuilt the unchanged executable or did not install assets.");
            }
            File.WriteAllText(compiler, "test marker");
            var changed = new PatchServer { Changed = true };
            await using (var app = new LauncherCoordinator(directory, directory, changed))
            {
                // The changed fixture deliberately isn't PE: preparation must start on startup,
                // report the failed new build, and keep the previous runtime active.
                try { await app.StartAndWaitAsync("startup"); throw new Exception("Invalid changed executable was accepted."); }
                catch (InvalidOperationException) when (changed.Downloads == 2) { }
                if (app.Snapshot()["active"]!["id"]!.ToString() != assetPatchId || File.ReadAllText(Path.Combine(runtime, "nostale.js")) != "previous runtime")
                    throw new Exception("Failed startup preparation replaced the previous working build.");
            }
            var retried = new PatchServer { Changed = true };
            await using (var app = new LauncherCoordinator(directory, directory, retried))
            {
                try { await app.StartAndWaitAsync("startup"); throw new Exception("Invalid PE retry was accepted."); }
                catch (InvalidOperationException) when (retried.Downloads == 0) { }
                if (!app.Snapshot()["logs"]!.AsArray().Any(line => line!.ToString().Contains("already verified")))
                    throw new Exception("An interrupted build retry did not reuse verified patch files.");
            }
            Console.WriteLine("PASS: UI/source-path/CDN/order changes and asset-only patches do not recompile; changed executables prepare before Play and failures preserve the active build.");
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    private sealed class PatchServer : HttpMessageHandler
    {
        public bool Changed;
        public bool ReverseOrder, DifferentCdn;
        public bool AssetChanged;
        public int Manifests, Downloads;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
        {
            var contents = Encoding.UTF8.GetBytes(Changed ? "changed invalid PE fixture" : "initial invalid PE fixture");
            byte[] assetContents = AssetChanged ? [3, 4] : [1, 2];
            if (request.RequestUri!.Host == "spark.gameforge.com")
            {
                Manifests++;
                var exe = new JsonObject { ["file"] = "NostaleClientX.exe", ["sha1"] = Convert.ToHexStringLower(SHA1.HashData(contents)), ["path"] = DifferentCdn ? "/new-cdn/executable" : "/fixture/executable", ["size"] = contents.Length, ["folder"] = false };
                var asset = new JsonObject { ["file"] = "data.txt", ["sha1"] = Convert.ToHexStringLower(SHA1.HashData(assetContents)), ["path"] = "/fixture/asset", ["size"] = 2, ["folder"] = false };
                var json = new JsonObject { ["build"] = "fixture", ["entries"] = ReverseOrder ? new JsonArray(asset, exe) : new JsonArray(exe, asset) };
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(json.ToJsonString(), Encoding.UTF8, "application/json") });
            }
            if (request.RequestUri.Host != "patches.gameforge.com") throw new Exception("Unexpected startup request.");
            Downloads++;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(request.RequestUri.AbsolutePath.EndsWith("/asset") ? assetContents : contents) });
        }
    }
}
