using System.Collections.Concurrent;
using System.Globalization;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json.Nodes;
using System.Threading.Channels;
using NosCore.ClientLab.Plugins;
using NosCore.ClientLab.Recompiler;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Requests from a plugin to the running game. The game page fetches them from the game host (/plugin/next),
/// hands each one to the game thread through a shared buffer and posts the answer back (/plugin/answer).
/// </summary>
public sealed class GameBridge : IGameSession
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);
    private readonly Channel<JsonObject> requests = Channel.CreateUnbounded<JsonObject>(new UnboundedChannelOptions { SingleReader = false });
    private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonNode?>> pending = new();
    private readonly Action<string>? log;
    private readonly Lazy<ExecutableImage> image;
    private long sequence;

    public GameBridge(string clientExecutable, string packetLogFile, Action<string>? log)
    {
        ClientExecutable = clientExecutable;
        PacketLogFile = packetLogFile;
        this.log = log;
        image = new Lazy<ExecutableImage>(() => new ExecutableImage(clientExecutable));
    }

    public string ClientExecutable { get; }
    public string PacketLogFile { get; }

    public void Log(string message) => log?.Invoke("Packet plugin: " + message);

    public uint? FindCode(string pattern) => FindUnique(image.Value, pattern);

    /// <summary>The single match of a byte pattern ('?' or '??' for any byte) in the image's code, or null.</summary>
    public static uint? FindUnique(ExecutableImage image, string pattern)
    {
        var tokens = pattern.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (tokens.Length == 0) throw new ArgumentException("The pattern is empty.", nameof(pattern));
        var bytes = new byte[tokens.Length];
        var known = new bool[tokens.Length];
        for (var i = 0; i < tokens.Length; i++)
        {
            if (tokens[i] is "?" or "??") continue;
            if (tokens[i].Length != 2 || !byte.TryParse(tokens[i], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out bytes[i]))
                throw new ArgumentException("Pattern bytes are two hex digits or '??': " + tokens[i], nameof(pattern));
            known[i] = true;
        }
        var anchor = Array.IndexOf(known, true);
        if (anchor < 0) throw new ArgumentException("The pattern has no fixed byte.", nameof(pattern));
        var code = image.Code;
        uint? found = null;
        for (var at = 0; at <= code.Length - bytes.Length;)
        {
            var relative = code.AsSpan(at + anchor, code.Length - bytes.Length - at + 1).IndexOf(bytes[anchor]);
            if (relative < 0) break;
            var start = at + relative;
            var match = true;
            for (var i = 0; i < bytes.Length && match; i++) match = !known[i] || code[start + i] == bytes[i];
            if (match)
            {
                if (found is not null) return null;
                found = image.CodeStart + (uint)start;
            }
            at = start + 1;
        }
        return found;
    }

    public async Task<byte[]> ReadAsync(uint address, int length, CancellationToken cancellationToken = default)
    {
        if (length is < 0 or > 1 << 20) throw new ArgumentOutOfRangeException(nameof(length), "Read at most 1 MB at a time.");
        if (length == 0) return [];
        var result = await RequestAsync(new JsonObject { ["op"] = "read", ["address"] = address, ["length"] = length }, cancellationToken);
        return Convert.FromHexString(result?["hex"]?.GetValue<string>() ?? throw new InvalidDataException("The game did not return memory."));
    }

    public async Task WriteAsync(uint address, ReadOnlyMemory<byte> data, CancellationToken cancellationToken = default)
    {
        if (data.Length > 1 << 20) throw new ArgumentOutOfRangeException(nameof(data), "Write at most 1 MB at a time.");
        if (data.Length == 0) return;
        await RequestAsync(new JsonObject { ["op"] = "write", ["address"] = address, ["hex"] = Convert.ToHexString(data.Span) }, cancellationToken);
    }

    public async Task<uint> CallAsync(uint function, uint eax = 0, uint edx = 0, uint ecx = 0, IReadOnlyList<uint>? stack = null,
        CancellationToken cancellationToken = default)
    {
        if (stack is { Count: > 3 }) throw new ArgumentException("At most three stack arguments are supported.", nameof(stack));
        var arguments = new JsonArray();
        foreach (var value in stack ?? []) arguments.Add(value);
        var result = await RequestAsync(new JsonObject { ["op"] = "call", ["address"] = function, ["eax"] = eax, ["edx"] = edx, ["ecx"] = ecx, ["stack"] = arguments }, cancellationToken);
        return result?["eax"]?.GetValue<uint>() ?? 0;
    }

    public Task<bool> SendPacketAsync(string packet, CancellationToken cancellationToken = default) => PacketAsync(0, packet, cancellationToken);
    public Task<bool> ReceivePacketAsync(string packet, CancellationToken cancellationToken = default) => PacketAsync(1, packet, cancellationToken);

    private async Task<bool> PacketAsync(int kind, string packet, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(packet) || packet.Length > 65536) throw new ArgumentException("A packet is 1 to 65536 characters.", nameof(packet));
        var result = await RequestAsync(new JsonObject { ["op"] = "packet", ["kind"] = kind, ["raw"] = packet }, cancellationToken);
        return result?["ok"]?.GetValue<bool>() == true;
    }

    private async Task<JsonNode?> RequestAsync(JsonObject request, CancellationToken cancellationToken)
    {
        var id = Interlocked.Increment(ref sequence);
        request["id"] = id;
        var answer = new TaskCompletionSource<JsonNode?>(TaskCreationOptions.RunContinuationsAsynchronously);
        pending[id] = answer;
        try
        {
            await requests.Writer.WriteAsync(request, cancellationToken);
            return await answer.Task.WaitAsync(RequestTimeout, cancellationToken);
        }
        catch (TimeoutException) { throw new TimeoutException("The game did not answer the plugin request. Is the game running?"); }
        finally { pending.TryRemove(id, out _); }
    }

    /// <summary>The next request for the game page, or null when none arrives within the wait.</summary>
    public async Task<JsonObject?> NextAsync(TimeSpan wait, CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(wait);
        try
        {
            while (await requests.Reader.WaitToReadAsync(timeout.Token))
                if (requests.Reader.TryRead(out var request))
                {
                    // A request whose caller has given up is not worth running in the game.
                    if (pending.ContainsKey(request["id"]!.GetValue<long>())) return request;
                }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { }
        return null;
    }

    public void Answer(JsonObject answer)
    {
        var id = answer["id"]?.GetValue<long>() ?? throw new InvalidDataException("The answer has no request id.");
        if (!pending.TryGetValue(id, out var waiting)) return;
        if (answer["error"]?.GetValue<string>() is { } error) waiting.TrySetException(new InvalidOperationException("The game refused the plugin request: " + error));
        else waiting.TrySetResult(answer["result"]?.DeepClone());
    }
}

