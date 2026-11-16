using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json.Nodes;
using NosCore.ClientLab.Launcher.Services;

internal static class RelayChecks
{
    public static async Task RunAsync()
    {
        var directory = Path.Combine(Path.GetTempPath(), "ClientLab-relay-check-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var echo = new TcpListener(IPAddress.Loopback, 0); echo.Start();
        try
        {
            var requestBytes = Encoding.ASCII.GetBytes("original client socket bytes");
            var replyBytes = Encoding.ASCII.GetBytes("server reply bytes");
            var exchange = Task.Run(async () =>
            {
                using var peer = await echo.AcceptTcpClientAsync();
                var buffer = new byte[requestBytes.Length];
                await peer.GetStream().ReadExactlyAsync(buffer);
                if (!buffer.SequenceEqual(requestBytes)) throw new Exception("The relay changed outgoing bytes.");
                await peer.GetStream().WriteAsync(replyBytes);
            });
            await using var host = await GameHost.StartAsync(directory, new JsonObject {
                ["client"] = directory, ["executable"] = Path.Combine(directory, "fixture.exe"), ["runtime"] = directory
            }, new JsonObject { ["provider"] = "noscore" });
            using var http = new HttpClient { BaseAddress = new Uri(host.Url), Timeout = TimeSpan.FromSeconds(10) };
            http.DefaultRequestHeaders.Add("Sec-Fetch-Site", "same-origin");
            http.DefaultRequestHeaders.Add("Origin", host.Url.TrimEnd('/'));
            using var connect = await http.PostAsync("net/connect?host=127.0.0.1&port=" + ((IPEndPoint)echo.LocalEndpoint).Port, new ByteArrayContent([]));
            connect.EnsureSuccessStatusCode();
            var id = long.Parse(await connect.Content.ReadAsStringAsync());
            if (id <= 0 || id > int.MaxValue || (int)(double)id != id) throw new Exception("The connection ID cannot round-trip through the WebAssembly signed-int socket interface.");
            using var sent = await http.PostAsync("net/send?id=" + (int)(double)id, new ByteArrayContent(requestBytes));
            sent.EnsureSuccessStatusCode();
            byte[] received = [];
            for (var attempt = 0; attempt < 5 && received.Length == 0; attempt++)
            {
                using var response = await http.PostAsync("net/recv?id=" + (int)(double)id + "&wait=1", new ByteArrayContent([]));
                response.EnsureSuccessStatusCode(); received = await response.Content.ReadAsByteArrayAsync();
            }
            if (!received.SequenceEqual(replyBytes)) throw new Exception("The relay lost or changed the server response.");
            await exchange.WaitAsync(TimeSpan.FromSeconds(10));
            Console.WriteLine("PASS: socket IDs round-trip through JS/WASM signed ints and the C# relay exchanges exact bytes with a real TCP peer.");
        }
        finally { echo.Stop(); Directory.Delete(directory); }
    }
}
