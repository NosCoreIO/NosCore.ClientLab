using System.Buffers.Binary;

namespace NosCore.ClientLab.Launcher.Services;

/// <summary>
/// Minimal read-only LevelDB reader for Chromium Local Storage: walks the write-ahead <c>.log</c> files and the
/// Snappy-compressed <c>.ldb</c> tables of one database directory and returns, for every key, the value with the
/// highest sequence number. Compressed tables split keys and values across back-references, so a plain byte scan
/// misses entries that a later compaction rewrote; reading the records properly avoids falling back to stale ones.
/// </summary>
public static class LevelDbReader
{
    private const ulong TableMagic = 0xdb4775248b80fb57UL;

    public readonly record struct Entry(ulong Sequence, byte[]? Value);

    /// <summary>Latest live value per key across the given files (deleted keys are dropped).</summary>
    public static Dictionary<string, byte[]> ReadLatest(IEnumerable<(string Name, byte[] Data)> files)
    {
        var latest = new Dictionary<string, Entry>(StringComparer.Ordinal);
        void Offer(byte[] key, ulong sequence, byte[]? value)
        {
            var text = Latin1(key);
            if (!latest.TryGetValue(text, out var current) || sequence >= current.Sequence) latest[text] = new Entry(sequence, value);
        }
        foreach (var (name, data) in files)
        {
            try
            {
                if (name.EndsWith(".log", StringComparison.OrdinalIgnoreCase)) ReadLog(data, Offer);
                else if (name.EndsWith(".ldb", StringComparison.OrdinalIgnoreCase) || name.EndsWith(".sst", StringComparison.OrdinalIgnoreCase)) ReadTable(data, Offer);
            }
            catch (Exception error) when (error is InvalidDataException or IndexOutOfRangeException or ArgumentException or OverflowException) { }
        }
        return latest.Where(pair => pair.Value.Value is not null).ToDictionary(pair => pair.Key, pair => pair.Value.Value!, StringComparer.Ordinal);
    }

    public static string Latin1(ReadOnlySpan<byte> bytes) => System.Text.Encoding.Latin1.GetString(bytes);