/// <summary>Loads a packet logger plugin and delivers captured packets to it in order.</summary>
public sealed class PacketPluginHost : IAsyncDisposable
{
    private readonly Channel<Packet> queue = Channel.CreateBounded<Packet>(new BoundedChannelOptions(10000)
        { SingleReader = true, FullMode = BoundedChannelFullMode.DropOldest });
    private readonly CancellationTokenSource stop = new();
    private readonly IPacketPlugin plugin;
    private readonly Action<string>? log;
    private readonly Task worker;
    private readonly AssemblyLoadContext context;

    public string Name { get; }

    private PacketPluginHost(IPacketPlugin plugin, string name, AssemblyLoadContext context, IGameSession game, Action<string>? log)
    {
        this.plugin = plugin;
        this.context = context;
        this.log = log;
        Name = name;
        // Started in the background: the game page that answers game requests opens only after the host is up.
        worker = Task.Run(() => RunAsync(game));
    }

    public static PacketPluginHost Load(string path, IGameSession game, Action<string>? log)
    {
        path = Path.GetFullPath(path);
        if (!File.Exists(path)) throw new FileNotFoundException("The packet logger plugin was not found: " + path, path);
        var context = new PluginLoadContext(path);
        Assembly assembly;
        try { assembly = context.LoadUnlocked(path); }
        catch (BadImageFormatException) { context.Unload(); throw new InvalidOperationException("The packet logger plugin is not a .NET assembly: " + path); }
        Type[] types;
        try { types = assembly.GetExportedTypes(); }
        catch (ReflectionTypeLoadException error) { types = error.Types.OfType<Type>().Where(type => type.IsPublic).ToArray(); }
        var type = types.FirstOrDefault(type => type is { IsClass: true, IsAbstract: false } && typeof(IPacketPlugin).IsAssignableFrom(type) &&
            type.GetConstructor(Type.EmptyTypes) is not null);
        if (type is null)
        {
            context.Unload();
            throw new InvalidOperationException($"{Path.GetFileName(path)} has no public {nameof(IPacketPlugin)} class with a parameterless constructor. " +
                $"Build it against {typeof(IPacketPlugin).Assembly.GetName().Name} {typeof(IPacketPlugin).Assembly.GetName().Version}.");
        }
        var plugin = (IPacketPlugin)Activator.CreateInstance(type)!;
        return new PacketPluginHost(plugin, type.FullName ?? type.Name, context, game, log);
    }

