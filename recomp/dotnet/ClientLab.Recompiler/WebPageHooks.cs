namespace NosCore.ClientLab.Recompiler;

/// <summary>
/// The in-game web page window (NosMall and the other pages it hosts). With the Gameforge launcher the client shows
/// these in its own window through an embedded Chromium (libcef), which the recompiled client cannot load; with Steam
/// it opens the same address in the Steam overlay instead. The hook goes at the entry of the window's "shown" handler
/// and hands the window's address to the game page, which draws the web page over the window's browser area (the
/// TCEFWindowParent child the window manager reports), as the embedded Chromium would.
/// </summary>
public static class WebPageHooks
{
    // TNTNosMallTotalWidget's shown handler: frame setup, then "if Steam and the overlay is enabled, hide the frame".
    private const string Shown =
        "558BEC6A0053568BD833C055 68???????? 64FF30 648920 A1???????? 803801 75?? E8???????? 3C01 75?? 33D2 8B83????????";
    // Later in the same handler: "if Trim(FUrl) = '' then request a new address", which gives FUrl's offset.
    private const string UrlField = "8D55FC 8B83 ???????? E8";

    /// <summary>The shown handler's address and the offset of the window's address field, or nothing unless unique.</summary>
    public static IReadOnlyDictionary<uint, uint> Find(ExecutableImage image)
    {
        var result = new Dictionary<uint, uint>();
        var shown = Parse(Shown);
        var url = Parse(UrlField);
        var matches = Matches(image.Code, shown, 0, image.Code.Length).ToList();
        if (matches.Count != 1) return result;
        var field = Matches(image.Code, url, matches[0], Math.Min(image.Code.Length, matches[0] + 0x180)).ToList();
        if (field.Count == 0) return result;
        var offset = BitConverter.ToUInt32(image.Code, field[0] + 5);
        if (offset is 0 or > 0x10000) return result;
        result[image.CodeStart + (uint)matches[0]] = offset;
        return result;
    }

    private static short[] Parse(string pattern)
    {
        var compact = pattern.Replace(" ", "");
        return [.. Enumerable.Range(0, compact.Length / 2).Select(i => compact.Substring(2 * i, 2))
            .Select(pair => pair == "??" ? (short)-1 : Convert.ToInt16(pair, 16))];
    }

    private static IEnumerable<int> Matches(byte[] code, short[] pattern, int from, int to)
    {
        for (var at = from; at + pattern.Length <= to; at++)
        {
            var hit = true;
            for (var k = 0; k < pattern.Length && hit; k++) hit = pattern[k] < 0 || code[at + k] == pattern[k];
            if (hit) yield return at;
        }
    }
}
