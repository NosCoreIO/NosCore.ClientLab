using System.Text;
using System.Text.RegularExpressions;

namespace NosCore.ClientLab.Launcher.Services;

public sealed record GameOptions(bool Music = true, bool SoundEffects = true, bool SpatialSound = true, bool FullScreen = false);

internal static class GameOptionsIni
{
    private sealed record Line(string Text, string Ending);

    public static GameOptions Read(byte[]? bytes)
    {
        var (text, _, _) = Decode(bytes ?? []);
        bool Flag(string section, string key, bool fallback)
        {
            var current = "";
            foreach (var line in Lines(text))
            {
                if (Section(line.Text) is { } name) { current = name; continue; }
                if (!current.Equals(section, StringComparison.OrdinalIgnoreCase)) continue;
                var match = Regex.Match(line.Text, @"^\s*" + Regex.Escape(key) + @"\s*=\s*([^;]*)", RegexOptions.IgnoreCase);
                if (!match.Success) continue;
                return match.Groups[1].Value.Trim().Trim('"', '\'').ToLowerInvariant() switch {
                    "1" or "true" or "yes" => true, "0" or "false" or "no" => false, _ => fallback };
            }
            return fallback;
        }
        return new(!Flag("EW_Sound_Config", "BGMSoundOff", false), !Flag("EW_Sound_Config", "FxSoundOff", false),
            !Flag("EW_Sound_Config", "3DSoundOff", false), Flag("EW_Multi_Navigator", "FullScreen", false));
    }

    public static byte[] Update(byte[]? bytes, GameOptions options)
    {
        var (text, encoding, preamble) = Decode(bytes ?? []);
        var lines = Lines(text);
        var newline = lines.FirstOrDefault(line => line.Ending.Length > 0)?.Ending ?? "\r\n";
        void Set(string section, string key, bool value)
        {
            var start = lines.FindIndex(line => string.Equals(Section(line.Text), section, StringComparison.OrdinalIgnoreCase));
            if (start < 0)
            {
                if (lines.Count > 0 && lines[^1].Ending.Length == 0) lines[^1] = lines[^1] with { Ending = newline };
                lines.Add(new("[" + section + "]", newline)); start = lines.Count - 1;
            }
            var end = start + 1;
            for (; end < lines.Count && Section(lines[end].Text) is null; end++)
            {
                var match = Regex.Match(lines[end].Text, @"^([\t ]*" + Regex.Escape(key) + @"[\t ]*=[\t ]*)([^;]*)(;.*)?$", RegexOptions.IgnoreCase);
                if (!match.Success) continue;
                var spaces = Regex.Match(match.Groups[2].Value, @"[\t ]*$").Value;
                lines[end] = lines[end] with { Text = match.Groups[1].Value + (value ? "1" : "0") + spaces + match.Groups[3].Value };
                return;
            }
            if (end > 0 && lines[end - 1].Ending.Length == 0) lines[end - 1] = lines[end - 1] with { Ending = newline };
            lines.Insert(end, new(key + "=" + (value ? "1" : "0"), newline));
        }
        Set("EW_Sound_Config", "BGMSoundOff", !options.Music);
        Set("EW_Sound_Config", "FxSoundOff", !options.SoundEffects);
        Set("EW_Sound_Config", "3DSoundOff", !options.SpatialSound);
        Set("EW_Multi_Navigator", "FullScreen", options.FullScreen);
        return preamble.Concat(encoding.GetBytes(string.Concat(lines.Select(line => line.Text + line.Ending)))).ToArray();
    }

    private static List<Line> Lines(string text) => Regex.Matches(text, @"([^\r\n]*)(\r\n|\r|\n|$)")
        .Where(match => match.Length > 0).Select(match => new Line(match.Groups[1].Value, match.Groups[2].Value)).ToList();
    private static string? Section(string line)
    {
        var match = Regex.Match(line, @"^\s*\[\s*([^\]]+?)\s*\]");
        return match.Success ? match.Groups[1].Value : null;
    }
    private static (string Text, Encoding Encoding, byte[] Preamble) Decode(byte[] bytes)
    {
        // Without a BOM, Latin1 provides a lossless byte mapping for both legacy
        // ANSI and UTF-8 data while the edited section/key/value text is ASCII.
        foreach (var encoding in new Encoding[] { Encoding.UTF8, Encoding.Unicode, Encoding.BigEndianUnicode })
        {
            var preamble = encoding.GetPreamble();
            if (bytes.AsSpan().StartsWith(preamble)) return (encoding.GetString(bytes, preamble.Length, bytes.Length - preamble.Length), encoding, preamble);
        }
        return (Encoding.Latin1.GetString(bytes), Encoding.Latin1, []);
    }
}
