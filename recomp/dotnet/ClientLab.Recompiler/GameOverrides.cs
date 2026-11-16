using System.Reflection;
using System.Security.Cryptography;
using System.Text;

namespace NosCore.ClientLab.Recompiler;

/// <summary>The client routines that runtime/rt_game.c replaces with hand-written C: the height ray against a triangle,
/// the height walk over a model's nodes, and the grid path search. They are used when every routine, and the division
/// helper's check the ray repeats, has the code they were written for. The fingerprint leaves out the bytes of
/// relocated operands (the absolute addresses of globals), so a build that only moved its data still qualifies, and
/// the globals rt_game.c touches are read from operands that hold them.</summary>
public static class GameOverrides
{
    private sealed record Routine(uint Start, uint End, string Fingerprint, bool Replaced);

    private static readonly Routine[] Routines =
    [
        new(0x0045d2e0, 0x0045d3e6, "075d29f6b0a1965d509cb2038ed298ed", true),
        new(0x00489ad0, 0x00489c99, "3812afb0a451ec19752c3dedbce12f87", true),
        new(0x00490264, 0x004905dd, "38d01384c4372d99966713fa6fdf26ec", true),
        // The first instruction of the floating-point division helper: cmp byte ptr [checked], 0.
        new(0x004039ec, 0x004039f3, "d7fe85fc8d1fc6a7a415b554bf859bda", false),
    ];

    private static readonly (string Name, uint Operand)[] Globals =
    [
        ("RG_FDIV_CHECKED", 0x004039ee), ("RG_QUEUES", 0x0049030a), ("RG_VISITED", 0x0049032b),
        ("RG_STEP_X", 0x004903c1), ("RG_STEP_Y", 0x004903c6), ("RG_BACK", 0x00490594),
    ];

    /// <summary>MD5 of the code from start to end with every relocated 4-byte operand zeroed.</summary>
    public static string Fingerprint(ExecutableImage image, uint start, uint end)
    {
        if (!image.ContainsCode(start) || end > image.CodeEnd || end <= start) return "";
        var bytes = image.Code.AsSpan((int)(start - image.CodeStart), (int)(end - start)).ToArray();
        foreach (var slot in image.RelocationSlots.Keys)
            if (slot >= start && slot + 4 <= end) Array.Clear(bytes, (int)(slot - start), 4);
        return Convert.ToHexStringLower(MD5.HashData(bytes));
    }

    /// <summary>The entries replaced and the C that replaces them, or none when this build differs.</summary>
    public static (HashSet<uint> Entries, string? Source, string Report) Find(ExecutableImage image)
    {
        var differing = Routines.Where(r => Fingerprint(image, r.Start, r.End) != r.Fingerprint).Select(r => $"{r.Start:x8} {Fingerprint(image, r.Start, r.End)}").ToList();
        if (differing.Count > 0) return ([], null, "hand-written routines not used, this build differs at " + string.Join(", ", differing));
        var source = new StringBuilder("/* Hand-written routines (runtime/rt_game.c) with this build's globals. */\n");
        foreach (var (name, operand) in Globals)
        {
            if (!image.RelocationSlots.TryGetValue(operand, out var value)) return ([], null, $"hand-written routines not used, no address operand at {operand:x8}");
            source.Append($"#define {name} 0x{value:x8}u\n");
        }
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("rt_game.c") ?? throw new InvalidOperationException("rt_game.c is not embedded");
        source.Append(new StreamReader(stream).ReadToEnd());
        var entries = Routines.Where(r => r.Replaced).Select(r => r.Start).ToHashSet();
        return (entries, source.ToString(), $"hand-written routines used for {string.Join(", ", entries.Select(e => e.ToString("x8")))}");
    }
}
