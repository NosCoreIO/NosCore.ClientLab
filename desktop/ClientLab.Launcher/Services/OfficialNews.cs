using System.Globalization;
using System.Text.Json.Nodes;
using System.Security.Cryptography;
using System.Text;

namespace NosCore.ClientLab.Launcher.Services;

public sealed record NewsEntry(string Title, string Url, string Category, string Date, string? ImageUrl = null);

public static class OfficialNews
{
    public const string Home = "https://gameforge.com/en-GB/play/nostale";
    private const string Feed = "https://gameforge.com/api/v1/product/nostale/news/en_GB";
    public const string FallbackImage = "https://secure-asset-delivery.gameforge.com/partnersite_live_product/81854f0b-0698-4507-bcae-59b909e2f1f0/2b180b68-0146-470d-8950-691523742b342025-08-14_NT_Steam_SM_Generic_Assets_NT_TNT_card_616x353.jpg";

    public static async Task<NewsEntry[]> LoadAsync(string directory)
    {
        var cache = Path.Combine(directory, "news", "official-product.json");
        string? xml = null;
        try
        {
            if (File.Exists(cache) && File.GetLastWriteTimeUtc(cache) > DateTime.UtcNow.AddHours(-1)) xml = await File.ReadAllTextAsync(cache);
            else
            {
                using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8), MaxResponseContentBufferSize = 2 * 1024 * 1024 };
                xml = await http.GetStringAsync(Feed);
                var entries = Parse(xml); // Validate before replacing the offline cache.
                Directory.CreateDirectory(Path.GetDirectoryName(cache)!);
                await File.WriteAllTextAsync(cache, xml);
                return entries;
            }
        }
        catch (Exception)
        {
            try { if (File.Exists(cache)) xml = await File.ReadAllTextAsync(cache); } catch (IOException) { }
        }
        try { if (xml is not null) return Parse(xml); } catch (Exception) { }
        return [new("Latest news & events from NosTale", Home, "OFFICIAL NEWS", "", FallbackImage)];
    }

    public static NewsEntry[] Parse(string json)
    {
        var items = JsonNode.Parse(json)?.AsArray() ?? throw new InvalidDataException("Invalid product news feed.");
        return items.OfType<JsonObject>().Where(item => item["showInLibrary"]?.GetValue<int>() != 0).Select(item =>
        {
            var title = item["title"]?.GetValue<string>().Trim() ?? "";
            var id = item["id"]?.GetValue<string>() ?? "";
            var link = item["link"]?.GetValue<string>() ?? "";
            var url = IsOfficialLink(link) ? link : Guid.TryParse(id, out _) ? Home + "/news/" + id : Home;
            var date = DateTimeOffset.TryParse(item["publishDate"]?.GetValue<string>(), CultureInfo.InvariantCulture, DateTimeStyles.AllowWhiteSpaces, out var parsed) ? parsed.ToString("dd MMM", CultureInfo.InvariantCulture) : "";
            var category = item["showAsEvent"]?.GetValue<int>() == 1 || title.Contains("event", StringComparison.OrdinalIgnoreCase) ? "EVENT" : "NEWS";
            var image = item["imageSmall"]?.GetValue<string>();
            return new NewsEntry(title.Length > 180 ? title[..180] : title, url, category, date, IsOfficialImage(image) ? image : FallbackImage);
        }).Where(entry => entry.Title.Length > 0).DistinctBy(entry => entry.Url).ToArray();
    }

    public static async Task<byte[]?> LoadImageAsync(NewsEntry entry, string directory, bool useFallback = true)
    {
        if (!IsOfficialImage(entry.ImageUrl)) return useFallback ? await LoadImageAsync(entry with { ImageUrl = FallbackImage }, directory, false) : null;
        var file = Path.Combine(directory, "news", Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(entry.ImageUrl!)))) + ".image";
        try
        {
            if (File.Exists(file)) return await File.ReadAllBytesAsync(file);
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8), MaxResponseContentBufferSize = 4 * 1024 * 1024 };
            using var response = await http.GetAsync(entry.ImageUrl);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentType?.MediaType?.StartsWith("image/", StringComparison.Ordinal) != true) return null;
            var bytes = await response.Content.ReadAsByteArrayAsync();
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            var temporary = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
            await File.WriteAllBytesAsync(temporary, bytes); File.Move(temporary, file, overwrite: true);
            return bytes;
        }
        catch (Exception) { return useFallback && entry.ImageUrl != FallbackImage ? await LoadImageAsync(entry with { ImageUrl = FallbackImage }, directory, false) : null; }
    }

    private static bool IsOfficialImage(string? url) => Uri.TryCreate(url, UriKind.Absolute, out var uri) && uri.Scheme == "https" &&
        (uri.Host.EndsWith(".gameforge.com", StringComparison.Ordinal) || uri.Host.EndsWith(".gfsrv.net", StringComparison.Ordinal) || uri.Host == "cdn.lugc.link");

    public static bool IsOfficialLink(string url) => Uri.TryCreate(url, UriKind.Absolute, out var uri) &&
        uri.Scheme == "https" && (uri.Host == "forum.nostale.gameforge.com" || uri.Host == "gameforge.com" && uri.AbsolutePath.StartsWith("/en-GB/play/nostale", StringComparison.Ordinal));
}
