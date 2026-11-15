using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace NosCore.ClientLab.Launcher.Services;

public sealed class GameforgeIdentity
{
    private readonly JsonArray fields;
    private static readonly JsonSerializerOptions JsonOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
    public string InstallationId { get; }
    public string UserAgent => fields[26]!.GetValue<string>();

    public GameforgeIdentity(string filename, string? installationId = null)
    {
        var data = JsonNode.Parse(File.ReadAllText(filename))!.AsObject();
        InstallationId = data["installationId"]!.GetValue<string>();
        if (installationId is { Length: > 0 } && InstallationId != installationId) throw new InvalidDataException("The captured Gameforge identity belongs to a different installation.");
        fields = data["fingerprint"]!.DeepClone().AsArray();
        if (fields.Count != 30 || fields[0]!.GetValue<int>() != 12) throw new InvalidDataException("Capture a fresh version 12 Gameforge identity.");
        var vector = Encoding.ASCII.GetString(Convert.FromBase64String(fields[25]!.GetValue<string>()));
        if (!long.TryParse(vector[(vector.LastIndexOf(' ') + 1)..], out _)) throw new InvalidDataException("Invalid identity vector.");
    }

    public async Task<string> CreateAsync(HttpClient http, JsonObject? request = null)
    {
        using var message = new HttpRequestMessage(HttpMethod.Get, "https://gameforge.com/tra/game1.js");
        message.Headers.TryAddWithoutValidation("User-Agent", UserAgent);
        using var response = await http.SendAsync(message, HttpCompletionOption.ResponseHeadersRead);
        response.EnsureSuccessStatusCode();
        var serverDate = response.Headers.Date ?? throw new InvalidDataException("Gameforge did not return its server time.");
        var value = fields.DeepClone().AsArray();
        var vector = Encoding.ASCII.GetString(Convert.FromBase64String(fields[25]!.GetValue<string>()));
        var separator = vector.LastIndexOf(' '); var content = vector[..separator];
        var now = DateTimeOffset.UtcNow; var milliseconds = now.ToUnixTimeMilliseconds();
        if (milliseconds > long.Parse(vector[(separator + 1)..], CultureInfo.InvariantCulture) + 1000)
            content = content[1..] + (char)RandomNumberGenerator.GetInt32(32, 126);
        value[25] = Convert.ToBase64String(Encoding.ASCII.GetBytes(content + " " + milliseconds));
        fields[25] = value[25]!.GetValue<string>();
        value[21] = now.UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        value[27] = serverDate.UtcDateTime.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        value[28] = request?.DeepClone();
        return Encode(value);
    }

    public static string Encode(JsonArray fields)
    {
        var escaped = new StringBuilder();
        foreach (var value in Encoding.UTF8.GetBytes(fields.ToJsonString(JsonOptions)))
        {
            if (value is >= (byte)'a' and <= (byte)'z' or >= (byte)'A' and <= (byte)'Z' or >= (byte)'0' and <= (byte)'9' || "-_!~*.'()".Contains((char)value)) escaped.Append((char)value);
            else escaped.Append('%').Append(value.ToString("X2"));
        }
        var bytes = Encoding.ASCII.GetBytes(escaped.ToString()); byte previous = 0;
        for (var index = 0; index < bytes.Length; index++) bytes[index] = previous = unchecked((byte)(previous + bytes[index]));
        return "tra:" + Convert.ToBase64String(bytes).Replace('+', '-').Replace('/', '_').TrimEnd('=');
    }

    public static JsonArray Decode(string blackbox)
    {
        if (!blackbox.StartsWith("tra:", StringComparison.Ordinal)) throw new InvalidDataException("Invalid identity prefix.");
        var value = blackbox[4..].Replace('-', '+').Replace('_', '/'); value = value.PadRight((value.Length + 3) / 4 * 4, '=');
        var raw = Convert.FromBase64String(value); var decoded = new byte[raw.Length];
        for (var index = 0; index < raw.Length; index++) decoded[index] = unchecked((byte)(raw[index] - (index > 0 ? raw[index - 1] : 0)));
        return JsonNode.Parse(Uri.UnescapeDataString(Encoding.ASCII.GetString(decoded)))!.AsArray();
    }

    public static string Encrypt(string blackbox, string gsid, string accountId)
    {
        var key = Encoding.ASCII.GetBytes(Convert.ToHexStringLower(SHA512.HashData(Encoding.UTF8.GetBytes(gsid + "-" + accountId))));
        var bytes = Encoding.UTF8.GetBytes(blackbox);
        for (var index = 0; index < bytes.Length; index++) bytes[index] ^= (byte)(key[index % key.Length] ^ key[key.Length - index % key.Length - 1]);
        return Convert.ToBase64String(bytes);
    }
}
