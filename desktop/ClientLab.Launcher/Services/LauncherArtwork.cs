namespace NosCore.ClientLab.Launcher.Services;

internal static class LauncherArtwork
{
    // Artwork referenced by https://play.nostale.gameforge.com/. Kept remote, never embedded in releases.
    public const string Url = "https://cdn.lugc.link/b65f8da0-24e2-41be-a3a2-f07ad32a8f51/-/resize/1200x/-/format/jpeg/";

    public static async Task<byte[]?> LoadAsync(string dataDirectory)
    {
        var file = Path.Combine(dataDirectory, "artwork", "nostale-official.jpg");
        byte[]? cached = null;
        try
        {
            if (File.Exists(file))
            {
                cached = await File.ReadAllBytesAsync(file);
                if (File.GetLastWriteTimeUtc(file) > DateTime.UtcNow.AddDays(-7)) return cached;
            }
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10), MaxResponseContentBufferSize = 8 * 1024 * 1024 };
            using var response = await http.GetAsync(Url);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentType?.MediaType != "image/jpeg") return cached;
            var image = await response.Content.ReadAsByteArrayAsync();
            if (image.Length < 4 || image[0] != 0xFF || image[1] != 0xD8) return cached;
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            var temporary = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
            await File.WriteAllBytesAsync(temporary, image);
            File.Move(temporary, file, overwrite: true);
            return image;
        }
        catch (Exception) { return cached; } // Artwork availability never blocks sign-in, updates, or play.
    }
}
