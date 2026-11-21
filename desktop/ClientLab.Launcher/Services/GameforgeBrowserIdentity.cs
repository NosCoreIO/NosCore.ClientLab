using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading.Channels;

namespace NosCore.ClientLab.Launcher.Services;

// Runs the publisher's collector unchanged in the embedded browser. No credentials or
// session tokens enter this bridge, and no fingerprint fields are manufactured locally.
public sealed class GameforgeBrowserIdentity : IGameforgeIdentity, IGameforgeVerification, IDisposable
{
    private readonly HttpListener listener = new();
    private readonly CancellationTokenSource stopped = new();
    private readonly TaskCompletionSource<string> ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Channel<JsonObject> jobs = Channel.CreateUnbounded<JsonObject>();
    private readonly ConcurrentDictionary<string, TaskCompletionSource<string>> pending = new();
    private readonly ConcurrentDictionary<string, TaskCompletionSource<bool>> verifications = new();
    private readonly string prefix = "/" + Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(32)) + "/";
    private readonly string authority;
    private readonly string origin;
    private readonly string? storageFile;
    private readonly JsonObject initialStorage;
    public string Url { get; }
    public string InstallationId { get; }
    public Func<Task>? PrepareCollection { get; set; }
    public Action? CollectionFinished { get; set; }
    public string UserAgent => ready.Task.IsCompletedSuccessfully ? ready.Task.Result : throw new InvalidOperationException("Gameforge identity browser is still starting.");

    // officialClientIdentity is injectable so checks can exercise storage precedence without this machine's
    // real Gameforge client; production defaults to reading the official client's registered device identity.
    public GameforgeBrowserIdentity(string installation, string? dataDirectory = null, string? importedIdentity = null, Func<JsonObject?>? officialClientIdentity = null)
    {
        if (!Guid.TryParse(installation, out _)) throw new InvalidDataException("A registered Gameforge installation ID is required.");
        InstallationId = installation;
        storageFile = dataDirectory is null ? null : Path.Combine(dataDirectory, "gameforge-browser-state-" + installation + ".json");
        initialStorage = ReadStorage(storageFile, importedIdentity, installation, officialClientIdentity ?? GameforgeOfficialClientIdentity.TryRead);
        var socket = new TcpListener(IPAddress.Loopback, 0); socket.Start();
        var port = ((IPEndPoint)socket.LocalEndpoint).Port; socket.Stop();
        authority = $"127.0.0.1:{port}"; origin = "http://" + authority;
        Url = origin + prefix;
        listener.Prefixes.Add(origin + "/"); listener.Start();
        _ = ListenAsync();
    }

    public Task WaitUntilReadyAsync() => ready.Task.WaitAsync(TimeSpan.FromSeconds(35), stopped.Token);

    public async Task<string> CreateAsync(HttpClient http, JsonObject? request = null)
    {
        if (PrepareCollection is not null) await PrepareCollection();
        var id = Guid.NewGuid().ToString("N");
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        pending[id] = completion;
        try
        {
            await WaitUntilReadyAsync();
            await jobs.Writer.WriteAsync(new JsonObject { ["id"] = id, ["request"] = request?.DeepClone() }, stopped.Token);
            return await completion.Task.WaitAsync(TimeSpan.FromSeconds(25), stopped.Token);
        }
        finally { pending.TryRemove(id, out _); CollectionFinished?.Invoke(); }
    }

    public async Task CompleteProofOfWorkAsync(string challengeId)
    {
        if (!Guid.TryParse(challengeId, out _)) throw new InvalidDataException("Gameforge returned an invalid verification challenge ID.");
        if (PrepareCollection is not null) await PrepareCollection();
        var id = Guid.NewGuid().ToString("N");
        var completion = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        verifications[id] = completion;
        try
        {
            await WaitUntilReadyAsync();
            await jobs.Writer.WriteAsync(new JsonObject { ["kind"] = "pow", ["id"] = id, ["challengeId"] = challengeId }, stopped.Token);
            await completion.Task.WaitAsync(TimeSpan.FromSeconds(215), stopped.Token);
        }
        finally { verifications.TryRemove(id, out _); CollectionFinished?.Invoke(); }
    }

    private async Task ListenAsync()
    {
        try { while (!stopped.IsCancellationRequested) _ = HandleAsync(await listener.GetContextAsync()); }
        catch (Exception error) when (error is HttpListenerException or ObjectDisposedException) { }
    }

    private async Task HandleAsync(HttpListenerContext context)
    {
        try
        {
            var request = context.Request;
            if (request.UserHostName != authority || request.Url is null || !request.Url.AbsolutePath.StartsWith(prefix, StringComparison.Ordinal))
            { context.Response.StatusCode = 403; return; }
            var route = request.Url.AbsolutePath[prefix.Length..];
            if (route.Length == 0 && request.HttpMethod == "GET")
            {
                context.Response.ContentType = "text/html; charset=utf-8";
                context.Response.Headers["Cache-Control"] = "no-store";
                context.Response.Headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' https://gameforge.com https://pow-captcha.gameforge.com; frame-src https://pow-captcha.gameforge.com; connect-src 'self' https://gameforge.com https://*.gameforge.com; img-src 'self' data:; media-src 'self' data:; style-src 'unsafe-inline'";
                await context.Response.OutputStream.WriteAsync(Encoding.UTF8.GetBytes(Page.Replace("__STORAGE__", initialStorage.ToJsonString())));
                return;
            }
            if (request.Headers["Origin"] != origin && request.Headers["Referer"]?.StartsWith(Url, StringComparison.Ordinal) != true)
            { context.Response.StatusCode = 403; return; }
            if (route == "verification.js" && request.HttpMethod == "GET")
            {
                using var script = typeof(GameforgeBrowserIdentity).Assembly.GetManifestResourceStream("GameforgeVerification.js")!;
                context.Response.ContentType = "text/javascript; charset=utf-8";
                await script.CopyToAsync(context.Response.OutputStream, stopped.Token); return;
            }
            if (route == "next" && request.HttpMethod == "GET")
            {
                var job = await jobs.Reader.ReadAsync(stopped.Token);
                await ReplyAsync(context, job); return;
            }
            if (request.HttpMethod != "POST" || request.ContentLength64 is < 0 or > 32768)
            { context.Response.StatusCode = 400; return; }
            using var reader = new StreamReader(request.InputStream);
            var text = await reader.ReadToEndAsync(stopped.Token);
            if (text.Length > 32768) { context.Response.StatusCode = 413; return; }
            var data = JsonNode.Parse(text)!.AsObject();
            if (route == "ready") ready.TrySetResult(data["userAgent"]!.GetValue<string>());
            else if (route == "error") ready.TrySetException(new InvalidOperationException("Gameforge's identity script could not load. Check the connection and retry sign-in."));
            else if (route == "verification-result" && verifications.TryGetValue(data["id"]!.GetValue<string>(), out var verification))
            {
                var status = data["status"]?.GetValue<string>();
                if (status == "solved") verification.TrySetResult(true);
                else verification.TrySetException(new InvalidOperationException(status == "cancelled"
                    ? "Verification was cancelled. You can sign in again." : "Gameforge proof-of-work verification did not complete. Please retry sign-in."));
            }
            else if (route == "result" && pending.TryGetValue(data["id"]!.GetValue<string>(), out var completion))
            {
                if (storageFile is not null && data["storage"] is JsonObject storage)
                {
                    var saved = FilterStorage(storage);
                    if (saved.Count == 2)
                    {
                        var temporary = storageFile + "." + Guid.NewGuid().ToString("N") + ".tmp";
                        File.WriteAllText(temporary, saved.ToJsonString()); File.Move(temporary, storageFile, overwrite: true);
                    }
                }
                if (data["blackbox"]?.GetValue<string>() is { } blackbox && blackbox.StartsWith("tra:", StringComparison.Ordinal)) completion.TrySetResult(blackbox);
                else completion.TrySetException(new InvalidOperationException("Gameforge's identity script did not complete. Please retry sign-in."));
            }
            else { context.Response.StatusCode = 404; return; }
            await ReplyAsync(context, new JsonObject());
        }
        catch (Exception error) when (error is IOException or HttpListenerException or OperationCanceledException or System.Text.Json.JsonException or InvalidOperationException) { }
        finally { context.Response.Close(); }
    }

    private static Task ReplyAsync(HttpListenerContext context, JsonObject data)
    {
        context.Response.ContentType = "application/json";
        context.Response.Headers["Cache-Control"] = "no-store";
        return context.Response.OutputStream.WriteAsync(Encoding.UTF8.GetBytes(data.ToJsonString())).AsTask();
    }

    private const string Page = """
        <!doctype html><meta charset="utf-8"><title>ClientLab</title>
        <style>html,body{margin:0;min-height:100%;background:#142329;color:#edf5f0;font:16px system-ui,sans-serif}body{display:grid;place-items:center;height:100vh}main{text-align:center}p{color:#a5b9b5;font-size:14px}.pulse{display:inline-block;width:24px;height:24px;border:3px solid #38544d;border-top-color:#b9e8cc;border-radius:50%;animation:spin 1s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}</style>
        <main><span class="pulse"></span><h2>Connecting securely…</h2><p>This will only take a moment.</p></main>
        <script src="verification.js"></script>
        <script>
        const stored=__STORAGE__;
        for(const key of ['x-game','x-vec']) if(stored[key]&&!localStorage.getItem(key)) localStorage.setItem(key,stored[key]);
        const storage=()=>Object.fromEntries(['x-game','x-vec'].map(key=>[key,localStorage.getItem(key)]));
        const send=(path,body)=>fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
        async function start(){
          await send('ready',{userAgent:navigator.userAgent});
          while(true){
            const job=await (await fetch('next',{cache:'no-store'})).json();
            try{
              if(job.kind==='pow'){
                await ClientLabGameforgeVerification.run(job.challengeId);
                await send('verification-result',{id:job.id,status:'solved'});
                continue;
              }
              const blackbox=await new Promise((resolve,reject)=>{
                const timeout=setTimeout(()=>reject(new Error('Collection timed out')),18000);
                const done=value=>{
                  clearTimeout(timeout);
                  // The publisher's script can return an encoded error instead of a fingerprint.
                  try{const error=JSON.parse(atob(value));if(error && error.e){reject(new Error('Collector failed'));return;}}catch(error){}
                  resolve('tra:'+value);
                };
                if(job.request===null) game1(done); else game1(done,job.request);
              });
              await send('result',{id:job.id,blackbox,storage:storage()});
            }catch(error){
              if(job.kind==='pow') await send('verification-result',{id:job.id,status:error.name==='AbortError'?'cancelled':'failed'});
              else await send('result',{id:job.id,error:'collection failed'});
            }
          }
        }
        </script>
        <script src="https://gameforge.com/tra/game1.js" onload="start().catch(()=>send('error',{}))" onerror="send('error',{})"></script>
        """;

    private static JsonObject FilterStorage(JsonObject storage)
    {
        var result = new JsonObject();
        foreach (var key in new[] { "x-game", "x-vec" })
            if (storage[key] is JsonValue node && node.TryGetValue<string>(out var value) && value.Length is > 0 and <= 4096)
                result[key] = value;
        return result;
    }

    private static JsonObject ReadStorage(string? file, string? importedIdentity, string installation, Func<JsonObject?> officialClientIdentity)
    {
        JsonObject? stored = null;
        try
        {
            if (file is not null && File.Exists(file) && JsonNode.Parse(File.ReadAllText(file)) is JsonObject storage)
            { var filtered = FilterStorage(storage); if (filtered.Count == 2) stored = filtered; }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { }

        var official = officialClientIdentity() is { } raw && FilterStorage(raw) is { Count: 2 } valid ? valid : null;
        // The official Gameforge client's device id is the one registered with the server. A saved state file
        // bound to a different id was minted by this collector and will be refused at code creation (HTTP 403),
        // so the registered id wins; when they already agree, keep the saved file's live vector rotation.
        if (stored is not null)
        {
            if (official is not null && stored["x-game"]?.GetValue<string>() != official["x-game"]?.GetValue<string>()) return official;
            return stored;
        }
        if (!string.IsNullOrEmpty(importedIdentity))
        {
            var identity = JsonNode.Parse(File.ReadAllText(importedIdentity))!;
            if (identity["installationId"]?.GetValue<string>() == installation && identity["fingerprint"] is JsonArray fields && fields.Count == 30)
                return new JsonObject { ["x-game"] = fields[22]!.GetValue<string>(), ["x-vec"] = Encoding.ASCII.GetString(Convert.FromBase64String(fields[25]!.GetValue<string>())) };
        }
        // No saved state and no imported file: reuse the official client's registered identity rather than let
        // the collector mint an unregistered one (a guaranteed later HTTP 403).
        if (official is not null) return official;
        return new JsonObject();
    }

    public void Dispose()
    {
        stopped.Cancel(); listener.Close(); ready.TrySetCanceled();
        foreach (var item in pending.Values) item.TrySetCanceled();
        foreach (var item in verifications.Values) item.TrySetCanceled();
        jobs.Writer.TryComplete();
    }
}
