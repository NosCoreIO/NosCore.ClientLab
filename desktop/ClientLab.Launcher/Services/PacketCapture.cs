using System.Text;
using System.Text.Json;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>DeveloperTools-style cleartext events, with an independent durable log per game session.</summary>
public sealed class PacketCapture : IDisposable
{
    public sealed record Entry(long Id, DateTimeOffset Timestamp, string Connection, string Direction, string Raw);
    private readonly object gate = new();
    private readonly Queue<Entry> entries = new();
    private readonly StreamWriter writer;
    private long sequence;
    private bool disposed;
    public string FilePath { get; }

    public PacketCapture(string dataDirectory)
    {
        var directory = Path.Combine(dataDirectory, "packet-logs");
        Directory.CreateDirectory(directory);
        var started = DateTimeOffset.UtcNow;
        FilePath = Path.Combine(directory, $"packets-{started:yyyyMMddTHHmmss.fffZ}-{Guid.NewGuid():N}.jsonl");
        writer = new StreamWriter(new FileStream(FilePath, FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
        writer.WriteLine(JsonSerializer.Serialize(new { type = "session", started, format = "ClientLab cleartext packets", hookSource = "NosCore.DeveloperTools" }));
    }

    public Entry? Add(int kind, string raw, DateTimeOffset timestamp)
    {
        if (kind is < 0 or > 2 || raw.Length > 65536) throw new InvalidDataException("Invalid packet capture event.");
        lock (gate)
        {
            if (disposed) return null;
            var entry = new Entry(++sequence, timestamp, kind == 2 ? "Login" : "World", kind == 0 ? "Send" : "Receive", raw);
            writer.WriteLine(JsonSerializer.Serialize(entry));
            entries.Enqueue(entry);
            while (entries.Count > 10000) entries.Dequeue();
            return entry;
        }
    }

    public object Snapshot(long since)
    {
        lock (gate) return new { enabled = true, file = FilePath, cursor = sequence, entries = entries.Where(e => e.Id > since).ToArray() };
    }

    public void Dispose() { lock (gate) { if (disposed) return; disposed = true; writer.Dispose(); } }
}
