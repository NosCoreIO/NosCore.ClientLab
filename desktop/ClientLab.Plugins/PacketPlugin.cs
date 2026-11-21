namespace NosCore.ClientLab.Plugins;

/// <summary>Which way a packet travels.</summary>
public enum PacketDirection
{
    /// <summary>From the client to the server.</summary>
    Send,
    /// <summary>From the server to the client.</summary>
    Receive
}

/// <summary>Which server connection carried a packet.</summary>
public enum PacketConnection
{
    /// <summary>The world (channel) server.</summary>
    World,
    /// <summary>The login server.</summary>
    Login
}

/// <summary>A cleartext packet captured at the client's own send/receive boundary.</summary>
public sealed record Packet(long Id, DateTimeOffset Timestamp, PacketConnection Connection, PacketDirection Direction, string Raw);

/// <summary>
/// A packet logger chosen in ClientLab's settings instead of the built-in packet window. ClientLab loads the
/// assembly into the launcher process (it is not injected into the game: the game runs as WebAssembly) and
/// creates the first public non-abstract type implementing this interface through its parameterless constructor.
/// </summary>
public interface IPacketPlugin : IAsyncDisposable
{
    /// <summary>Called once when the game host starts, before any packet is delivered.</summary>
    Task StartAsync(IGameSession game, CancellationToken cancellationToken);

    /// <summary>
    /// Called for every captured packet, in capture order, on one background thread. Keep it short: packets
    /// wait in a queue while this runs. Exceptions are logged and do not stop later packets.
    /// </summary>
    void OnPacket(Packet packet);
}

/// <summary>
/// The running game, as seen from a plugin. Addresses are the client's own 32-bit virtual addresses: the
/// recompiled client keeps its data at the addresses the original executable would use.
///
/// The client's code was compiled ahead of time, so writing over code bytes does not change what runs. Data
/// writes and function calls do. Every memory and call request runs on the game thread between window
/// messages, the same place the client's own input handlers run.
/// </summary>
public interface IGameSession
{
    /// <summary>The original client executable this game was compiled from.</summary>
    string ClientExecutable { get; }

    /// <summary>The JSONL file this session's packets are also written to.</summary>
    string PacketLogFile { get; }

    /// <summary>Writes a line to ClientLab's activity log.</summary>
    void Log(string message);

    /// <summary>
    /// Finds a byte pattern in the client executable's code ("53 56 8B ?? 8B D8", '?' or '??' for any byte)
    /// and returns its virtual address, or null unless it matches exactly once.
    /// </summary>
    uint? FindCode(string pattern);

    /// <summary>Reads guest memory.</summary>
    Task<byte[]> ReadAsync(uint address, int length, CancellationToken cancellationToken = default);

    /// <summary>Writes guest memory.</summary>
    Task WriteAsync(uint address, ReadOnlyMemory<byte> data, CancellationToken cancellationToken = default);

    /// <summary>
    /// Calls a client function with Delphi's register convention: EAX, EDX and ECX, then at most three
    /// arguments pushed left to right. Returns EAX.
    /// </summary>
    Task<uint> CallAsync(uint function, uint eax = 0, uint edx = 0, uint ecx = 0, IReadOnlyList<uint>? stack = null,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Hands a world packet to the client's own send function, as if the client had sent it. Returns false
    /// until the client has sent a world packet of its own (the function's object is taken from that call).
    /// </summary>
    Task<bool> SendPacketAsync(string packet, CancellationToken cancellationToken = default);

    /// <summary>
    /// Hands a world packet to the client's own receive handler, as if the server had sent it. Returns false
    /// until the client has received a world packet.
    /// </summary>
    Task<bool> ReceivePacketAsync(string packet, CancellationToken cancellationToken = default);
}