    public void Enqueue(PacketCapture.Entry entry) => queue.Writer.TryWrite(new Packet(entry.Id, entry.Timestamp,
        entry.Connection == "Login" ? PacketConnection.Login : PacketConnection.World,
        entry.Direction == "Send" ? PacketDirection.Send : PacketDirection.Receive, entry.Raw));

    private async Task RunAsync(IGameSession game)
    {
        try
        {
            await plugin.StartAsync(game, stop.Token);
            log?.Invoke($"Packet plugin {Name} started.");
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { return; }
        catch (Exception error) { log?.Invoke($"Packet plugin {Name} failed to start: {error.Message}"); return; }
        var failures = 0;
        try
        {
            await foreach (var packet in queue.Reader.ReadAllAsync(stop.Token))
            {
                try { plugin.OnPacket(packet); }
                catch (Exception error) when (failures++ < 10) { log?.Invoke($"Packet plugin {Name} failed on packet {packet.Id}: {error.Message}"); }
                catch (Exception) { }
            }
        }
        catch (OperationCanceledException) { }
    }

    public async ValueTask DisposeAsync()
    {
        queue.Writer.TryComplete();
        stop.Cancel();
        try { await worker.WaitAsync(TimeSpan.FromSeconds(5)); } catch { /* the plugin's own failure is already logged */ }
        try { await plugin.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5)); }
        catch (Exception error) { log?.Invoke($"Packet plugin {Name} failed to stop: {error.Message}"); }
        stop.Dispose();
        // The plugin's files were never locked; unloading lets a rebuilt plugin load fresh on the next game.
        context.Unload();
    }

    /// <summary>
    /// The plugin's own dependencies come from its folder; the contract assembly is always the launcher's, so the
    /// plugin's IPacketPlugin is the type the launcher looks for.
    /// </summary>
    private sealed class PluginLoadContext(string path) : AssemblyLoadContext("ClientLab plugin " + Path.GetFileName(path), isCollectible: true)
    {
        private static readonly string Contract = typeof(IPacketPlugin).Assembly.GetName().Name!;
        private readonly AssemblyDependencyResolver resolver = new(path);

        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == Contract) return null;
            return resolver.ResolveAssemblyToPath(name) is { } file ? LoadUnlocked(file) : null;
        }

        /// <summary>Loads from a copy in memory, with symbols when present, so the plugin can be rebuilt while the launcher runs.</summary>
        public Assembly LoadUnlocked(string file)
        {
            using var image = new MemoryStream(File.ReadAllBytes(file));
            var symbols = Path.ChangeExtension(file, ".pdb");
            if (!File.Exists(symbols)) return LoadFromStream(image);
            using var debug = new MemoryStream(File.ReadAllBytes(symbols));
            return LoadFromStream(image, debug);
        }

        protected override IntPtr LoadUnmanagedDll(string name) =>
            resolver.ResolveUnmanagedDllToPath(name) is { } file ? LoadUnmanagedDllFromPath(file) : IntPtr.Zero;
    }
}