    private static void ReadLog(byte[] data, Action<byte[], ulong, byte[]?> offer)
    {
        const int BlockSize = 32768;
        var record = new List<byte>();
        var position = 0;
        while (position + 7 <= data.Length)
        {
            var leftInBlock = BlockSize - position % BlockSize;
            if (leftInBlock < 7) { position += leftInBlock; continue; }
            var length = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(position + 4));
            var type = data[position + 6];
            position += 7;
            if (type == 0 && length == 0) { position += leftInBlock - 7; continue; } // preallocated zero padding
            if (position + length > data.Length) break;
            var fragment = data.AsSpan(position, length);
            position += length;
            switch (type)
            {
                case 1: ReadBatch(fragment.ToArray(), offer); record.Clear(); break;
                case 2: record.Clear(); record.AddRange(fragment.ToArray()); break;
                case 3: record.AddRange(fragment.ToArray()); break;
                case 4: record.AddRange(fragment.ToArray()); ReadBatch(record.ToArray(), offer); record.Clear(); break;
            }
        }
    }

    private static void ReadBatch(byte[] batch, Action<byte[], ulong, byte[]?> offer)
    {
        if (batch.Length < 12) return;
        var sequence = BinaryPrimitives.ReadUInt64LittleEndian(batch);
        var count = BinaryPrimitives.ReadUInt32LittleEndian(batch.AsSpan(8));
        var position = 12;
        for (var index = 0u; index < count && position < batch.Length; index++, sequence++)
        {
            var tag = batch[position++];
            var key = ReadSlice(batch, ref position);
            byte[]? value = null;
            if (tag == 1) value = ReadSlice(batch, ref position);
            else if (tag != 0) return;
            offer(key, sequence, value);
        }
    }

    private static void ReadTable(byte[] data, Action<byte[], ulong, byte[]?> offer)
    {
        if (data.Length < 48 || BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(data.Length - 8)) != TableMagic) return;
        var footer = data.Length - 48;
        ReadVarint(data, ref footer); ReadVarint(data, ref footer); // metaindex handle
        var indexOffset = (int)ReadVarint(data, ref footer); var indexSize = (int)ReadVarint(data, ref footer);
        foreach (var (_, handle) in ReadBlock(Block(data, indexOffset, indexSize)))
        {
            var cursor = 0;
            var offset = (int)ReadVarint(handle, ref cursor); var size = (int)ReadVarint(handle, ref cursor);
            foreach (var (internalKey, value) in ReadBlock(Block(data, offset, size)))
            {
                if (internalKey.Length < 8) continue;
                var tag = BinaryPrimitives.ReadUInt64LittleEndian(internalKey.AsSpan(internalKey.Length - 8));
                offer(internalKey[..^8], tag >> 8, (tag & 0xff) == 1 ? value : null);
            }
        }
    }

    private static byte[] Block(byte[] data, int offset, int size)
    {
        if (offset < 0 || size < 0 || offset + size + 5 > data.Length) throw new InvalidDataException("Block outside table.");
        var contents = data.AsSpan(offset, size);
        return data[offset + size] switch
        {
            0 => contents.ToArray(),
            1 => Snappy(contents),
            _ => throw new InvalidDataException("Unsupported block compression."),
        };
    }

    private static IEnumerable<(byte[] Key, byte[] Value)> ReadBlock(byte[] block)
    {
        if (block.Length < 4) yield break;
        var restarts = (int)BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(block.Length - 4));
        var end = block.Length - 4 - restarts * 4;
        var position = 0; var key = Array.Empty<byte>();
        while (position < end)
        {
            var shared = (int)ReadVarint(block, ref position);
            var unshared = (int)ReadVarint(block, ref position);
            var valueLength = (int)ReadVarint(block, ref position);
            var next = new byte[shared + unshared];
            key.AsSpan(0, shared).CopyTo(next);
            block.AsSpan(position, unshared).CopyTo(next.AsSpan(shared));
            position += unshared;
            key = next;
            yield return (key, block.AsSpan(position, valueLength).ToArray());
            position += valueLength;
        }
    }

    /// <summary>Raw Snappy block decompression (no framing), as used by LevelDB tables.</summary>
    public static byte[] Snappy(ReadOnlySpan<byte> input)
    {
        var position = 0;
        var length = (int)ReadVarint(input, ref position);
        var output = new byte[length]; var written = 0;
        while (position < input.Length)
        {
            var tag = input[position++];
            int count, offset;
            switch (tag & 3)
            {
                case 0:
                    count = tag >> 2;
                    if (count >= 60)
                    {
                        var bytes = count - 59; count = 0;
                        for (var index = 0; index < bytes; index++) count |= input[position++] << (8 * index);
                    }
                    count++;
                    input.Slice(position, count).CopyTo(output.AsSpan(written));
                    position += count; written += count;
                    continue;
                case 1: count = ((tag >> 2) & 7) + 4; offset = ((tag >> 5) << 8) | input[position++]; break;
                case 2: count = (tag >> 2) + 1; offset = BinaryPrimitives.ReadUInt16LittleEndian(input[position..]); position += 2; break;
                default: count = (tag >> 2) + 1; offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(input[position..]); position += 4; break;
            }
            if (offset <= 0 || offset > written) throw new InvalidDataException("Invalid Snappy back-reference.");
            for (var index = 0; index < count; index++, written++) output[written] = output[written - offset]; // may overlap
        }
        if (written != length) throw new InvalidDataException("Snappy length mismatch.");
        return output;
    }

    private static byte[] ReadSlice(byte[] data, ref int position)
    {
        var length = (int)ReadVarint(data, ref position);
        var slice = data.AsSpan(position, length).ToArray();
        position += length;
        return slice;
    }

    private static ulong ReadVarint(ReadOnlySpan<byte> data, ref int position)
    {
        ulong value = 0;
        for (var shift = 0; shift < 64; shift += 7)
        {
            var current = data[position++];
            value |= (ulong)(current & 0x7f) << shift;
            if ((current & 0x80) == 0) return value;
        }
        throw new InvalidDataException("Varint too long.");
    }
}
