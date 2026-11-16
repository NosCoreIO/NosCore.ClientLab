using System.Net;
using NosCore.ClientLab.Launcher.Services;

internal static class AnalysisChecks
{
    private const string Published = "0123456789abcdef0123456789abcdef";
    private const string Missing = "fedcba9876543210fedcba9876543210";

    public static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "ClientLab-analysis-check-" + Guid.NewGuid().ToString("N"));
        var source = Path.Combine(root, "source"); var data = Path.Combine(root, "data");
        Directory.CreateDirectory(Path.Combine(source, "symbols", "11111111111111111111111111111111"));
        File.WriteAllText(Path.Combine(source, "symbols", "11111111111111111111111111111111", "ghidra-functions.txt"), "401000 bundled 401000-401010\n");
        Directory.CreateDirectory(data);
        try
        {
            var server = new Server();
            await using (var app = new LauncherCoordinator(source, data, server))
            {
                Check(await app.AnalysisDirectoryAsync("11111111111111111111111111111111", CancellationToken.None) == Path.Combine(source, "symbols", "11111111111111111111111111111111") && server.Requests == 0,
                    "A bundled analysis was not used directly.");
                var fetched = await app.AnalysisDirectoryAsync(Published, CancellationToken.None);
                Check(fetched == Path.Combine(data, "symbols", Published) && File.ReadAllText(Path.Combine(fetched, "ghidra-functions.txt")).Contains("published"),
                    "The published analysis was not fetched into the data directory.");
                var requests = server.Requests;
                Check(await app.AnalysisDirectoryAsync(Published, CancellationToken.None) == fetched && server.Requests == requests, "A fetched analysis was downloaded again.");
                Check(await app.AnalysisDirectoryAsync(Missing, CancellationToken.None) is null && !Directory.Exists(Path.Combine(data, "symbols", Missing)),
                    "A missing analysis left files behind or was reported as available.");
                Check(await app.AnalysisDirectoryAsync("../../etc", CancellationToken.None) is null && server.Requests == requests + 1, "A malformed build id reached the network.");
            }
            Console.WriteLine("PASS: client analysis comes from the bundle, then the repository (once, validated), and a build not yet analysed is reported without leftovers.");
        }
        finally { Directory.Delete(root, recursive: true); }
    }

    private static void Check(bool condition, string error) { if (!condition) throw new Exception(error); }

    private sealed class Server : HttpMessageHandler
    {
        public int Requests;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Requests++;
            var url = request.RequestUri!.ToString();
            Check(url.StartsWith(LauncherCoordinator.AnalysisUrl), "Analysis was requested from an unexpected address: " + url);
            if (url.EndsWith(Published + "/ghidra-functions.txt"))
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent("401000 published 401000-401010\n") });
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound));
        }
    }
}
