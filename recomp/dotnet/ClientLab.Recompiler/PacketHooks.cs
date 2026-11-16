namespace NosCore.ClientLab.Recompiler;

/// <summary>Cleartext capture sites from NosCore.DeveloperTools.Hook/Signatures.cs.</summary>
public static class PacketHooks
{
    public static IReadOnlyDictionary<uint, int> Find(ExecutableImage image)
    {
        var result = new Dictionary<uint, int>();
        // Match uniquely: ambiguous or outdated signatures never instrument arbitrary code.
        var signatures = new[] {
            "53568BF28BD8EB04", // World Send: EDX is the Delphi packet string.
            "558BEC83C4F053565733C9894DF4894DF08955FC8BD88B45FC", // World Receive: EDX.
            "8D45F4508D55F8B1208B45F8" // Login Receive: [EBP-8], before tokenisation.
        };
        for (var kind = 0; kind < signatures.Length; kind++)
        {
            var pattern = Convert.FromHexString(signatures[kind]);
            var matches = new List<int>();
            for (var offset = 0; offset <= image.Code.Length - pattern.Length;)
            {
                var relative = image.Code.AsSpan(offset).IndexOf(pattern);
                if (relative < 0) break;
                var match = offset + relative;
                // DeveloperTools' login recipe additionally requires the immediately
                // preceding cmp [ebp-8],0 / jz exit. Other tokenizer loops share the
                // short LEA sequence but are not packet boundaries.
                if (kind != 2 || match >= 10 && image.Code.AsSpan(match - 10, 6).SequenceEqual(Convert.FromHexString("837DF8000F84")))
                    matches.Add(match);
                offset = match + 1;
            }
            if (matches.Count == 1) result[image.CodeStart + (uint)matches[0]] = kind;
        }
        return result;
    }
}
